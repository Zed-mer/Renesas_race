#!/usr/bin/env python3
"""Merge RA8P1 SDR sender/runtime evidence into a deterministic acceptance report.

The tool is deliberately offline.  It never accesses J-Link, the board, or the
SDR.  Every hardware claim must already be present in one of the input files.
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


TOOL_VERSION = "1.1"
EXPECTED_CENTERS = {
    0: 2_420_000_000,
    1: 2_464_000_000,
    2: 5_760_000_000,
    3: 5_816_000_000,
}
EXPECTED_SAMPLES = 6_000_000
EXPECTED_PAYLOAD_BYTES = 24_000_000
EXPECTED_TILES = 19
EXPECTED_STRIDE = 295_168
EXPECTED_WINDOW_SAMPLES = 590_336
EXPECTED_STFT_FRAMES = 1_152
EXPECTED_SOURCE_SAMPLE_RATE_HZ = 60_000_000
EXPECTED_WINDOW_PAYLOAD_BYTES = EXPECTED_WINDOW_SAMPLES * 4
WINDOW_RF_SPAN_MS = EXPECTED_WINDOW_SAMPLES * 1000.0 / EXPECTED_SOURCE_SAMPLE_RATE_HZ
NPU_PROOF_PASS_MAGIC = 0x4E5055A5
NPU_BENCHMARK_PASS_MAGIC = 0x4E5042A5
SHA256_RE = re.compile(r"^[0-9A-F]{64}$")


class EvidenceError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def as_int(value: Any, label: str) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text, 0)
        except ValueError:
            if text.isdigit():
                return int(text, 10)
    raise EvidenceError(f"{label} is not an integer: {value!r}")


def bool_value(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def field(obj: Any, path: str, default: Any = None) -> Any:
    current = obj
    for name in path.split("."):
        if not isinstance(current, dict) or name not in current:
            return default
        current = current[name]
    return current


def first_field(obj: Any, paths: Iterable[str], default: Any = None) -> Any:
    for path in paths:
        value = field(obj, path, None)
        if value is not None:
            return value
    return default


def normalize_hash(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise EvidenceError(f"{label} SHA-256 is missing")
    result = value.strip().upper()
    if not SHA256_RE.fullmatch(result):
        raise EvidenceError(f"{label} SHA-256 is invalid: {value!r}")
    return result


def expand_paths(values: Iterable[str], label: str) -> list[Path]:
    paths: list[Path] = []
    seen: set[str] = set()
    for raw in values:
        matches = [Path(item) for item in glob.glob(raw)]
        if not matches and Path(raw).is_file():
            matches = [Path(raw)]
        if not matches:
            raise EvidenceError(f"{label} path did not match a file: {raw}")
        for path in matches:
            resolved = str(path.resolve()).lower()
            if resolved not in seen:
                seen.add(resolved)
                paths.append(path.resolve())
    return sorted(paths, key=lambda item: str(item).lower())


def read_text_file(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise EvidenceError(f"cannot read {path}: {exc}") from exc
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        # Windows PowerShell 5 redirection commonly emits UTF-16LE without a
        # useful extension.  Rejecting that would make the documented capture
        # commands non-reproducible on the primary host platform.
        try:
            return data.decode("utf-16-le")
        except UnicodeDecodeError as exc:
            raise EvidenceError(f"cannot decode {path} as UTF-8 or UTF-16LE") from exc


def load_json_documents(paths: list[Path], label: str) -> list[dict[str, Any]]:
    documents: list[dict[str, Any]] = []
    for path in paths:
        try:
            payload = json.loads(read_text_file(path))
        except json.JSONDecodeError as exc:
            raise EvidenceError(f"cannot read {label} JSON {path}: {exc}") from exc
        entries = payload if isinstance(payload, list) else [payload]
        if not entries or not all(isinstance(item, dict) for item in entries):
            raise EvidenceError(f"{label} JSON must contain an object or object array: {path}")
        for index, entry in enumerate(entries):
            copy = dict(entry)
            copy["_evidence_path"] = str(path)
            copy["_evidence_index"] = index
            documents.append(copy)
    return documents


def parse_sender_logs(paths: list[Path]) -> tuple[list[dict[str, Any]], list[str]]:
    sessions: list[dict[str, Any]] = []
    ignored: list[str] = []
    required = {
        "session", "center_index", "center_hz", "samples", "data_packets",
        "udp_packets", "target_mbps", "payload_mbps_x1000", "tiles", "stride",
    }
    for path in paths:
        try:
            lines = read_text_file(path).splitlines()
        except EvidenceError as exc:
            raise EvidenceError(f"cannot read sender log {path}: {exc}") from exc
        for line_number, raw_line in enumerate(lines, 1):
            line = raw_line.strip()
            match = re.match(r"^(sent|dry-run)\s+(.*)$", line)
            if not match:
                continue
            values: dict[str, str] = {}
            for key, value in re.findall(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)", match.group(2)):
                values[key] = value
            missing = sorted(required - values.keys())
            if missing:
                ignored.append(f"{path}:{line_number}: sender record missing {', '.join(missing)}")
                continue
            record: dict[str, Any] = {
                "mode": match.group(1),
                "evidence_path": str(path),
                "line": line_number,
            }
            for key in required:
                record[key] = as_int(values[key], f"{path}:{line_number}:{key}")
            sessions.append(record)
    if not sessions:
        raise EvidenceError("no complete 'sent session=...' records were found in sender logs")
    return sessions, ignored


def check_sender_contract(record: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    session = record["session"]
    center = record["center_index"]
    expected_hz = EXPECTED_CENTERS.get(center)
    if record["mode"] != "sent":
        failures.append("dry-run is not hardware transmission evidence")
    if session <= 0:
        failures.append("session ID must be nonzero")
    if expected_hz is None:
        failures.append(f"unexpected center_index={center}")
    elif record["center_hz"] != expected_hz:
        failures.append(f"center {center} frequency is {record['center_hz']}, expected {expected_hz}")
    if record["samples"] != EXPECTED_SAMPLES:
        failures.append(f"samples={record['samples']}, expected {EXPECTED_SAMPLES}")
    if record["tiles"] != EXPECTED_TILES:
        failures.append(f"tiles={record['tiles']}, expected {EXPECTED_TILES}")
    if record["stride"] != EXPECTED_STRIDE:
        failures.append(f"stride={record['stride']}, expected {EXPECTED_STRIDE}")
    if record["data_packets"] <= 0:
        failures.append("data packet count is not positive")
    if record["udp_packets"] != record["data_packets"] + 2:
        failures.append("UDP packet count is not data_packets + START + END")
    if record["target_mbps"] <= 0:
        failures.append("target rate is not positive")
    if record["payload_mbps_x1000"] <= 0:
        failures.append("measured sender payload rate is not positive")
    return failures


def cpu0_hash_from(document: dict[str, Any], label: str) -> str:
    value = first_field(document, ("Elf.Cpu0.Sha256", "Elf.Sha256", "Cpu0Elf.Sha256"))
    return normalize_hash(value, label)


def cpu1_hash_from(document: dict[str, Any], label: str) -> str:
    value = first_field(document, ("Elf.Cpu1.Sha256", "Cpu1Elf.Sha256"))
    return normalize_hash(value, label)


def collect_net_candidates(documents: list[dict[str, Any]]) -> dict[int, list[dict[str, Any]]]:
    result: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for document in documents:
        snapshots = document.get("Snapshots")
        if not isinstance(snapshots, list) or not snapshots:
            raise EvidenceError(f"network evidence has no Snapshots array: {document['_evidence_path']}")
        for snapshot in snapshots:
            if not isinstance(snapshot, dict):
                continue
            raw_session = field(snapshot, "Iq.SessionId", 0)
            try:
                session = as_int(raw_session, "network Iq.SessionId")
            except EvidenceError:
                continue
            if session > 0:
                result[session].append({"document": document, "snapshot": snapshot})
    return result


def evaluate_net(record: dict[str, Any], candidates: list[dict[str, Any]]) -> dict[str, Any]:
    if not candidates:
        return {"state": "missing", "failures": ["no CPU0 network snapshot for session"], "warnings": []}
    completed: list[dict[str, Any]] = []
    for candidate in candidates:
        iq = candidate["snapshot"].get("Iq", {})
        try:
            active = as_int(iq.get("Active", 1), "Iq.Active")
            payload = as_int(iq.get("PayloadBytes", -1), "Iq.PayloadBytes")
        except EvidenceError:
            continue
        if active == 0 and payload == EXPECTED_PAYLOAD_BYTES:
            completed.append(candidate)
    selected = completed[-1] if completed else candidates[-1]
    snapshot = selected["snapshot"]
    iq = snapshot.get("Iq", {})
    ring = snapshot.get("Ring", {})
    phy = snapshot.get("Phy", {})
    rmac = snapshot.get("Rmac", {})
    failures: list[str] = []
    warnings: list[str] = []

    def require_int(container: dict[str, Any], name: str, expected: int) -> None:
        try:
            actual = as_int(container.get(name), name)
        except EvidenceError as exc:
            failures.append(str(exc))
            return
        if actual != expected:
            failures.append(f"{name}={actual}, expected {expected}")

    if not bool_value(iq.get("Initialized", False)):
        failures.append("IQ fast statistics were not initialized")
    require_int(iq, "Active", 0)
    require_int(iq, "SessionId", record["session"])
    require_int(iq, "Packets", record["data_packets"])
    require_int(iq, "PayloadBytes", EXPECTED_PAYLOAD_BYTES)
    for name in ("SequenceGaps", "Reordered", "Invalid"):
        require_int(iq, name, 0)
    for name in ("FullDrops", "OversizeDrops"):
        require_int(ring, name, 0)
    # CRC backend/timing fields were added in schema v6.  Keep older evidence
    # readable, but surface their absence explicitly instead of presenting a
    # software-only or untimed result as a hardware measurement.
    crc_backend = iq.get("CrcBackend")
    crc_hw_self_test = iq.get("CrcHardwareSelfTest")
    crc_timing_flags = iq.get("CrcTimingFlags")
    crc_timing_warning = None
    if crc_backend is None or crc_hw_self_test is None:
        crc_timing_warning = "CRC backend/self-test fields are absent (pre-v6 evidence)"
    else:
        try:
            if as_int(crc_backend, "CrcBackend") == 2 and as_int(crc_hw_self_test, "CrcHardwareSelfTest") != 1:
                failures.append("CrcBackend=2 without a passing hardware self-test")
            if as_int(crc_backend, "CrcBackend") != 2:
                warnings.append(f"CRC backend={as_int(crc_backend, 'CrcBackend')} (hardware backend 2 not proven)")
        except EvidenceError as exc:
            warnings.append(str(exc))
    if crc_timing_flags is not None:
        try:
            if (as_int(crc_timing_flags, "CrcTimingFlags") & 0x3) != 0x3:
                crc_timing_warning = "CRC END-to-completion timing is incomplete"
        except EvidenceError as exc:
            crc_timing_warning = str(exc)
    if crc_timing_warning is not None:
        warnings.append(crc_timing_warning)
    # A formal IQSC/UDP session must not share the data path with the optional
    # boot-time iiod TCP benchmark.  Older evidence schemas omit IiodPerf and
    # remain valid; when it is present, any initialized result or non-zero
    # state/counters proves that the diagnostic was enabled or ran.
    for candidate in candidates:
        # Check every snapshot, not only the selected completed one: a
        # diagnostic result is cumulative and must not be hidden by a later
        # snapshot chosen for its payload-complete IQ counters.
        iiod = candidate["snapshot"].get("IiodPerf")
        if iiod is None:
            continue
        if not isinstance(iiod, dict):
            failures.append("IiodPerf evidence is not an object")
            continue
        if bool_value(iiod.get("Initialized", False)):
            failures.append("iiod diagnostic is initialized during formal session")
        for name in ("State", "BytesReceived", "TargetBytes"):
            try:
                value = as_int(iiod.get(name, 0), f"IiodPerf.{name}")
                if value != 0:
                    failures.append(f"IiodPerf.{name}={value}, expected 0 for formal session")
            except EvidenceError as exc:
                failures.append(str(exc))
    if not bool_value(phy.get("LinkUp", False)):
        failures.append("PHY link is not up")
    for name in (
        "RxOverflow", "RxErrorFrames", "RxFcsErrorRaw", "RxFragmentErrorRaw",
        "DriverRxFail", "DriverRxPbufAllocFail", "IrqRxMessageLost",
    ):
        value = rmac.get(name)
        if value is not None:
            try:
                numeric = as_int(value, name)
                if numeric != 0:
                    warnings.append(f"{name}={numeric}")
            except EvidenceError as exc:
                warnings.append(str(exc))
    evidence = f"{selected['document']['_evidence_path']}#snapshot-session-{record['session']}"
    return {
        "state": "pass" if not failures else "fail",
        "failures": failures,
        "warnings": warnings,
        "evidence": evidence,
        "payload_mbps": round(as_int(iq.get("MbpsX1000", 0), "MbpsX1000") / 1000.0, 3),
        "crc": {
            "backend": iq.get("CrcBackend"),
            "hardware_self_test": iq.get("CrcHardwareSelfTest"),
            "timing_flags": iq.get("CrcTimingFlags"),
            "end_packet_cpu0_cycles": iq.get("EndPacketCpu0Cycles"),
            "complete_cpu0_cycles": iq.get("CrcCompleteCpu0Cycles"),
            "after_end_cycles": iq.get("CrcAfterEndCycles"),
        },
    }


def collect_runtime_candidates(documents: list[dict[str, Any]]) -> dict[int, list[dict[str, Any]]]:
    result: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for document in documents:
        snapshots = document.get("Snapshots")
        if not isinstance(snapshots, list) or not snapshots:
            raise EvidenceError(f"runtime evidence has no Snapshots array: {document['_evidence_path']}")
        for snapshot in snapshots:
            if not isinstance(snapshot, dict):
                continue
            try:
                session = as_int(snapshot.get("SessionId", 0), "runtime SessionId")
            except EvidenceError:
                continue
            if session > 0:
                result[session].append({"document": document, "snapshot": snapshot})
    return result


def evaluate_runtime(record: dict[str, Any], candidates: list[dict[str, Any]]) -> dict[str, Any]:
    if not candidates:
        return {"state": "missing", "failures": ["no CPU0/CPU1 runtime snapshot for session"], "warnings": []}
    selected = candidates[-1]
    snapshot = selected["snapshot"]
    latest = snapshot.get("Latest") if isinstance(snapshot.get("Latest"), dict) else {}
    tile = snapshot.get("Tile") if isinstance(snapshot.get("Tile"), dict) else {}
    runtime = snapshot.get("Runtime") if isinstance(snapshot.get("Runtime"), dict) else {}
    failures: list[str] = []
    warnings: list[str] = []

    def require(container: dict[str, Any], name: str, expected: int) -> None:
        try:
            actual = as_int(container.get(name), name)
        except EvidenceError as exc:
            failures.append(str(exc))
            return
        if actual != expected:
            failures.append(f"{name}={actual}, expected {expected}")

    if not bool_value(snapshot.get("ControlValid", False)):
        failures.append("display control is invalid")
    if not bool_value(latest.get("Valid", False)):
        failures.append("latest display slot is invalid")
    require(latest, "SessionId", record["session"])
    require(latest, "CenterIndex", record["center_index"])
    require(latest, "TileCount", EXPECTED_TILES)
    require(latest, "TileIndex", EXPECTED_TILES - 1)
    require(latest, "WindowSampleCount", EXPECTED_WINDOW_SAMPLES)
    require(latest, "StftFrameCount", EXPECTED_STFT_FRAMES)
    require(latest, "IngressDrops", 0)
    require(latest, "NpuReady", 1)
    try:
        timing_flags = as_int(latest.get("TimingFlags"), "TimingFlags")
        if (timing_flags & 0x7) != 0x7:
            failures.append(f"TimingFlags=0x{timing_flags:X} does not contain STFT/NPU/E2E proof bits")
    except EvidenceError as exc:
        failures.append(str(exc))
    for name in ("StftCycles", "NpuCycles", "EndToEndCycles"):
        try:
            if as_int(latest.get(name), name) <= 0:
                failures.append(f"{name} is not positive")
        except EvidenceError as exc:
            failures.append(str(exc))
    if not bool_value(tile.get("Valid", False)):
        failures.append("CPU1 display tile is invalid")
    require(tile, "SessionId", record["session"])
    if "WindowSequence" in latest and "WindowSequence" in tile:
        require(tile, "WindowSequence", as_int(latest["WindowSequence"], "Latest.WindowSequence"))
    if not bool_value(runtime.get("Valid", False)):
        failures.append("CPU1 runtime metrics are invalid")
    try:
        if as_int(runtime.get("Heartbeat"), "Runtime.Heartbeat") <= 0:
            failures.append("CPU1 heartbeat did not start")
    except EvidenceError as exc:
        failures.append(str(exc))
    if as_int(runtime.get("WaterfallTilesDropped", 0), "WaterfallTilesDropped") != 0:
        warnings.append(f"CPU1 waterfall drops={runtime.get('WaterfallTilesDropped')}")
    evidence = f"{selected['document']['_evidence_path']}#snapshot-session-{record['session']}"
    return {
        "state": "pass" if not failures else "fail",
        "failures": failures,
        "warnings": warnings,
        "evidence": evidence,
        "stft_ms": latest.get("StftMs"),
        "npu_ms": latest.get("NpuMs"),
        "end_to_end_ms": latest.get("EndToEndMs"),
        "cpu1_stage": runtime.get("Stage"),
        "cpu1_last_error": runtime.get("LastError"),
        "cpu1_running": runtime.get("Running"),
    }


def percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise EvidenceError("cannot calculate a percentile from an empty sample set")
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + ((ordered[upper] - ordered[lower]) * fraction)


def latency_stats(values: list[float]) -> dict[str, float]:
    return {
        "p50_ms": round(statistics.median(values), 6),
        "p95_ms": round(percentile(values, 0.95), 6),
        "max_ms": round(max(values), 6),
    }


def latency_array(session: dict[str, Any], name: str, failures: list[str]) -> list[float]:
    raw = session.get(name)
    if not isinstance(raw, list):
        failures.append(f"latency {name} array is missing")
        return []
    values: list[float] = []
    for index, value in enumerate(raw):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            failures.append(f"latency {name}[{index}] is not numeric")
            continue
        numeric = float(value)
        if not math.isfinite(numeric) or numeric < 0.0:
            failures.append(f"latency {name}[{index}] is invalid: {value!r}")
            continue
        values.append(numeric)
    if len(values) != EXPECTED_TILES:
        failures.append(f"latency {name} has {len(values)} samples, expected {EXPECTED_TILES}")
    return values


def collect_latency_candidates(documents: list[dict[str, Any]]) -> dict[int, list[dict[str, Any]]]:
    result: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for document in documents:
        sessions = document.get("Sessions")
        if not isinstance(sessions, list) or not sessions:
            raise EvidenceError(f"latency evidence has no Sessions array: {document['_evidence_path']}")
        for session in sessions:
            if not isinstance(session, dict):
                continue
            try:
                session_id = as_int(first_field(session, ("SessionId", "session_id"), 0), "latency SessionId")
            except EvidenceError:
                continue
            if session_id > 0:
                result[session_id].append({"document": document, "session": session})
    return result


def evaluate_latency(record: dict[str, Any], candidates: list[dict[str, Any]]) -> dict[str, Any]:
    if not candidates:
        return {"state": "missing", "failures": ["no per-window latency evidence for session"], "warnings": []}
    selected = candidates[-1]
    session = selected["session"]
    failures: list[str] = []
    warnings: list[str] = []
    try:
        center = as_int(first_field(session, ("CenterIndex", "center_index")), "latency CenterIndex")
        if center != record["center_index"]:
            failures.append(f"latency CenterIndex={center}, expected {record['center_index']}")
    except EvidenceError as exc:
        failures.append(str(exc))
    tile_indices_raw = first_field(session, ("TileIndices", "tile_indices"))
    if not isinstance(tile_indices_raw, list):
        failures.append("latency TileIndices array is missing")
    else:
        try:
            tile_indices = [as_int(value, "latency TileIndices") for value in tile_indices_raw]
            if tile_indices != list(range(EXPECTED_TILES)):
                failures.append("latency TileIndices are not exactly 0..18")
        except EvidenceError as exc:
            failures.append(str(exc))
    first_to_complete = latency_array(session, "FirstPacketToWindowCompleteMs", failures)
    complete_to_npu = latency_array(session, "WindowCompleteToNpuPublishMs", failures)
    complete_to_cpu1 = latency_array(session, "WindowCompleteToCpu1PublishMs", failures)
    counters: dict[str, int] = {}
    for name in ("PacketLossEvents", "ReorderEvents", "BackpressureEvents"):
        try:
            counters[name] = as_int(session.get(name), f"latency {name}")
            if counters[name] != 0:
                failures.append(f"latency {name}={counters[name]}, expected 0")
        except EvidenceError as exc:
            failures.append(str(exc))
    if WINDOW_RF_SPAN_MS > 10.0:
        failures.append(f"590336-sample RF window spans {WINDOW_RF_SPAN_MS:.6f} ms, over 10 ms")
    target_payload_wire_ms = EXPECTED_WINDOW_PAYLOAD_BYTES * 8.0 / (record["target_mbps"] * 1000.0)
    measured_mbps = record["payload_mbps_x1000"] / 1000.0
    measured_payload_wire_ms = EXPECTED_WINDOW_PAYLOAD_BYTES * 8.0 / (measured_mbps * 1000.0)
    if first_to_complete and percentile(first_to_complete, 0.50) < (target_payload_wire_ms * 0.5):
        warnings.append(
            "first-packet-to-window latency is implausibly below configured payload transfer time; check timestamp domains"
        )
    evidence = f"{selected['document']['_evidence_path']}#latency-session-{record['session']}"
    return {
        "state": "pass" if not failures else "fail",
        "failures": failures,
        "warnings": warnings,
        "evidence": evidence,
        "window_samples": EXPECTED_WINDOW_SAMPLES,
        "window_payload_bytes": EXPECTED_WINDOW_PAYLOAD_BYTES,
        "rf_window_span_ms": round(WINDOW_RF_SPAN_MS, 6),
        "target_payload_wire_ms": round(target_payload_wire_ms, 6),
        "sender_measured_payload_wire_ms": round(measured_payload_wire_ms, 6),
        "first_packet_to_window_complete": latency_stats(first_to_complete) if first_to_complete else None,
        "window_complete_to_npu_publish": latency_stats(complete_to_npu) if complete_to_npu else None,
        "window_complete_to_cpu1_publish": latency_stats(complete_to_cpu1) if complete_to_cpu1 else None,
        "samples": {
            "first_packet_to_window_complete_ms": first_to_complete,
            "window_complete_to_npu_publish_ms": complete_to_npu,
            "window_complete_to_cpu1_publish_ms": complete_to_cpu1,
        },
        "packet_loss_events": counters.get("PacketLossEvents"),
        "reorder_events": counters.get("ReorderEvents"),
        "backpressure_events": counters.get("BackpressureEvents"),
    }


def evaluate_npu_proof(document: dict[str, Any]) -> dict[str, Any]:
    proof = document.get("Proof") if isinstance(document.get("Proof"), dict) else document
    benchmark = document.get("Benchmark") if isinstance(document.get("Benchmark"), dict) else document.get("NpuBenchmark", {})
    fault = document.get("Fault") if isinstance(document.get("Fault"), dict) else document
    failures: list[str] = []

    def integer(paths: Iterable[str], label: str, source: dict[str, Any]) -> int | None:
        value = first_field(source, paths)
        try:
            return as_int(value, label)
        except EvidenceError as exc:
            failures.append(str(exc))
            return None

    magic = integer(("Magic", "magic", "Marker", "marker"), "NPU proof magic", proof)
    open_result = integer(("OpenResult", "open_result", "RmEthosuOpen", "rm_ethosu_open"), "RM_ETHOSU_Open result", proof)
    median = integer(("MedianCycles", "median_cycles"), "NPU proof median cycles", proof)
    checksum = integer(("Checksum", "checksum"), "NPU proof checksum", proof)
    benchmark_magic = integer(("Magic", "magic"), "NPU benchmark magic", benchmark)
    runs = integer(("Runs", "runs"), "NPU benchmark runs", benchmark)
    core_clock = integer(("CoreClockHz", "core_clock_hz"), "NPU core clock", benchmark)
    minimum = integer(("MinCycles", "min_cycles"), "NPU minimum cycles", benchmark)
    benchmark_median = integer(("MedianCycles", "median_cycles"), "NPU benchmark median", benchmark)
    maximum = integer(("MaxCycles", "max_cycles"), "NPU maximum cycles", benchmark)
    benchmark_checksum = integer(("Checksum", "checksum"), "NPU benchmark checksum", benchmark)
    cfsr = integer(("Cfsr", "CFSR", "cfsr"), "CFSR", fault)
    hfsr = integer(("Hfsr", "HFSR", "hfsr"), "HFSR", fault)
    samples_raw = first_field(benchmark, ("Samples", "samples"), [])
    samples: list[int] = []
    if not isinstance(samples_raw, list):
        failures.append("NPU benchmark samples are missing")
    else:
        for index, value in enumerate(samples_raw):
            try:
                samples.append(as_int(value, f"NPU sample[{index}]"))
            except EvidenceError as exc:
                failures.append(str(exc))
    if magic is not None and magic != NPU_PROOF_PASS_MAGIC:
        failures.append(f"NPU proof magic=0x{magic:08X}, expected 0x{NPU_PROOF_PASS_MAGIC:08X}")
    if open_result is not None and open_result != 0:
        failures.append(f"RM_ETHOSU_Open result={open_result}")
    if median is not None and median <= 0:
        failures.append("NPU proof median cycles is not positive")
    if benchmark_magic is not None and benchmark_magic != NPU_BENCHMARK_PASS_MAGIC:
        failures.append(f"benchmark magic=0x{benchmark_magic:08X}, expected 0x{NPU_BENCHMARK_PASS_MAGIC:08X}")
    if runs is not None and runs < 5:
        failures.append(f"only {runs} timed NPU runs; at least 5 are required")
    if runs is not None and len(samples) != runs:
        failures.append(f"benchmark has {len(samples)} samples but runs={runs}")
    if core_clock is not None and core_clock <= 0:
        failures.append("NPU core clock is not positive")
    if None not in (minimum, benchmark_median, maximum) and not (minimum <= benchmark_median <= maximum):
        failures.append("NPU min/median/max ordering is invalid")
    if samples:
        ordered = sorted(samples)
        calculated_median = ordered[len(ordered) // 2]
        if minimum is not None and minimum != ordered[0]:
            failures.append("NPU minimum does not match samples")
        if maximum is not None and maximum != ordered[-1]:
            failures.append("NPU maximum does not match samples")
        if benchmark_median is not None and benchmark_median != calculated_median:
            failures.append("NPU median does not match samples")
    if median is not None and benchmark_median is not None and median != benchmark_median:
        failures.append("g_npu_proof median differs from g_npu_benchmark median")
    if checksum is not None and benchmark_checksum is not None and checksum != benchmark_checksum:
        failures.append("g_npu_proof checksum differs from g_npu_benchmark checksum")
    if cfsr is not None and cfsr != 0:
        failures.append(f"CFSR=0x{cfsr:08X}")
    if hfsr is not None and hfsr != 0:
        failures.append(f"HFSR=0x{hfsr:08X}")
    return {
        "state": "pass" if not failures else "fail",
        "failures": failures,
        "evidence": document["_evidence_path"],
        "magic": f"0x{magic:08X}" if magic is not None else None,
        "runs": runs,
        "core_clock_hz": core_clock,
        "min_cycles": minimum,
        "median_cycles": benchmark_median,
        "max_cycles": maximum,
        "checksum": f"0x{checksum:08X}" if checksum is not None else None,
        "cfsr": f"0x{cfsr:08X}" if cfsr is not None else None,
        "hfsr": f"0x{hfsr:08X}" if hfsr is not None else None,
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    sender_paths = expand_paths(args.sender_log, "sender log")
    net_paths = expand_paths(args.net_stats, "network statistics")
    runtime_paths = expand_paths(args.runtime, "runtime sampler")
    latency_paths = expand_paths(args.latency, "window latency")
    npu_paths = expand_paths(args.npu_proof, "NPU proof")
    sender_records, parse_warnings = parse_sender_logs(sender_paths)
    net_documents = load_json_documents(net_paths, "network statistics")
    runtime_documents = load_json_documents(runtime_paths, "runtime sampler")
    latency_documents = load_json_documents(latency_paths, "window latency")
    npu_documents = load_json_documents(npu_paths, "NPU proof")

    duplicate_sessions = sorted(session for session, count in Counter(r["session"] for r in sender_records).items() if count != 1)
    global_failures: list[str] = []
    global_warnings = list(parse_warnings)
    if duplicate_sessions:
        global_failures.append(f"sender session IDs are not unique: {duplicate_sessions}")

    hashes: list[dict[str, str]] = []
    for index, document in enumerate(net_documents):
        hashes.append({"source": f"net[{index}]", "core": "CPU0", "sha256": cpu0_hash_from(document, f"net[{index}]")})
    for index, document in enumerate(runtime_documents):
        hashes.append({"source": f"runtime[{index}]", "core": "CPU0", "sha256": cpu0_hash_from(document, f"runtime[{index}] CPU0")})
        hashes.append({"source": f"runtime[{index}]", "core": "CPU1", "sha256": cpu1_hash_from(document, f"runtime[{index}] CPU1")})
    for index, document in enumerate(latency_documents):
        hashes.append({"source": f"latency[{index}]", "core": "CPU0", "sha256": cpu0_hash_from(document, f"latency[{index}] CPU0")})
        hashes.append({"source": f"latency[{index}]", "core": "CPU1", "sha256": cpu1_hash_from(document, f"latency[{index}] CPU1")})
    for index, document in enumerate(npu_documents):
        hashes.append({"source": f"npu[{index}]", "core": "CPU0", "sha256": cpu0_hash_from(document, f"npu[{index}] CPU0")})

    cpu0_hashes = sorted({item["sha256"] for item in hashes if item["core"] == "CPU0"})
    cpu1_hashes = sorted({item["sha256"] for item in hashes if item["core"] == "CPU1"})
    if len(cpu0_hashes) != 1:
        global_failures.append(f"CPU0 ELF hash mismatch across evidence: {cpu0_hashes}")
    if len(cpu1_hashes) != 1:
        global_failures.append(f"CPU1 ELF hash mismatch across runtime evidence: {cpu1_hashes}")
    expected_cpu0 = normalize_hash(args.expected_cpu0_sha256, "expected CPU0") if args.expected_cpu0_sha256 else None
    expected_cpu1 = normalize_hash(args.expected_cpu1_sha256, "expected CPU1") if args.expected_cpu1_sha256 else None
    if expected_cpu0 and cpu0_hashes != [expected_cpu0]:
        global_failures.append(f"CPU0 evidence hash does not equal expected {expected_cpu0}")
    if expected_cpu1 and cpu1_hashes != [expected_cpu1]:
        global_failures.append(f"CPU1 evidence hash does not equal expected {expected_cpu1}")

    npu_results = [evaluate_npu_proof(document) for document in npu_documents]
    for result in npu_results:
        if result["state"] != "pass":
            global_failures.extend(f"NPU proof: {failure}" for failure in result["failures"])

    net_candidates = collect_net_candidates(net_documents)
    runtime_candidates = collect_runtime_candidates(runtime_documents)
    latency_candidates = collect_latency_candidates(latency_documents)
    session_results: list[dict[str, Any]] = []
    for record in sorted(sender_records, key=lambda item: (item["target_mbps"], item["session"])):
        contract_failures = check_sender_contract(record)
        net = evaluate_net(record, net_candidates.get(record["session"], []))
        runtime = evaluate_runtime(record, runtime_candidates.get(record["session"], []))
        latency = evaluate_latency(record, latency_candidates.get(record["session"], []))
        explicit_transport_failure = net["state"] == "fail"
        if contract_failures:
            state = "invalid-contract"
            global_failures.extend(f"session {record['session']}: {failure}" for failure in contract_failures)
        elif net["state"] == "missing":
            state = "incomplete"
        elif net["state"] == "fail":
            state = "fail"
        elif runtime["state"] == "missing":
            state = "incomplete"
        elif runtime["state"] == "fail":
            state = "fail"
        elif latency["state"] == "missing":
            state = "incomplete"
        elif latency["state"] == "fail":
            state = "fail"
        else:
            state = "pass"
        # A failed transport can legitimately prevent the final 19th tile.  It
        # remains an explicit FAIL rate, not an evidence-INCOMPLETE rate.
        if explicit_transport_failure and runtime["state"] == "missing":
            state = "fail"
        if explicit_transport_failure and latency["state"] == "missing":
            state = "fail"
        session_results.append({
            "session_id": record["session"],
            "center_index": record["center_index"],
            "center_hz": record["center_hz"],
            "target_mbps": record["target_mbps"],
            "sender_measured_mbps": round(record["payload_mbps_x1000"] / 1000.0, 3),
            "sender_contract": "pass" if not contract_failures else "fail",
            "sender_failures": contract_failures,
            "net": net,
            "runtime": runtime,
            "latency": latency,
            "state": state,
        })
        global_warnings.extend(f"session {record['session']} network: {item}" for item in net.get("warnings", []))
        global_warnings.extend(f"session {record['session']} runtime: {item}" for item in runtime.get("warnings", []))
        global_warnings.extend(f"session {record['session']} latency: {item}" for item in latency.get("warnings", []))

    rates: list[dict[str, Any]] = []
    grouped: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for result in session_results:
        grouped[result["target_mbps"]].append(result)
    for rate in sorted(grouped):
        sessions = grouped[rate]
        center_counts = Counter(item["center_index"] for item in sessions)
        balanced = set(center_counts) == set(EXPECTED_CENTERS) and len(set(center_counts.values())) == 1
        states = Counter(item["state"] for item in sessions)
        if not balanced or states.get("incomplete", 0) or states.get("invalid-contract", 0):
            state = "incomplete"
        elif states.get("fail", 0):
            state = "fail"
        else:
            state = "pass"
        rates.append({
            "target_mbps": rate,
            "state": state,
            "session_count": len(sessions),
            "repetitions_per_center": min(center_counts.values()) if center_counts else 0,
            "center_counts": {str(key): center_counts.get(key, 0) for key in EXPECTED_CENTERS},
            "four_centers_balanced": balanced,
            "session_states": dict(sorted(states.items())),
            "latency": {
                "first_packet_to_window_complete": latency_stats([
                    value for item in sessions
                    for value in item["latency"].get("samples", {}).get("first_packet_to_window_complete_ms", [])
                ]) if any(item["latency"].get("samples", {}).get("first_packet_to_window_complete_ms") for item in sessions) else None,
                "window_complete_to_npu_publish": latency_stats([
                    value for item in sessions
                    for value in item["latency"].get("samples", {}).get("window_complete_to_npu_publish_ms", [])
                ]) if any(item["latency"].get("samples", {}).get("window_complete_to_npu_publish_ms") for item in sessions) else None,
                "window_complete_to_cpu1_publish": latency_stats([
                    value for item in sessions
                    for value in item["latency"].get("samples", {}).get("window_complete_to_cpu1_publish_ms", [])
                ]) if any(item["latency"].get("samples", {}).get("window_complete_to_cpu1_publish_ms") for item in sessions) else None,
            },
        })

    clean_rates = [item["target_mbps"] for item in rates if item["state"] == "pass"]
    peak_rate = max(clean_rates) if clean_rates else None
    stable_target = math.floor(peak_rate * 0.9) if peak_rate is not None else None
    stable_rate = next((item for item in rates if item["target_mbps"] == stable_target), None)
    if peak_rate is None:
        global_failures.append("no complete four-center zero-loss rate was proven")
    if stable_target is not None and stable_rate is None:
        global_failures.append(f"90% stable target {stable_target} Mbps was not explicitly tested")
    elif stable_rate is not None and stable_rate["state"] != "pass":
        global_failures.append(f"90% stable target {stable_target} Mbps did not pass")
    if peak_rate is not None:
        for rate in rates:
            if rate["target_mbps"] <= peak_rate and rate["state"] != "pass":
                global_failures.append(
                    f"rate sweep is non-monotonic/incomplete: {rate['target_mbps']} Mbps is {rate['state']} below peak {peak_rate} Mbps"
                )
            if rate["target_mbps"] > peak_rate and rate["state"] == "incomplete":
                global_failures.append(f"exploratory rate {rate['target_mbps']} Mbps lacks decisive evidence")

    unique_failures = list(dict.fromkeys(global_failures))
    verdict = "PASS" if not unique_failures else "FAIL"
    return {
        "tool": "ra8p1-acceptance-report",
        "tool_version": TOOL_VERSION,
        "generated_utc": utc_now(),
        "verdict": verdict,
        "policy": {
            "centers_hz": list(EXPECTED_CENTERS.values()),
            "samples_per_session": EXPECTED_SAMPLES,
            "tiles_per_session": EXPECTED_TILES,
            "tile_stride_samples": EXPECTED_STRIDE,
            "zero_loss_fields": ["SequenceGaps", "Reordered", "Invalid", "FullDrops", "OversizeDrops"],
            "fault_fields": ["CFSR", "HFSR"],
            "stable_rate_rule": "floor(highest complete four-center zero-loss target Mbps * 0.90), explicitly retested",
        },
        "summary": {
            "sender_sessions": len(sender_records),
            "rate_count": len(rates),
            "clean_rates_mbps": clean_rates,
            "highest_zero_loss_rate_mbps": peak_rate,
            "stable_rate_target_mbps": stable_target,
            "stable_rate_verified": bool(stable_rate and stable_rate["state"] == "pass"),
            "window_rf_span_ms": round(WINDOW_RF_SPAN_MS, 6),
            "window_rf_span_under_10ms": WINDOW_RF_SPAN_MS <= 10.0,
            "cpu0_elf_sha256": cpu0_hashes[0] if len(cpu0_hashes) == 1 else None,
            "cpu1_elf_sha256": cpu1_hashes[0] if len(cpu1_hashes) == 1 else None,
            "npu_proof_runs": len(npu_results),
        },
        "failures": unique_failures,
        "warnings": global_warnings,
        "rates": rates,
        "sessions": session_results,
        "npu_proofs": npu_results,
        "elf_hash_evidence": hashes,
        "inputs": {
            "sender_logs": [str(path) for path in sender_paths],
            "net_stats": [str(path) for path in net_paths],
            "runtime": [str(path) for path in runtime_paths],
            "latency": [str(path) for path in latency_paths],
            "npu_proof": [str(path) for path in npu_paths],
        },
        "evidence_boundary": (
            "This is an offline merger. PASS means the supplied artifacts consistently prove the stated gates; "
            "it does not acquire new hardware evidence or prove model accuracy."
        ),
    }


def markdown_report(report: dict[str, Any]) -> str:
    summary = report["summary"]
    lines = [
        "# RA8P1 SDR 流式推理最终验收报告",
        "",
        f"- 结论：**{report['verdict']}**",
        f"- 生成时间：`{report['generated_utc']}`",
        f"- CPU0 ELF SHA-256：`{summary['cpu0_elf_sha256'] or 'UNPROVEN'}`",
        f"- CPU1 ELF SHA-256：`{summary['cpu1_elf_sha256'] or 'UNPROVEN'}`",
        f"- 最高四中心零丢包速率：`{summary['highest_zero_loss_rate_mbps']}` Mbps",
        f"- 90% 稳定速率：`{summary['stable_rate_target_mbps']}` Mbps（已验证：`{summary['stable_rate_verified']}`）",
        f"- 590,336 点 RF 窗时长：`{summary['window_rf_span_ms']}` ms（≤10 ms：`{summary['window_rf_span_under_10ms']}`）",
        "",
        "## 速率判定",
        "",
        "| 目标速率 (Mbps) | 状态 | session 数 | 每中心重复数 | 四中心平衡 |",
        "|---:|:---:|---:|---:|:---:|",
    ]
    for rate in report["rates"]:
        lines.append(
            f"| {rate['target_mbps']} | {rate['state'].upper()} | {rate['session_count']} | "
            f"{rate['repetitions_per_center']} | {rate['four_centers_balanced']} |"
        )
    lines.extend([
        "",
        "## 逐窗低延时",
        "",
        "590,336 complex S16 IQ = 2,361,344 bytes；RF 时间跨度按 60 MSPS 计算。实际缓存 UDP 传输无需等于 RF 实时速率。",
        "",
        "| 目标 Mbps | 首包→窗完成 p50/p95/max ms | 窗完成→NPU发布 p50/p95/max ms | 窗完成→CPU1发布 p50/p95/max ms |",
        "|---:|:---:|:---:|:---:|",
    ])
    for rate in report["rates"]:
        latency = rate["latency"]
        def compact(metric: dict[str, Any] | None) -> str:
            if not metric:
                return "UNPROVEN"
            return f"{metric['p50_ms']}/{metric['p95_ms']}/{metric['max_ms']}"
        lines.append(
            f"| {rate['target_mbps']} | {compact(latency['first_packet_to_window_complete'])} | "
            f"{compact(latency['window_complete_to_npu_publish'])} | "
            f"{compact(latency['window_complete_to_cpu1_publish'])} |"
        )
    lines.extend([
        "",
        "## Session 证据",
        "",
        "| Session | 中心 | 目标 Mbps | Sender | CPU0 网络 | CPU0/CPU1 运行 | 逐窗延时 | 结论 |",
        "|---:|---:|---:|:---:|:---:|:---:|:---:|:---:|",
    ])
    for session in report["sessions"]:
        lines.append(
            f"| {session['session_id']} | {session['center_index']} | {session['target_mbps']} | "
            f"{session['sender_contract'].upper()} | {session['net']['state'].upper()} | "
            f"{session['runtime']['state'].upper()} | {session['latency']['state'].upper()} | "
            f"{session['state'].upper()} |"
        )
    lines.extend(["", "## NPU 与故障证据", ""])
    for index, proof in enumerate(report["npu_proofs"], 1):
        lines.extend([
            f"### Proof {index}",
            "",
            f"- 状态：`{proof['state'].upper()}`",
            f"- runs：`{proof['runs']}`",
            f"- min/median/max cycles：`{proof['min_cycles']}/{proof['median_cycles']}/{proof['max_cycles']}`",
            f"- checksum：`{proof['checksum']}`",
            f"- CFSR/HFSR：`{proof['cfsr']}/{proof['hfsr']}`",
            f"- 证据：`{proof['evidence']}`",
            "",
        ])
    lines.extend(["## 未通过项", ""])
    if report["failures"]:
        lines.extend(f"- {item}" for item in report["failures"])
    else:
        lines.append("- 无")
    lines.extend(["", "## 警告与证据边界", ""])
    if report["warnings"]:
        lines.extend(f"- {item}" for item in report["warnings"])
    else:
        lines.append("- 无额外警告")
    lines.extend(["", f"> {report['evidence_boundary']}", ""])
    return "\n".join(lines)


def write_text(path: str | None, text: str) -> None:
    if not path:
        return
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--sender-log", action="append", required=True, help="sender stdout log; repeat or use a glob")
    result.add_argument("--net-stats", action="append", required=True, help="ra8p1-cpu0-net-stats JSON; repeat or use a glob")
    result.add_argument("--runtime", action="append", required=True, help="ra8p1-runtime-sampler JSON; repeat or use a glob")
    result.add_argument("--latency", action="append", required=True, help="per-window latency JSON; repeat or use a glob")
    result.add_argument("--npu-proof", action="append", required=True, help="NPU proof JSON; repeat for reset-stability runs")
    result.add_argument("--expected-cpu0-sha256")
    result.add_argument("--expected-cpu1-sha256")
    result.add_argument("--output-json", help="write the complete machine-readable report")
    result.add_argument("--output-md", help="write the Markdown report")
    result.add_argument("--quiet", action="store_true", help="do not print Markdown to stdout")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        report = build_report(args)
        json_text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        md_text = markdown_report(report)
        write_text(args.output_json, json_text)
        write_text(args.output_md, md_text)
        if not args.quiet:
            print(md_text, end="")
        return 0 if report["verdict"] == "PASS" else 1
    except EvidenceError as exc:
        print(f"acceptance input error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
