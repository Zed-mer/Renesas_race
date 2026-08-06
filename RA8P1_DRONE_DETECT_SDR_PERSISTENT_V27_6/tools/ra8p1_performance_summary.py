#!/usr/bin/env python3
"""Summarize offline SDR capture, transport, inference, and CPU1 evidence.

The command deliberately consumes files only.  It never opens a board, SDR,
socket, or debugger.  Values are tagged as ``measured``, ``estimated``, or
``missing`` so a convenient serial estimate cannot be mistaken for a timing
measurement.  In particular, four-frequency totals are not reported as a
measured value unless the sender supplied an explicit cycle interval.
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
from pathlib import Path
from typing import Any, Iterable, Sequence


TOOL_VERSION = "1.0"
CENTER_HZ = {
    0: 2_420_000_000,
    1: 2_464_000_000,
    2: 5_760_000_000,
    3: 5_816_000_000,
}
CENTER_ORDER = (0, 1, 2, 3)
SAMPLE_RATE_HZ = 60_000_000
IQ_BYTES_PER_SAMPLE = 4
WINDOW_SAMPLES = 590_336
WINDOW_STRIDE_SAMPLES = 295_168
WINDOW_PAYLOAD_BYTES = WINDOW_SAMPLES * IQ_BYTES_PER_SAMPLE
RF_WINDOW_SPAN_MS = WINDOW_SAMPLES * 1000.0 / SAMPLE_RATE_HZ

KEY_VALUE_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s,;]+)")
EVENT_RE = re.compile(r"\b(captured|tuned|set_rx|sent|dry-run)\b", re.IGNORECASE)


class EvidenceError(RuntimeError):
    """Raised when an input artifact cannot be interpreted safely."""


def _read_text(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise EvidenceError(f"cannot read {path}: {exc}") from exc
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        try:
            return data.decode("utf-16-le")
        except UnicodeDecodeError as exc:
            raise EvidenceError(f"cannot decode {path} as UTF-8 or UTF-16LE") from exc


def _as_float(value: Any, label: str, *, nonnegative: bool = True) -> float:
    if isinstance(value, bool):
        raise EvidenceError(f"{label} is not numeric: {value!r}")
    if isinstance(value, (int, float)):
        result = float(value)
    elif isinstance(value, str):
        try:
            result = float(value.strip())
        except ValueError as exc:
            raise EvidenceError(f"{label} is not numeric: {value!r}") from exc
    else:
        raise EvidenceError(f"{label} is not numeric: {value!r}")
    if not math.isfinite(result) or (nonnegative and result < 0.0):
        raise EvidenceError(f"{label} is invalid: {value!r}")
    return result


def _as_int(value: Any, label: str) -> int:
    result = _as_float(value, label)
    if not result.is_integer():
        raise EvidenceError(f"{label} is not an integer: {value!r}")
    return int(result)


def _bool_value(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def _first(obj: Any, names: Iterable[str], default: Any = None) -> Any:
    if not isinstance(obj, dict):
        return default
    for name in names:
        if name in obj and obj[name] is not None:
            return obj[name]
    return default


def _round(value: float | None, digits: int = 6) -> float | None:
    return None if value is None else round(float(value), digits)


def _source(path: Path, line: int | None = None) -> str:
    return f"{path}:{line}" if line is not None else str(path)


def _expand_paths(values: Sequence[str | Path], label: str) -> list[Path]:
    result: list[Path] = []
    seen: set[str] = set()
    for raw in values:
        text = str(raw)
        matches = [Path(item) for item in glob.glob(text)]
        if not matches and Path(text).is_file():
            matches = [Path(text)]
        if not matches:
            raise EvidenceError(f"{label} path did not match a file: {text}")
        for match in matches:
            resolved = match.resolve()
            key = str(resolved).lower()
            if key not in seen:
                seen.add(key)
                result.append(resolved)
    return result


def _missing(reason: str, *, scope: str = "") -> dict[str, Any]:
    return {
        "status": "missing",
        "value_ms": None,
        "basis": reason,
        "source": [],
        "scope": scope,
    }


def _metric(
    value_ms: float,
    status: str,
    basis: str,
    source: Sequence[str],
    *,
    scope: str = "",
    sample_count: int = 1,
) -> dict[str, Any]:
    value = float(value_ms)
    if not math.isfinite(value) or value < 0.0:
        raise EvidenceError(f"invalid metric value {value_ms!r}")
    return {
        "status": status,
        "value_ms": _round(value),
        "basis": basis,
        "source": list(source),
        "scope": scope,
        "sample_count": sample_count,
    }


def _stats_metric(
    values: Sequence[float],
    status: str,
    basis: str,
    source: Sequence[str],
    *,
    scope: str = "",
) -> dict[str, Any]:
    if not values:
        return _missing(basis, scope=scope)
    ordered = sorted(float(item) for item in values)
    if len(ordered) == 1:
        p95 = ordered[0]
    else:
        position = (len(ordered) - 1) * 0.95
        lower = math.floor(position)
        upper = math.ceil(position)
        p95 = ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    result = _metric(
        statistics.median(ordered),
        status,
        basis,
        source,
        scope=scope,
        sample_count=len(ordered),
    )
    result.update({
        "p50_ms": _round(statistics.median(ordered)),
        "p95_ms": _round(p95),
        "max_ms": _round(max(ordered)),
    })
    return result


def _parse_timestamp_ms(values: dict[str, str]) -> float | None:
    for key in ("timestamp_ms", "time_ms", "start_ms", "end_ms", "elapsed_ms"):
        if key in values:
            try:
                return _as_float(values[key], key)
            except EvidenceError:
                return None
    for key in ("timestamp_us", "time_us", "start_us", "end_us"):
        if key in values:
            try:
                return _as_float(values[key], key) / 1000.0
            except EvidenceError:
                return None
    for key in ("timestamp", "timestamp_utc", "time", "time_utc"):
        text = values.get(key)
        if not text:
            continue
        try:
            parsed = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
            return parsed.timestamp() * 1000.0
        except ValueError:
            continue
    return None


def _event_number(values: dict[str, str], names: Iterable[str]) -> float | None:
    for name in names:
        if name in values:
            try:
                return _as_float(values[name], name)
            except EvidenceError:
                return None
    return None


def _center_index(center_hz: int | None, center_index: int | None) -> int | None:
    if center_index in CENTER_HZ and (center_hz is None or CENTER_HZ[center_index] == center_hz):
        return center_index
    if center_hz is None:
        return None
    for index, expected in CENTER_HZ.items():
        if center_hz == expected:
            return index
    return None


def parse_sender_logs(paths: Sequence[str | Path]) -> tuple[list[dict[str, Any]], list[str]]:
    """Parse sender records and optional capture/tune timing events.

    A sender record is a ``sent`` or ``dry-run`` key/value line.  Capture and
    tune events are associated by explicit session when present, otherwise by
    center and occurrence order.  Unknown lines are ignored intentionally.
    """

    records: list[dict[str, Any]] = []
    events: list[dict[str, Any]] = []
    warnings: list[str] = []
    sequence = 0
    for raw_path in paths:
        path = Path(raw_path).resolve()
        for line_number, raw_line in enumerate(_read_text(path).splitlines(), 1):
            match = EVENT_RE.search(raw_line)
            if not match:
                continue
            event_name = match.group(1).lower()
            values = {key.lower(): value.rstrip("\r\n") for key, value in KEY_VALUE_RE.findall(raw_line)}
            source = _source(path, line_number)
            center_hz: int | None = None
            center_index: int | None = None
            try:
                if "center_hz" in values:
                    center_hz = _as_int(values["center_hz"], f"{source}:center_hz")
                if "center_index" in values:
                    center_index = _as_int(values["center_index"], f"{source}:center_index")
            except EvidenceError as exc:
                warnings.append(str(exc))
                continue
            center_index = _center_index(center_hz, center_index)
            session: int | None = None
            if "session" in values:
                try:
                    session = _as_int(values["session"], f"{source}:session")
                except EvidenceError as exc:
                    warnings.append(str(exc))
            timestamp_ms = _parse_timestamp_ms(values)
            if event_name in {"captured", "tuned", "set_rx"}:
                event: dict[str, Any] = {
                    "kind": event_name,
                    "center_hz": center_hz,
                    "center_index": center_index,
                    "session": session,
                    "source": source,
                    "order": sequence,
                    "timestamp_ms": timestamp_ms,
                }
                if event_name == "captured":
                    event["capture_ms"] = _event_number(values, ("capture_ms", "capture_time_ms"))
                else:
                    event["tune_ms"] = _event_number(
                        values, ("tune_ms", "tune_time_ms", "set_rx_ms", "elapsed_ms")
                    )
                if any(key in event for key in ("capture_ms", "tune_ms")) and event.get(
                    "capture_ms", event.get("tune_ms")
                ) is not None:
                    events.append(event)
                sequence += 1
                continue
            if event_name not in {"sent", "dry-run"}:
                sequence += 1
                continue
            required = ("session", "samples")
            if any(key not in values for key in required):
                warnings.append(f"{source}: sender record missing session or samples")
                sequence += 1
                continue
            try:
                session = _as_int(values["session"], f"{source}:session")
                samples = _as_int(values["samples"], f"{source}:samples")
            except EvidenceError as exc:
                warnings.append(str(exc))
                sequence += 1
                continue
            if center_hz is None and center_index in CENTER_HZ:
                center_hz = CENTER_HZ[center_index]
            payload_bytes = _event_number(values, ("payload_bytes", "bytes"))
            if payload_bytes is None:
                payload_bytes = float(samples * IQ_BYTES_PER_SAMPLE)
            payload_rate = _event_number(values, ("payload_mbps_x1000",))
            if payload_rate is not None:
                payload_rate /= 1000.0
            if payload_rate is None:
                payload_rate = _event_number(values, ("payload_mbps",))
            target_rate = _event_number(values, ("target_mbps",))
            explicit_send_ms = _event_number(
                values,
                ("send_ms", "send_time_ms", "wire_ms", "payload_wire_ms", "elapsed_ms"),
            )
            start_ms = _event_number(values, ("start_ms", "session_start_ms"))
            end_ms = _event_number(values, ("end_ms", "session_end_ms"))
            if start_ms is None and end_ms is None:
                start_ms = timestamp_ms
            record = {
                "mode": event_name,
                "session": session,
                "center_hz": center_hz,
                "center_index": center_index,
                "samples": samples,
                "payload_bytes": int(payload_bytes) if payload_bytes.is_integer() else payload_bytes,
                "payload_mbps": payload_rate,
                "target_mbps": target_rate,
                "send_ms": explicit_send_ms,
                "start_ms": start_ms,
                "end_ms": end_ms,
                "capture_ms": _event_number(values, ("capture_ms", "capture_time_ms")),
                "tune_ms": _event_number(
                    values, ("tune_ms", "tune_time_ms", "set_rx_ms")
                ),
                "source": source,
                "order": sequence,
            }
            records.append(record)
            sequence += 1
    if not records:
        raise EvidenceError("no complete sent/dry-run sender records were found")

    # Attach unkeyed capture/tune lines in the same center order as sender
    # records.  Explicit values on a sent line always take precedence.
    seen_sessions: set[int] = set()
    for record in records:
        if record["session"] in seen_sessions:
            warnings.append(f"{record['source']}: duplicate sender session {record['session']}")
        seen_sessions.add(record["session"])
    by_session: dict[int, dict[str, Any]] = {item["session"]: item for item in records}
    used_events: set[int] = set()
    for event_index, event in enumerate(events):
        if event.get("session") in by_session:
            record = by_session[event["session"]]
            key = "capture_ms" if event["kind"] == "captured" else "tune_ms"
            if record.get(key) is None and event.get(key) is not None:
                record[key] = event[key]
                record[f"{key}_source"] = event["source"]
                used_events.add(event_index)
    for record in records:
        for key, kind in (("capture_ms", "capture_ms"), ("tune_ms", "tune_ms")):
            if record.get(key) is not None:
                continue
            candidates: list[tuple[int, dict[str, Any]]] = []
            for event_index, event in enumerate(events):
                event_kind = "capture_ms" if event["kind"] == "captured" else "tune_ms"
                if (
                    event_index not in used_events
                    and event_kind == kind
                    and event.get("center_index") == record.get("center_index")
                    and event.get("order", -1) <= record.get("order", -1)
                    and event.get(kind) is not None
                ):
                    candidates.append((event_index, event))
            if candidates:
                event_index, event = max(candidates, key=lambda item: item[1].get("order", -1))
                used_events.add(event_index)
                record[key] = event[kind]
                record[f"{key}_source"] = event["source"]
    for record in records:
        if record.get("center_index") not in CENTER_HZ:
            warnings.append(f"{record['source']}: center does not match the fixed four-frequency contract")
        if record["mode"] == "dry-run":
            warnings.append(f"{record['source']}: dry-run is not hardware transmission evidence")
    return records, warnings


def _load_json(path: Path) -> Any:
    try:
        return json.loads(_read_text(path))
    except json.JSONDecodeError as exc:
        raise EvidenceError(f"cannot parse JSON {path}: {exc}") from exc


def _snapshot_entries(payload: Any, source: str) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    if isinstance(payload, list):
        for item in payload:
            entries.extend(_snapshot_entries(item, source))
        return entries
    if not isinstance(payload, dict):
        return entries
    snapshots = payload.get("Snapshots", payload.get("snapshots"))
    if isinstance(snapshots, list):
        inherited = {
            key: payload[key]
            for key in (
                "Cpu0DwtClockHz", "DwtClockHz", "Cpu0CycleHz", "CycleToMs", "TimestampUtc",
                "cpu0_dwt_clock_hz", "dwt_clock_hz", "cpu0_cycle_hz", "cycle_to_ms", "timestamp_utc",
            )
            if key in payload
        }
        for index, item in enumerate(snapshots):
            if isinstance(item, dict):
                copy = dict(inherited)
                copy.update(item)
                copy["_source"] = f"{source}#snapshot-{index}"
                entries.append(copy)
        return entries
    if any(key in payload for key in ("SessionId", "session_id", "Latest", "latest", "Latency", "latency", "Runtime", "runtime")):
        copy = dict(payload)
        copy["_source"] = source
        entries.append(copy)
    return entries


def parse_runtime_json(paths: Sequence[str | Path]) -> tuple[list[dict[str, Any]], list[str]]:
    entries: list[dict[str, Any]] = []
    warnings: list[str] = []
    for raw_path in paths:
        path = Path(raw_path).resolve()
        parsed = _snapshot_entries(_load_json(path), str(path))
        if not parsed:
            warnings.append(f"{path}: no Snapshots entries found")
        entries.extend(parsed)
    if not entries:
        raise EvidenceError("runtime input contains no snapshot entries")
    return entries, warnings


def _runtime_session(entry: dict[str, Any]) -> int | None:
    raw = _first(entry, ("SessionId", "session_id", "session"))
    if raw is None:
        latest = entry.get("Latest", entry.get("latest"))
        raw = _first(latest, ("SessionId", "session_id", "session"))
    if raw is None:
        return None
    try:
        value = _as_int(raw, "runtime SessionId")
        return value if value > 0 else None
    except EvidenceError:
        return None


def _runtime_center(entry: dict[str, Any]) -> int | None:
    latest_value = entry.get("Latest", entry.get("latest"))
    latest = latest_value if isinstance(latest_value, dict) else {}
    raw = _first(
        latest,
        ("CenterIndex", "center_index", "centerIndex"),
        _first(entry, ("CenterIndex", "center_index", "centerIndex")),
    )
    try:
        value = _as_int(raw, "runtime CenterIndex") if raw is not None else None
        return value if value in CENTER_HZ else None
    except EvidenceError:
        return None


def _select_runtime(sender: dict[str, Any], entries: Sequence[dict[str, Any]]) -> dict[str, Any] | None:
    exact = [item for item in entries if _runtime_session(item) == sender["session"]]
    if exact:
        return exact[-1]
    # Center-only matching is allowed only for legacy snapshots which carry no
    # session ID.  Reusing a timing result from another session at the same RF
    # center would manufacture evidence.
    same_center = [
        item for item in entries
        if _runtime_session(item) is None and _runtime_center(item) == sender.get("center_index")
    ]
    return same_center[-1] if same_center else None


def _clock_hz(entry: dict[str, Any]) -> float | None:
    raw = _first(
        entry,
        ("Cpu0DwtClockHz", "DwtClockHz", "Cpu0CycleHz", "cpu0_dwt_clock_hz", "dwt_clock_hz", "cpu0_cycle_hz"),
    )
    if raw is not None:
        try:
            value = _as_float(raw, "Cpu0DwtClockHz")
            return value if value > 0.0 else None
        except EvidenceError:
            return None
    # The current sampler emits this explicit conversion contract.  Do not
    # assume a clock when neither the clock nor the conversion string exists.
    conversion = entry.get("CycleToMs", entry.get("cycle_to_ms"))
    if isinstance(conversion, str) and "1000000" in conversion:
        return 1_000_000_000.0
    return None


def _latest_stage(
    entry: dict[str, Any],
    latest: dict[str, Any],
    ms_names: Iterable[str],
    cycle_names: Iterable[str],
    label: str,
) -> dict[str, Any]:
    ms_names = tuple(ms_names)
    cycle_names = tuple(cycle_names)
    source = [str(entry.get("_source", "runtime"))]
    latest_valid = latest.get("Valid", latest.get("valid"))
    if latest_valid is not None and not _bool_value(latest_valid):
        return _missing(f"runtime Latest.{label} slot is invalid", scope="window")
    timing_flags = latest.get("TimingFlags", latest.get("timing_flags"))
    flag_by_label = {"STFT": 0x1, "NPU": 0x2, "E2E": 0x4}
    if timing_flags is not None and label in flag_by_label:
        try:
            if (_as_int(timing_flags, "TimingFlags") & flag_by_label[label]) == 0:
                return _missing(f"runtime TimingFlags does not validate {label}", scope="window")
        except EvidenceError:
            return _missing("runtime TimingFlags is invalid", scope="window")
    raw = _first(latest, ms_names)
    if raw is not None:
        try:
            value = _as_float(raw, label)
            if value > 0.0:
                return _metric(value, "measured", f"runtime Latest.{next(iter(ms_names))}", source, scope="window")
        except EvidenceError:
            pass
    raw_cycles = _first(latest, cycle_names)
    clock = _clock_hz(entry)
    if raw_cycles is not None and clock:
        try:
            cycles = _as_float(raw_cycles, f"{label} cycles")
            if cycles > 0.0:
                value = cycles * 1000.0 / clock
                return _metric(value, "measured", f"runtime cycles / {clock:g} Hz", source, scope="window")
        except EvidenceError:
            pass
    return _missing(f"runtime {label} timing is absent", scope="window")


def _latency_values(entry: dict[str, Any], sender_session: int) -> tuple[list[float], list[str]]:
    latency_value = entry.get("Latency", entry.get("latency"))
    latency = latency_value if isinstance(latency_value, dict) else {}
    if ("Valid" in latency or "valid" in latency) and not _bool_value(
        latency.get("Valid", latency.get("valid"))
    ):
        return [], [str(entry.get("_source", "runtime"))]
    raw_records = latency.get("ValidRecords", latency.get("valid_records"))
    if not isinstance(raw_records, list):
        raw_records = latency.get("Records", latency.get("records"))
    if isinstance(raw_records, dict):
        raw_records = [raw_records]
    values: list[float] = []
    source = [str(entry.get("_source", "runtime"))]
    if isinstance(raw_records, list):
        for record in raw_records:
            if not isinstance(record, dict) or (
                ("Valid" in record or "valid" in record)
                and not _bool_value(record.get("Valid", record.get("valid")))
            ):
                continue
            session = _first(record, ("SessionId", "session_id", "session"))
            if session is not None:
                try:
                    if _as_int(session, "latency SessionId") != sender_session:
                        continue
                except EvidenceError:
                    continue
            raw = _first(
                record,
                (
                    "WindowCompleteToCpu1VisibleUpperMs",
                    "WindowCompleteToCpu1PublishMs",
                    "WindowCompleteToCpu1VisibleMs",
                    "Cpu1VisibleMs",
                    "window_complete_to_cpu1_visible_upper_ms",
                    "window_complete_to_cpu1_publish_ms",
                    "window_complete_to_cpu1_visible_ms",
                    "cpu1_visible_ms",
                ),
            )
            if (
                ("Cpu1VisibleUpperValid" in record or "cpu1_visible_upper_valid" in record)
                and not _bool_value(record.get("Cpu1VisibleUpperValid", record.get("cpu1_visible_upper_valid")))
            ):
                continue
            if raw is not None:
                try:
                    value = _as_float(raw, "CPU1 visible latency")
                    if value >= 0.0:
                        values.append(value)
                except EvidenceError:
                    continue
    if values:
        return values, source
    for name in (
        "WindowCompleteToCpu1VisibleUpperMs",
        "WindowCompleteToCpu1PublishMs",
        "WindowCompleteToCpu1VisibleMs",
        "window_complete_to_cpu1_visible_upper_ms",
        "window_complete_to_cpu1_publish_ms",
        "window_complete_to_cpu1_visible_ms",
    ):
        raw = latency.get(name)
        if isinstance(raw, list):
            for item in raw:
                try:
                    values.append(_as_float(item, name))
                except EvidenceError:
                    pass
        elif raw is not None and not values:
            try:
                values.append(_as_float(raw, name))
            except EvidenceError:
                pass
    return values, source


def _runtime_fps(entry: dict[str, Any]) -> dict[str, Any] | None:
    runtime_value = entry.get("Runtime", entry.get("runtime"))
    runtime = runtime_value if isinstance(runtime_value, dict) else {}
    source = [str(entry.get("_source", "runtime"))]
    fields = (
        ("InferenceRateHz", 1.0),
        ("WindowRateHz", 1.0),
        ("PresentedFpsHz", 1.0),
        ("ContentFpsHz", 1.0),
        ("inference_rate_hz", 1.0),
        ("window_rate_hz", 1.0),
        ("presented_fps_hz", 1.0),
        ("content_fps_hz", 1.0),
        ("InferenceRateMillihz", 0.001),
        ("WindowRateMillihz", 0.001),
        ("PresentedFpsMillihz", 0.001),
        ("ContentFpsMillihz", 0.001),
        ("inference_rate_millihz", 0.001),
        ("window_rate_millihz", 0.001),
        ("presented_fps_millihz", 0.001),
        ("content_fps_millihz", 0.001),
    )
    for name, scale in fields:
        if name not in runtime:
            continue
        try:
            value = _as_float(runtime[name], f"Runtime.{name}") * scale
        except EvidenceError:
            continue
        if value > 0.0:
            return {
                "status": "measured",
                "value_hz": _round(value),
                "basis": f"runtime Runtime.{name}",
                "source": source,
            }
    return None


def _sender_metric(record: dict[str, Any], key: str, override: float | None, label: str) -> dict[str, Any]:
    value = record.get(key)
    if value is not None:
        source = [record.get(f"{key}_source", record["source"])]
        return _metric(float(value), "measured", f"sender {label}", source, scope="session")
    if override is not None:
        return _metric(override, "estimated", f"operator supplied --{key.replace('_', '-')}", [], scope="session")
    return _missing(f"sender {label} timing is absent", scope="session")


def _send_metric(record: dict[str, Any], payload_bytes: float, *, scope: str) -> dict[str, Any]:
    source = [record["source"]]
    explicit = record.get("send_ms")
    if explicit is not None:
        return _metric(explicit, "measured", "sender explicit send/wire elapsed_ms", source, scope=scope)
    rate = record.get("payload_mbps") or record.get("target_mbps")
    if rate and rate > 0.0 and payload_bytes > 0.0:
        basis = "payload bytes / sender measured payload Mbps"
        status = "estimated"
        if record.get("payload_mbps") is None:
            basis = "payload bytes / sender target Mbps"
        return _metric(payload_bytes * 8.0 / (rate * 1000.0), status, basis, source, scope=scope)
    return _missing("sender payload rate is absent; send time cannot be derived", scope=scope)


def _window_count(samples: int) -> int:
    if samples < WINDOW_SAMPLES:
        return 0
    return 1 + (samples - WINDOW_SAMPLES) // WINDOW_STRIDE_SAMPLES


def _processing_metric(stages: dict[str, dict[str, Any]]) -> dict[str, Any]:
    e2e = stages["e2e"]
    visible = stages["cpu1_visible"]
    if e2e["value_ms"] is not None:
        if visible["value_ms"] is not None:
            return _metric(
                e2e["value_ms"] + visible["value_ms"],
                "estimated",
                "Latest.EndToEndMs + WindowCompleteToCpu1VisibleUpperMs",
                e2e.get("source", []) + visible.get("source", []),
                scope="window-to-CPU1",
            )
        return _metric(
            e2e["value_ms"],
            "estimated",
            "Latest.EndToEndMs (CPU1 visibility timing missing)",
            e2e.get("source", []),
            scope="window-to-NPU",
        )
    components = [stages[name]["value_ms"] for name in ("stft", "npu")]
    if all(value is not None for value in components):
        value = sum(float(item) for item in components)
        if visible["value_ms"] is not None:
            value += visible["value_ms"]
        return _metric(
            value,
            "estimated",
            "STFT + NPU + optional CPU1 visibility (E2E timing absent)",
            stages["stft"].get("source", []) + stages["npu"].get("source", []),
            scope="window-to-CPU1",
        )
    return _missing("no complete per-window processing timing", scope="window-to-CPU1")


def summarize_session(
    sender: dict[str, Any],
    runtime: dict[str, Any] | None,
    *,
    capture_ms: float | None,
    tune_ms: float | None,
) -> dict[str, Any]:
    samples = sender["samples"]
    payload_bytes = float(sender["payload_bytes"])
    windows = _window_count(samples)
    capture = _sender_metric(sender, "capture_ms", capture_ms, "capture_ms")
    tune = _sender_metric(sender, "tune_ms", tune_ms, "tune_ms")
    stages: dict[str, dict[str, Any]] = {
        "capture": capture,
        "tune": tune,
        "send": _send_metric(sender, payload_bytes, scope="session"),
        "stft": _missing("runtime snapshot is absent", scope="window"),
        "npu": _missing("runtime snapshot is absent", scope="window"),
        "e2e": _missing("runtime snapshot is absent", scope="window"),
        "cpu1_visible": _missing("CPU1 visibility timing is absent", scope="window"),
    }
    runtime_source: str | None = None
    measured_fps: dict[str, Any] | None = None
    runtime_warnings: list[str] = []
    if runtime is not None:
        runtime_source = str(runtime.get("_source", "runtime"))
        latest_value = runtime.get("Latest", runtime.get("latest"))
        latest = latest_value if isinstance(latest_value, dict) else {}
        control_valid = runtime.get("ControlValid", runtime.get("control_valid"))
        control_invalid = control_valid is not None and not _bool_value(control_valid)
        if control_invalid:
            runtime_warnings.append("runtime display control is invalid")
        else:
            stages["stft"] = _latest_stage(
                runtime,
                latest,
                ("StftMs", "STFTMs", "stft_ms"),
                ("StftCycles", "STFTCycles", "stft_cycles"),
                "STFT",
            )
            stages["npu"] = _latest_stage(
                runtime,
                latest,
                ("NpuMs", "NPUMs", "npu_ms"),
                ("NpuCycles", "NPUCycles", "npu_cycles"),
                "NPU",
            )
            stages["e2e"] = _latest_stage(
                runtime,
                latest,
                ("EndToEndMs", "E2EMs", "end_to_end_ms", "e2e_ms"),
                ("EndToEndCycles", "E2ECycles", "end_to_end_cycles", "e2e_cycles"),
                "E2E",
            )
        values, latency_source = (
            ([], [runtime_source])
            if control_invalid
            else _latency_values(runtime, sender["session"])
        )
        if values:
            stages["cpu1_visible"] = _stats_metric(
                values,
                "measured",
                "runtime Latency WindowCompleteToCpu1VisibleUpperMs",
                latency_source,
                scope="window",
            )
        measured_fps = _runtime_fps(runtime)
        if not measured_fps:
            runtime_warnings.append("runtime snapshot has no positive steady-state rate")
    else:
        runtime_warnings.append("no runtime snapshot matched this sender session")
    window_send = _send_metric(
        sender,
        WINDOW_PAYLOAD_BYTES if samples >= WINDOW_SAMPLES else payload_bytes,
        scope="window",
    )
    rf_span = _metric(
        RF_WINDOW_SPAN_MS,
        "estimated",
        "window samples / fixed 60 MSPS contract",
        [],
        scope="RF window",
    )
    processing = _processing_metric(stages)
    if measured_fps:
        fps = measured_fps
    else:
        periods = [rf_span["value_ms"], window_send.get("value_ms"), processing.get("value_ms")]
        periods = [float(value) for value in periods if value is not None and value > 0.0]
        if periods:
            period = max(periods)
            fps = {
                "status": "estimated",
                "value_hz": _round(1000.0 / period),
                "basis": "1000 / max(RF window span, window send time, processing-to-CPU1 time)",
                "source": [],
            }
        else:
            fps = {
                "status": "missing",
                "value_hz": None,
                "basis": "no measured or derivable per-window period",
                "source": [],
            }
    return {
        "session_id": sender["session"],
        "center_index": sender.get("center_index"),
        "center_hz": sender.get("center_hz"),
        "mode": sender["mode"],
        "samples": samples,
        "payload_bytes": payload_bytes,
        "window_count": windows,
        "source": sender["source"],
        "runtime_source": runtime_source,
        "stages": stages,
        "window_send": window_send,
        "rf_window_span": rf_span,
        "processing_to_cpu1": processing,
        "steady_state_fps": fps,
        "warnings": runtime_warnings,
    }


def _cycle_groups(sessions: Sequence[dict[str, Any]]) -> tuple[list[list[dict[str, Any]]], list[str]]:
    groups: list[list[dict[str, Any]]] = []
    warnings: list[str] = []
    index = 0
    while index < len(sessions):
        if [item.get("center_index") for item in sessions[index:index + 4]] == list(CENTER_ORDER):
            groups.append(list(sessions[index:index + 4]))
            index += 4
            continue
        if sessions[index].get("center_index") == CENTER_ORDER[0]:
            warnings.append(
                f"incomplete four-frequency cycle near session {sessions[index].get('session_id')}"
            )
        else:
            warnings.append(
                f"sender order is not fixed 2420/2464/5760/5816 near session {sessions[index].get('session_id')}"
            )
        index += 1
    return groups, warnings


def _coverage_for_cycle(cycle: Sequence[dict[str, Any]], cycle_index: int) -> dict[str, Any]:
    formula = (
        "sum over fixed centers [capture + tune + full-session send + "
        "window_count * (E2E + CPU1 visible)]"
    )
    missing: list[str] = []
    components: list[float] = []
    statuses: list[str] = []
    direct_start = cycle[0].get("_start_ms")
    direct_end = cycle[-1].get("_end_ms")
    # Session reports do not expose sender timestamps by default; the direct
    # path is populated by build_summary when explicit intervals are present.
    if direct_start is not None and direct_end is not None and direct_end >= direct_start:
        return {
            "cycle_index": cycle_index,
            "status": "measured",
            "value_ms": _round(direct_end - direct_start),
            "basis": "explicit sender cycle start/end timestamps",
            "formula": "cycle_end_ms - cycle_start_ms",
            "missing": [],
            "order_hz": [CENTER_HZ[index] for index in CENTER_ORDER],
        }
    for item in cycle:
        center = item.get("center_hz") or CENTER_HZ.get(item.get("center_index"))
        prefix = f"center {center}"
        for name in ("capture", "tune", "send"):
            metric = item["stages"][name]
            statuses.append(metric["status"])
            if metric.get("value_ms") is None:
                missing.append(f"{prefix} {name} timing")
            else:
                components.append(float(metric["value_ms"]))
        processing = item["processing_to_cpu1"]
        statuses.append(processing["status"])
        if (
            processing.get("value_ms") is None
            or item.get("window_count", 0) <= 0
            or item["stages"]["cpu1_visible"].get("value_ms") is None
        ):
            missing.append(f"{prefix} per-window processing/CPU1 timing")
        else:
            components.append(float(processing["value_ms"]) * int(item["window_count"]))
    if missing:
        return {
            "cycle_index": cycle_index,
            "status": "missing",
            "value_ms": None,
            "basis": "four-frequency total cannot be proven without every required stage",
            "formula": formula,
            "missing": missing,
            "order_hz": [CENTER_HZ[index] for index in CENTER_ORDER],
        }
    status = "measured" if all(item == "measured" for item in statuses) else "estimated"
    basis = (
        "explicitly observed stage intervals"
        if status == "measured"
        else "serial formula; sender/runtime files have no complete cycle timestamp"
    )
    return {
        "cycle_index": cycle_index,
        "status": status,
        "value_ms": _round(sum(components)),
        "basis": basis,
        "formula": formula,
        "missing": [],
        "order_hz": [CENTER_HZ[index] for index in CENTER_ORDER],
    }


def build_summary(
    sender_paths: Sequence[str | Path],
    runtime_paths: Sequence[str | Path],
    *,
    capture_ms: float | None = None,
    tune_ms: float | None = None,
) -> dict[str, Any]:
    if capture_ms is not None:
        capture_ms = _as_float(capture_ms, "capture_ms")
    if tune_ms is not None:
        tune_ms = _as_float(tune_ms, "tune_ms")
    expanded_sender_paths = _expand_paths(sender_paths, "sender log")
    expanded_runtime_paths = _expand_paths(runtime_paths, "runtime JSON")
    sender_records, sender_warnings = parse_sender_logs(expanded_sender_paths)
    runtime_entries, runtime_warnings = parse_runtime_json(expanded_runtime_paths)
    sessions: list[dict[str, Any]] = []
    for sender in sender_records:
        runtime = _select_runtime(sender, runtime_entries)
        report = summarize_session(sender, runtime, capture_ms=capture_ms, tune_ms=tune_ms)
        # Keep private timestamp fields only for the coverage calculator.
        report["_start_ms"] = sender.get("start_ms")
        report["_end_ms"] = sender.get("end_ms")
        sessions.append(report)
    cycles, cycle_warnings = _cycle_groups(sessions)
    coverage_cycles = [_coverage_for_cycle(cycle, index) for index, cycle in enumerate(cycles, 1)]
    if coverage_cycles:
        first = coverage_cycles[0]
        coverage = dict(first)
        coverage["cycles"] = coverage_cycles
    else:
        coverage = {
            "cycle_index": None,
            "status": "missing",
            "value_ms": None,
            "basis": "no complete fixed-order four-frequency cycle was found",
            "formula": "sum over 2420, 2464, 5760, 5816 MHz",
            "missing": ["complete sender records in fixed order"],
            "order_hz": [CENTER_HZ[index] for index in CENTER_ORDER],
            "cycles": [],
        }
    fps_values = [
        item["steady_state_fps"]["value_hz"]
        for item in sessions
        if item["steady_state_fps"].get("value_hz") is not None
    ]
    measured_fps = [
        item["steady_state_fps"]["value_hz"]
        for item in sessions
        if item["steady_state_fps"].get("status") == "measured"
        and item["steady_state_fps"].get("value_hz") is not None
    ]
    if measured_fps:
        steady_fps = {
            "status": "measured",
            "value_hz": _round(statistics.median(measured_fps)),
            "basis": "median of runtime-reported steady-state rates",
            "source": [item["runtime_source"] for item in sessions if item["runtime_source"]],
            "sample_count": len(measured_fps),
        }
    elif fps_values:
        steady_fps = {
            "status": "estimated",
            "value_hz": _round(statistics.median(fps_values)),
            "basis": "median of per-session bottleneck estimates",
            "source": [],
            "sample_count": len(fps_values),
        }
    else:
        steady_fps = {
            "status": "missing",
            "value_hz": None,
            "basis": "no measured or derivable steady-state rate",
            "source": [],
            "sample_count": 0,
        }
    # Remove private fields from the public report after coverage calculation.
    for item in sessions:
        item.pop("_start_ms", None)
        item.pop("_end_ms", None)
    warnings = sender_warnings + runtime_warnings + cycle_warnings
    warnings.extend(item_warning for item in sessions for item_warning in item["warnings"])
    warnings = list(dict.fromkeys(warnings))
    return {
        "tool": "ra8p1-performance-summary",
        "tool_version": TOOL_VERSION,
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "contract": {
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "window_samples": WINDOW_SAMPLES,
            "window_stride_samples": WINDOW_STRIDE_SAMPLES,
            "window_payload_bytes": WINDOW_PAYLOAD_BYTES,
            "rf_window_span_ms": _round(RF_WINDOW_SPAN_MS),
            "center_order_hz": [CENTER_HZ[index] for index in CENTER_ORDER],
        },
        "single_frequency": sessions,
        "steady_state_fps": steady_fps,
        "four_frequency_total": coverage,
        "summary": {
            "sender_sessions": len(sessions),
            "complete_four_frequency_cycles": len(coverage_cycles),
            "steady_state_fps": steady_fps,
            "four_frequency_total": coverage,
        },
        "model_accuracy": {
            "status": "not_proven",
            "conclusion": "The NPU model is a placeholder; this evidence cannot establish drone-detection accuracy.",
        },
        "warnings": warnings,
        "inputs": {
            "sender_logs": [str(item) for item in expanded_sender_paths],
            "runtime_json": [str(item) for item in expanded_runtime_paths],
            "capture_ms_override": capture_ms,
            "tune_ms_override": tune_ms,
        },
        "evidence_boundary": (
            "This is an offline timing summary. Runtime stage fields and explicit sender elapsed fields are measured; "
            "wire-rate formulas, operator overrides, and four-frequency serial sums are estimates unless explicit cycle "
            "timestamps are present. No model accuracy conclusion is valid for the placeholder NPU model."
        ),
    }


def _format_metric(metric: dict[str, Any], *, unit: str = "ms") -> str:
    status = str(metric.get("status", "missing")).upper()
    value = metric.get("value_ms") if unit == "ms" else metric.get("value_hz")
    if value is None:
        return f"MISSING ({metric.get('basis', 'no evidence')})"
    return f"{value:g} {unit} [{status}]"


def markdown_report(report: dict[str, Any]) -> str:
    coverage_value = report["four_frequency_total"].get("value_ms")
    coverage_text = "MISSING" if coverage_value is None else f"{coverage_value:g} ms"
    lines = [
        "# RA8P1 SDR performance summary",
        "",
        "Stage labels are explicit: MEASURED comes from a timing field/cycle counter; "
        "ESTIMATED comes from a formula or operator override; MISSING is not substituted.",
        "",
        "## Single-frequency sessions",
        "",
        "| Session | Center (MHz) | Capture | Tune | Send full cache | STFT/window | NPU/window | E2E/window | CPU1 visible | FPS |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for item in report["single_frequency"]:
        stages = item["stages"]
        center = item.get("center_hz")
        center_text = "?" if center is None else f"{center / 1e6:g}"
        lines.append(
            f"| {item['session_id']} | {center_text} | {_format_metric(stages['capture'])} | "
            f"{_format_metric(stages['tune'])} | {_format_metric(stages['send'])} | "
            f"{_format_metric(stages['stft'])} | {_format_metric(stages['npu'])} | "
            f"{_format_metric(stages['e2e'])} | {_format_metric(stages['cpu1_visible'])} | "
            f"{_format_metric(item['steady_state_fps'], unit='Hz')} |"
        )
    lines.extend([
        "",
        "## Four-frequency coverage",
        "",
        f"- Fixed order: `{', '.join(str(value // 1_000_000) for value in report['contract']['center_order_hz'])} MHz`",
        f"- Status: `{report['four_frequency_total']['status'].upper()}`",
        f"- Total: `{coverage_text}`",
        f"- Basis: `{report['four_frequency_total']['basis']}`",
        f"- Formula: `{report['four_frequency_total']['formula']}`",
    ])
    missing = report["four_frequency_total"].get("missing", [])
    if missing:
        lines.append("- Missing: " + "; ".join(missing))
    lines.extend([
        "",
        "## Overall steady-state rate",
        "",
        f"`{_format_metric(report['steady_state_fps'], unit='Hz')}`",
        "",
        "## Model boundary",
        "",
        "The NPU model is a placeholder. These measurements prove transport and "
        "pipeline behavior only; they cannot establish drone-detection accuracy.",
        "",
        "## Evidence boundary",
        "",
        report["evidence_boundary"],
    ])
    if report.get("warnings"):
        lines.extend(["", "## Warnings", ""])
        lines.extend(f"- {warning}" for warning in report["warnings"])
    return "\n".join(lines) + "\n"


def _write(path: str | None, content: str) -> None:
    if not path:
        return
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8", newline="\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--sender-log", action="append", required=True, help="sender stdout log; repeat or use a glob")
    result.add_argument("--runtime", "--runtime-json", dest="runtime", action="append", required=True, help="runtime sampler JSON; repeat for multiple sessions")
    result.add_argument(
        "--capture-ms", "--capture_ms", dest="capture_ms", type=float,
        help="fallback SDR capture duration per center (estimated)",
    )
    result.add_argument(
        "--tune-ms", "--tune_ms", dest="tune_ms", type=float,
        help="fallback retune duration per center (estimated)",
    )
    result.add_argument("--output-json", help="write machine-readable JSON")
    result.add_argument("--output-md", help="write Markdown report")
    result.add_argument("--quiet", action="store_true", help="do not print Markdown to stdout")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        report = build_summary(
            args.sender_log,
            args.runtime,
            capture_ms=args.capture_ms,
            tune_ms=args.tune_ms,
        )
    except EvidenceError as exc:
        print(f"performance summary input error: {exc}", file=sys.stderr)
        return 2
    _write(args.output_json, json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    markdown = markdown_report(report)
    _write(args.output_md, markdown)
    if not args.quiet:
        print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
