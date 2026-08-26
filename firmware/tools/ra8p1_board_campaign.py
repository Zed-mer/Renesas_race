#!/usr/bin/env python3
"""Orchestrate and verify low-latency RA8P1/SDR hardware campaigns.

The tool deliberately treats the three physical paths as independent:

* programming: development PC -> J-Link/SWD -> RA8P1;
* runtime: SDR 192.168.31.10 <-> RA8P1 192.168.31.20;
* management: development PC -> SDR shell used only to stage /tmp artifacts.

No command in this file pings either runtime address. A missing PC management
route is recorded as a deployment-channel issue, never as proof that the SDR
and RA8P1 runtime link is down.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence


TOOL_VERSION = "1.2"
PLAN_SCHEMA = 1
MANIFEST_SCHEMA = 1
WINDOW_SAMPLES = 590_336
WINDOW_PAYLOAD_BYTES = WINDOW_SAMPLES * 4
SAMPLE_RATE_HZ = 60_000_000
BANDWIDTH_HZ = 56_000_000
TRACE_COMPLETE_FLAGS = 0xE7FF
TRACE_CAPTURE_READY_FLAG = 1 << 12
TRACE_RETRY_FLAG = 1 << 11
CENTERS_HZ = (2_420_000_000, 2_464_000_000, 5_760_000_000, 5_816_000_000)
RATE_STEPS_MBPS = (390, 500, 600, 700, 800)
SENDER_CRC_BACKENDS = frozenset({"nibble", "slice8"})
FAULT_FLAGS = {
    "none": 0,
    "crc": 1 << 0,
    "drop": 1 << 1,
    "request-timeout": 1 << 2,
    "ack-timeout": 1 << 3,
    # This requires a switch/netem/control-datagram duplicator. It is not a
    # firmware flag because the receiver must observe a real duplicate.
    "duplicate-request": 0,
}
KV_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
SHA256_RE = re.compile(r"^[0-9A-F]{64}$")


class CampaignError(RuntimeError):
    """Input, collection, or evidence failure."""


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    for encoding in ("utf-8-sig", "utf-16-le", "gb18030"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            pass
    raise CampaignError(f"cannot decode text evidence: {path}")


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(read_text(path))
    except (OSError, json.JSONDecodeError) as exc:
        raise CampaignError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise CampaignError(f"JSON root must be an object: {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def as_int(value: Any, label: str) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            pass
    raise CampaignError(f"{label} is not an integer: {value!r}")


def as_float(value: Any, label: str) -> float:
    if isinstance(value, bool):
        raise CampaignError(f"{label} is not numeric: {value!r}")
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise CampaignError(f"{label} is not numeric: {value!r}") from exc
    if not math.isfinite(result):
        raise CampaignError(f"{label} is not finite: {value!r}")
    return result


def bool_value(value: Any) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def nested(obj: Any, dotted: str, default: Any = None) -> Any:
    current = obj
    for part in dotted.split("."):
        if not isinstance(current, dict) or part not in current:
            return default
        current = current[part]
    return current


def u32_delta(new: int, old: int) -> int:
    return (new - old) & 0xFFFFFFFF


def newer_u32(candidate: int, baseline: int) -> bool:
    delta = u32_delta(candidate, baseline)
    return 0 < delta < 0x80000000


def cycle_ms(start: int, end: int) -> float:
    return round(u32_delta(end, start) / 1_000_000.0, 6)


def signed_cycle_ms(reference: int, event: int) -> float:
    delta = u32_delta(event, reference)
    if delta >= 0x80000000:
        delta -= 0x100000000
    return round(delta / 1_000_000.0, 6)


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise CampaignError("cannot calculate percentile of an empty series")
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def series_summary(values: Sequence[float]) -> dict[str, Any]:
    finite = [float(item) for item in values if math.isfinite(float(item))]
    if not finite:
        return {"count": 0, "p50": None, "p95": None, "max": None}
    return {
        "count": len(finite),
        "p50": round(statistics.median(finite), 6),
        "p95": round(percentile(finite, 0.95), 6),
        "max": round(max(finite), 6),
    }


def scenario(
    scenario_id: str,
    kind: str,
    target_mbps: int,
    centers: Sequence[int],
    *,
    mode: str = "overlap",
    fault: str = "none",
    minimum_payload_mbps: float | None = None,
    notes: str = "",
) -> dict[str, Any]:
    result = {
        "id": scenario_id,
        "kind": kind,
        "target_mbps": target_mbps,
        "sample_rate_hz": SAMPLE_RATE_HZ,
        "bandwidth_hz": BANDWIDTH_HZ,
        "sample_count": WINDOW_SAMPLES,
        "expected_windows": len(centers),
        "expected_center_sequence": list(centers),
        "pipeline_mode": mode,
        "fault": fault,
        "test_fault_flags": FAULT_FLAGS[fault],
        "notes": notes,
    }
    if minimum_payload_mbps is not None:
        result["minimum_payload_mbps"] = minimum_payload_mbps
    return result


def default_plan(production_rate_mbps: int) -> dict[str, Any]:
    scenarios: list[dict[str, Any]] = []
    scenarios.append(scenario(
        "single-2420-100", "single-frequency-100", production_rate_mbps,
        [0] * 100, mode="serial",
        notes="One CPU1-requested center repeated for 100 completed windows.",
    ))
    for rate in RATE_STEPS_MBPS:
        scenarios.append(scenario(
            f"rate-{rate}", "rate-step", rate, [0, 1, 2, 3],
            notes="Fresh four-center sweep; do not reuse cached evidence from another rate.",
        ))
    scenarios.append(scenario(
        "four-center-10-rounds", "four-center-10-rounds", production_rate_mbps,
        [0, 1, 2, 3] * 10,
    ))
    scenarios.extend([
        scenario("fault-crc", "fault-injection", 390, [0], mode="serial", fault="crc"),
        scenario("fault-drop", "fault-injection", 390, [0], mode="serial", fault="drop"),
        scenario(
            "fault-request-timeout", "fault-injection", 390, [0],
            mode="serial", fault="request-timeout",
        ),
        scenario(
            "fault-ack-timeout", "fault-injection", 390, [0],
            mode="serial", fault="ack-timeout",
        ),
        scenario(
            "fault-duplicate-request", "fault-injection", 390, [0],
            mode="serial", fault="duplicate-request",
            notes="Duplicate the same SDRC/5004 datagram externally; do not alter SDR firmware.",
        ),
        scenario(
            "serial-four-center-10", "pipeline-comparison", production_rate_mbps,
            [0, 1, 2, 3] * 10, mode="serial",
            notes="CPU1 schedules the next center only after prior CREDIT_ACCEPTED.",
        ),
        scenario(
            "overlap-four-center-10", "pipeline-comparison", production_rate_mbps,
            [0, 1, 2, 3] * 10, mode="overlap",
            notes="CPU0 prefetches the next CAPTURE_REQ before prior WINDOW_ACK.",
        ),
    ])
    return {
        "tool": "ra8p1-board-campaign",
        "tool_version": TOOL_VERSION,
        "schema_version": PLAN_SCHEMA,
        "created_utc": utc_now(),
        "topology": {
            "programming_link": {
                "path": "development PC -> J-Link/SWD -> RA8P1 CPU0/CPU1",
                "runtime_dependency": False,
            },
            "runtime_data_link": {
                "path": "SDR 192.168.31.10 <-> RA8P1 192.168.31.20",
                "data": "IQSC UDP/5003",
                "control": "SDRC UDP/5004",
                "diagnostic_ack": "UDP/5002",
                "host_ping_is_gate": False,
            },
            "development_management_link": {
                "purpose": "stage/start /tmp SDR agent and retrieve diagnostics",
                "allowed_methods": ["same-subnet", "USB", "serial", "existing-remote-shell"],
                "runtime_dependency": False,
            },
        },
        "fixed_contract": {
            "centers_hz": list(CENTERS_HZ),
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "bandwidth_hz": BANDWIDTH_HZ,
            "window_samples": WINDOW_SAMPLES,
            "window_payload_bytes": WINDOW_PAYLOAD_BYTES,
            "rf_span_ms_derived": round(WINDOW_SAMPLES * 1000.0 / SAMPLE_RATE_HZ, 6),
            "sdr_agent_install": "/tmp only; upload/start again after every SDR reboot",
            "sdr_firmware_changes": "forbidden",
        },
        "scenarios": scenarios,
    }


def get_scenario(plan: dict[str, Any], scenario_id: str) -> dict[str, Any]:
    for item in plan.get("scenarios", []):
        if isinstance(item, dict) and item.get("id") == scenario_id:
            return item
    raise CampaignError(f"scenario is not present in plan: {scenario_id}")


def validate_plan(plan: dict[str, Any]) -> None:
    if as_int(plan.get("schema_version"), "plan schema") != PLAN_SCHEMA:
        raise CampaignError("unsupported campaign plan schema")
    ids: set[str] = set()
    for item in plan.get("scenarios", []):
        if not isinstance(item, dict):
            raise CampaignError("scenario entry is not an object")
        scenario_id = str(item.get("id", ""))
        if not scenario_id or scenario_id in ids:
            raise CampaignError(f"invalid or duplicate scenario id: {scenario_id!r}")
        ids.add(scenario_id)
        centers = item.get("expected_center_sequence")
        if not isinstance(centers, list) or not centers:
            raise CampaignError(f"{scenario_id}: expected center sequence is empty")
        if any(as_int(value, "center index") not in range(4) for value in centers):
            raise CampaignError(f"{scenario_id}: invalid center index")
        if as_int(item.get("expected_windows"), "expected_windows") != len(centers):
            raise CampaignError(f"{scenario_id}: expected window count/sequence mismatch")
        if as_int(item.get("sample_count"), "sample_count") != WINDOW_SAMPLES:
            raise CampaignError(f"{scenario_id}: low-latency sample contract changed")
        if "minimum_payload_mbps" in item:
            minimum_payload = as_float(
                item["minimum_payload_mbps"], "minimum_payload_mbps"
            )
            if minimum_payload <= 0.0:
                raise CampaignError(f"{scenario_id}: minimum payload must be positive")
    required = {
        "single-2420-100", "four-center-10-rounds",
        *(f"rate-{rate}" for rate in RATE_STEPS_MBPS),
        "fault-crc", "fault-drop", "fault-request-timeout",
        "fault-ack-timeout", "fault-duplicate-request",
        "serial-four-center-10", "overlap-four-center-10",
    }
    missing = sorted(required - ids)
    if missing:
        raise CampaignError(f"campaign plan is missing required scenarios: {', '.join(missing)}")


def file_record(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise CampaignError(f"file does not exist: {resolved}")
    stat = resolved.stat()
    return {
        "path": str(resolved),
        "size": stat.st_size,
        "timestamp_utc": dt.datetime.fromtimestamp(
            stat.st_mtime, dt.timezone.utc
        ).isoformat().replace("+00:00", "Z"),
        "sha256": sha256_file(resolved),
    }


def powershell_path() -> str:
    candidates = [
        shutil.which("pwsh"),
        r"C:\Program Files\PowerShell\7\pwsh.exe",
        shutil.which("powershell"),
        r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise CampaignError("PowerShell executable was not found")


def run_collector(command: Sequence[str], output: Path, dry_run: bool) -> None:
    if dry_run:
        print("DRY-RUN:", subprocess.list2cmdline(list(command)))
        return
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")
        raise CampaignError(
            f"collector failed ({completed.returncode}): {subprocess.list2cmdline(list(command))}\n{stderr}"
        )
    raw = completed.stdout
    parsed: Any = None
    for encoding in ("utf-8-sig", "utf-16-le", "gb18030"):
        try:
            parsed = json.loads(raw.decode(encoding))
            break
        except (UnicodeDecodeError, json.JSONDecodeError):
            pass
    if not isinstance(parsed, dict):
        raise CampaignError(f"collector did not return a JSON object: {command[0]}")
    write_json(output, parsed)


def collector_commands(args: argparse.Namespace, phase_dir: Path) -> dict[str, list[str]]:
    root = Path(__file__).resolve().parent.parent
    ps = powershell_path()
    prefix = [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]

    def common(script: str) -> list[str]:
        command = prefix + [str(root / "tools" / script), "-ProbeSerial", args.probe_serial]
        if args.jlink_exe:
            command += ["-JLinkExe", str(Path(args.jlink_exe).resolve())]
        return command

    net = common("ra8p1-cpu0-net-stats.ps1") + [
        "-Cpu0Elf", str(Path(args.cpu0_elf).resolve()), "-Json",
    ]
    trace = common("ra8p1-cpu0-trace.ps1") + [
        "-Cpu0Elf", str(Path(args.cpu0_elf).resolve()), "-Json",
    ]
    runtime = common("ra8p1-runtime-sampler.ps1") + [
        "-Cpu0Elf", str(Path(args.cpu0_elf).resolve()),
        "-Cpu1Elf", str(Path(args.cpu1_elf).resolve()), "-Json",
    ]
    commands = {"net": net, "trace": trace, "runtime": runtime}
    if args.phase == "after":
        campaign = prefix + [
            str(root / "tools" / "ra8p1-cpu1-campaign.ps1"),
            "-Action", "ReadStatus",
            "-Cpu1Elf", str(Path(args.cpu1_elf).resolve()),
            "-ProbeSerial", args.probe_serial,
            "-Json",
        ]
        if args.jlink_exe:
            campaign += ["-JLinkExe", str(Path(args.jlink_exe).resolve())]
        commands["cpu1_campaign"] = campaign
    return commands


def copy_optional(path_value: str | None, destination: Path) -> dict[str, Any] | None:
    if not path_value:
        return None
    source = Path(path_value).resolve()
    if not source.is_file():
        raise CampaignError(f"evidence file does not exist: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return file_record(destination)


def capture_phase(args: argparse.Namespace) -> int:
    plan_path = Path(args.plan).resolve()
    plan = read_json(plan_path)
    validate_plan(plan)
    selected = get_scenario(plan, args.scenario)
    scenario_dir = Path(args.evidence_root).resolve() / args.scenario
    phase_dir = scenario_dir / args.phase
    manifest_path = scenario_dir / "manifest.json"
    commands = collector_commands(args, phase_dir)
    if args.dry_run:
        for name, command in commands.items():
            run_collector(command, phase_dir / f"{name}.json", True)
        return 0
    phase_dir.mkdir(parents=True, exist_ok=True)
    for name, command in commands.items():
        run_collector(command, phase_dir / f"{name}.json", False)

    cpu0 = file_record(Path(args.cpu0_elf))
    cpu1 = file_record(Path(args.cpu1_elf))
    sdr_artifacts = {
        "agent": file_record(Path(args.sdr_agent_artifact))
        if args.sdr_agent_artifact else None,
        "adapter": file_record(Path(args.sdr_adapter_artifact))
        if args.sdr_adapter_artifact else None,
    }
    if manifest_path.is_file():
        manifest = read_json(manifest_path)
        if nested(manifest, "firmware.cpu0.sha256") != cpu0["sha256"]:
            raise CampaignError("CPU0 ELF changed between campaign phases")
        if nested(manifest, "firmware.cpu1.sha256") != cpu1["sha256"]:
            raise CampaignError("CPU1 ELF changed between campaign phases")
        for name, record in sdr_artifacts.items():
            if record is None:
                continue
            expected = nested(manifest, f"sdr_artifacts.{name}.sha256")
            if expected and expected != record["sha256"]:
                raise CampaignError(f"SDR {name} changed between campaign phases")
            manifest.setdefault("sdr_artifacts", {})[name] = record
    else:
        manifest = {
            "tool": "ra8p1-board-campaign",
            "tool_version": TOOL_VERSION,
            "schema_version": MANIFEST_SCHEMA,
            "created_utc": utc_now(),
            "plan": file_record(plan_path),
            "scenario": selected,
            "firmware": {"cpu0": cpu0, "cpu1": cpu1},
            "sdr_artifacts": sdr_artifacts,
            "links": {
                "programming": {
                    "path": "development PC -> J-Link/SWD -> RA8P1",
                    "probe_serial": args.probe_serial,
                    "evidence": None,
                },
                "runtime": {
                    "path": "SDR 192.168.31.10 <-> RA8P1 192.168.31.20",
                    "ports": {"iqsc": 5003, "sdrc": 5004, "diagnostic_ack": 5002},
                    "host_ping_used": False,
                },
                "management": {
                    "method": args.management_method,
                    "runtime_prerequisite": False,
                    "issue": args.management_issue or None,
                    "sdr_agent_location": "/tmp",
                },
            },
            "phases": {},
        }

    phase_evidence = {
        "captured_utc": utc_now(),
        "net": file_record(phase_dir / "net.json"),
        "trace": file_record(phase_dir / "trace.json"),
        "runtime": file_record(phase_dir / "runtime.json"),
    }
    campaign_proof = phase_dir / "cpu1_campaign.json"
    if campaign_proof.is_file():
        phase_evidence["cpu1_campaign"] = file_record(campaign_proof)
    manifest["phases"][args.phase] = phase_evidence
    if args.phase == "after":
        agent_record = copy_optional(args.agent_log, scenario_dir / "agent.log")
        flash_record = copy_optional(args.flash_log, scenario_dir / "flash.log")
        management_record = copy_optional(
            args.management_log, scenario_dir / "management.log"
        )
        manifest["agent_log"] = agent_record
        manifest["links"]["programming"]["evidence"] = flash_record
        manifest["links"]["management"]["evidence"] = management_record
    write_json(manifest_path, manifest)
    print(f"captured {args.phase} evidence: {scenario_dir}")
    return 0


def parse_agent_log(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line_number, raw in enumerate(read_text(path).splitlines(), 1):
        if "SDRC control_trace " in raw:
            kind = "control"
        elif "SDRC window_trace " in raw:
            kind = "window"
        else:
            continue
        values = {key: value.rstrip(",;") for key, value in KV_RE.findall(raw)}
        event: dict[str, Any] = {
            "kind": kind,
            "line": line_number,
            "raw": raw,
            "values": values,
        }
        for key in (
            "request", "session", "center_index", "attempt", "credit", "status",
            "samples", "actual_mbps_x1000", "target_mbps_x1000", "gso_batches",
            "gso_fallbacks", "gaps", "reordered", "invalid", "ring_full_drops",
            "ring_oversize_drops", "crc_errors", "capture_elapsed_us",
            "tune_elapsed_us", "send_elapsed_us", "retransmit",
            "adapter_block_setup_us", "adapter_dma_wait_us",
            "adapter_disable_us", "adapter_copy_us",
        ):
            if key in values:
                try:
                    event[key] = int(values[key], 0)
                except ValueError:
                    pass
        events.append(event)
    return events


def trace_records(document: dict[str, Any], baseline: int | None) -> list[dict[str, Any]]:
    records = nested(document, "Trace.Records")
    if not isinstance(records, list):
        raise CampaignError("trace evidence has no Trace.Records array")
    valid = [item for item in records if isinstance(item, dict)]
    if baseline is None:
        return sorted(valid, key=lambda item: as_int(item.get("Sequence"), "trace sequence"))
    selected = [
        item for item in valid
        if newer_u32(as_int(item.get("Sequence"), "trace sequence"), baseline)
    ]
    return sorted(
        selected,
        key=lambda item: u32_delta(as_int(item.get("Sequence"), "trace sequence"), baseline),
    )


def validate_trace_interval(
    report: dict[str, Any],
    before: dict[str, Any],
    after: dict[str, Any],
    selected: Sequence[dict[str, Any]],
    expected: int,
) -> None:
    """Prove that the selected post-baseline trace interval is recoverable.

    A full ring may overwrite historical records while a short campaign runs.
    That is not itself a campaign-data loss when every sequence after the
    baseline remains present.  The interval is unrecoverable only when it
    exceeds the ring capacity or its exact sequence range cannot be rebuilt.
    """
    baseline = as_int(nested(before, "Trace.LatestSequence", 0), "LatestSequence")
    latest = as_int(nested(after, "Trace.LatestSequence", 0), "LatestSequence")
    sequence_delta = u32_delta(latest, baseline)
    before_started = as_int(nested(before, "Trace.RecordsStarted", 0), "RecordsStarted")
    after_started = as_int(nested(after, "Trace.RecordsStarted", 0), "RecordsStarted")
    produced = u32_delta(after_started, before_started)
    after_records = trace_records(after, None)
    capacity_raw = nested(after, "Trace.Capacity")
    capacity = (
        as_int(capacity_raw, "Trace.Capacity")
        if capacity_raw is not None
        else max(expected, len(after_records))
    )
    before_overwrites = as_int(
        nested(before, "Trace.RecordsOverwritten", 0), "RecordsOverwritten"
    )
    after_overwrites = as_int(
        nested(after, "Trace.RecordsOverwritten", 0), "RecordsOverwritten"
    )
    overwrite_delta = u32_delta(after_overwrites, before_overwrites)
    interval: dict[str, Any] = {
        "baseline_sequence": baseline,
        "latest_sequence": latest,
        "sequence_delta": sequence_delta,
        "records_started_delta": produced,
        "capacity": capacity,
        "selected_records": len(selected),
        "expected_records": expected,
        "overwrite_delta": overwrite_delta,
        "recoverable": False,
    }
    report["trace_interval"] = interval
    if capacity <= 0:
        add_failure(report, "trace capacity is absent or invalid")
        return
    if sequence_delta == 0:
        add_failure(report, "trace interval has no post-baseline record updates")
        return
    if produced == 0:
        add_failure(report, "trace interval has no new records")
        return
    if produced > capacity:
        add_failure(
            report,
            f"trace interval produced {produced} records, exceeding ring capacity {capacity}",
        )
        return
    if produced != expected:
        add_failure(
            report,
            f"trace interval has {produced} records; expected exactly {expected}",
        )
        return
    if len(selected) != produced:
        add_failure(
            report,
            f"trace interval recovered {len(selected)} of {produced} produced records",
        )
        return
    record_sequences = [as_int(record.get("Sequence"), "trace sequence") for record in selected]
    if len(set(record_sequences)) != len(record_sequences):
        add_failure(report, "trace interval has duplicate record sequences")
        return
    if any(not newer_u32(sequence, baseline) for sequence in record_sequences):
        add_failure(report, "trace interval includes a pre-baseline record")
        return
    interval["recoverable"] = True
    if overwrite_delta:
        report["warnings"].append(
            "trace ring overwrote "
            f"{overwrite_delta} historical record(s), but the selected interval is complete"
        )


def last_snapshot(document: dict[str, Any], label: str) -> dict[str, Any]:
    values = document.get("Snapshots")
    if not isinstance(values, list) or not values or not isinstance(values[-1], dict):
        raise CampaignError(f"{label} has no snapshots")
    return values[-1]


def elf_hash(document: dict[str, Any], core: str) -> str | None:
    candidates = (
        f"Elf.{core}.Sha256", f"Elf.{core.lower()}.Sha256",
        "Elf.Sha256" if core in {"Cpu0", "Cpu1"} else "",
    )
    for key in candidates:
        if not key:
            continue
        value = nested(document, key)
        if isinstance(value, str):
            normalized = value.upper()
            if SHA256_RE.fullmatch(normalized):
                return normalized
    return None


def counter_delta(before: dict[str, Any], after: dict[str, Any], path: str) -> int:
    return u32_delta(as_int(nested(after, path, 0), path), as_int(nested(before, path, 0), path))


def add_failure(report: dict[str, Any], text: str) -> None:
    report["failures"].append(text)


def validate_hashes(
    report: dict[str, Any], manifest: dict[str, Any], documents: Iterable[tuple[str, dict[str, Any]]]
) -> None:
    expected0 = str(nested(manifest, "firmware.cpu0.sha256", "")).upper()
    expected1 = str(nested(manifest, "firmware.cpu1.sha256", "")).upper()
    if not SHA256_RE.fullmatch(expected0) or not SHA256_RE.fullmatch(expected1):
        add_failure(report, "manifest does not bind valid CPU0/CPU1 ELF SHA-256 values")
        return
    for label, document in documents:
        if label != "cpu1_campaign":
            actual0 = elf_hash(document, "Cpu0")
            if actual0 != expected0:
                add_failure(report, f"{label} CPU0 ELF hash does not match manifest")
        if label.endswith("runtime") or label == "cpu1_campaign":
            actual1 = elf_hash(document, "Cpu1")
            if actual1 != expected1:
                add_failure(report, f"{label} CPU1 ELF hash does not match manifest")


def validate_sdr_artifacts(
    report: dict[str, Any], manifest: dict[str, Any], management_log: Path
) -> None:
    hashes: dict[str, str] = {}
    for name in ("agent", "adapter"):
        value = str(nested(manifest, f"sdr_artifacts.{name}.sha256", "")).upper()
        if not SHA256_RE.fullmatch(value):
            add_failure(report, f"manifest does not bind the SDR {name} SHA-256")
        else:
            hashes[name] = value
    if nested(manifest, "links.management.sdr_agent_location") != "/tmp":
        add_failure(report, "SDR agent deployment is not explicitly bound to /tmp")
    lines = read_text(management_log).upper().splitlines()
    for name, value in hashes.items():
        if not any((value in line) and ("/TMP/" in line) for line in lines):
            add_failure(
                report,
                f"management log does not prove /tmp SDR {name} hash {value}",
            )


def validate_network(
    report: dict[str, Any], before: dict[str, Any], after: dict[str, Any], clean: bool
) -> None:
    if not bool_value(nested(after, "Phy.LinkUp", False)):
        add_failure(report, "PHY is not link-up in the post-run SWD snapshot")
    local_1000 = as_int(nested(after, "Phy.GigabitControl", 0), "GigabitControl") & 0x0300
    peer_1000 = as_int(nested(after, "Phy.GigabitStatus", 0), "GigabitStatus") & 0x0C00
    payload_mbps = as_int(nested(after, "Iq.MbpsX1000", 0), "Iq.MbpsX1000") / 1000.0
    one_gig_derived = bool(local_1000 and peer_1000 and payload_mbps > 100.0)
    report["network"]["link"] = {
        "status": "derived" if one_gig_derived else "missing",
        "value_mbps": 1000 if one_gig_derived else None,
        "basis": "measured MDIO reg9/reg10 capability plus measured IQ payload above 100 Mbps",
        "local_gigabit_control": as_int(nested(after, "Phy.GigabitControl", 0), "reg9"),
        "peer_gigabit_status": as_int(nested(after, "Phy.GigabitStatus", 0), "reg10"),
    }
    if not one_gig_derived:
        add_failure(report, "1 Gbps runtime link is not proven by MDIO capability and payload evidence")
    if as_int(nested(after, "Iq.CrcBackend", 0), "CrcBackend") != 2:
        add_failure(report, "RA8P1 hardware CRC32C backend 2 is not active")
    if as_int(nested(after, "Iq.CrcHardwareSelfTest", 0), "CrcHardwareSelfTest") != 1:
        add_failure(report, "RA8P1 hardware CRC32C self-test did not pass")
    if (as_int(nested(after, "Iq.CrcTimingFlags", 0), "CrcTimingFlags") & 3) != 3:
        add_failure(report, "CRC END-to-completion timing is incomplete")

    error_paths = (
        "Rmac.RxOverflow", "Rmac.RxErrorFrames", "Rmac.RxFcsErrorRaw",
        "Rmac.RxFragmentErrorRaw", "Rmac.DriverRxFail",
        "Rmac.DriverRxPbufAllocFail", "Rmac.IrqRxMessageLost",
        "Rmac.IrqErrorGlobal", "Rmac.LwipTcpipInpktAllocFail",
        "Rmac.LwipTcpipInpktMboxFail", "Ring.FullDrops", "Ring.OversizeDrops",
    )
    deltas = {path: counter_delta(before, after, path) for path in error_paths}
    report["network"]["counter_deltas"] = deltas
    if clean:
        for path, value in deltas.items():
            if value != 0:
                add_failure(report, f"clean run {path} increased by {value}")
    report["network"]["pause_deltas"] = {
        "automatic_pause_tx": counter_delta(before, after, "Rmac.MacAutoPauseTx"),
        "pause_rx": counter_delta(before, after, "Rmac.MacPauseRx"),
    }


def validate_trace(
    report: dict[str, Any], selected: Sequence[dict[str, Any]], spec: dict[str, Any]
) -> None:
    expected = as_int(spec["expected_windows"], "expected windows")
    if len(selected) < expected:
        add_failure(report, f"only {len(selected)} new trace records; expected {expected}")
        return
    records = list(selected[:expected])
    report["records_used"] = len(records)
    expected_centers = [as_int(value, "expected center") for value in spec["expected_center_sequence"]]
    actual_centers: list[int] = []
    sessions: set[int] = set()
    requests: set[int] = set()
    stft: list[float] = []
    npu: list[float] = []
    request_to_npu: list[float] = []
    first_to_npu: list[float] = []
    capture_upper: list[float] = []
    tune: list[float] = []
    capture: list[float] = []
    network_window: list[float] = []
    crc_after_end: list[float] = []
    npu_to_cpu1: list[float] = []
    request_to_ack: list[float] = []
    request_to_ready: list[float] = []
    ready_to_iqsc_start: list[float] = []
    iqsc_start_to_complete: list[float] = []
    complete_to_credit: list[float] = []
    ack_to_credit: list[float] = []
    request_to_iqsc_start: list[float] = []
    rates: list[float] = []
    tick_rates: list[float] = []
    cpu_load: list[int] = []
    pacing_target = as_float(spec["target_mbps"], "target_mbps")
    minimum_payload_raw = spec.get("minimum_payload_mbps")
    minimum_payload = (
        as_float(minimum_payload_raw, "minimum_payload_mbps")
        if minimum_payload_raw is not None else None
    )
    if minimum_payload is not None and minimum_payload <= 0.0:
        add_failure(report, "minimum_payload_mbps must be positive when specified")
    for index, record in enumerate(records):
        session_id = as_int(record.get("SessionId"), "SessionId")
        request_id = as_int(record.get("RequestId"), "RequestId")
        sessions.add(session_id)
        requests.add(request_id)
        center = as_int(record.get("CenterIndex"), "CenterIndex")
        actual_centers.append(center)
        if as_int(record.get("SampleCount"), "SampleCount") != WINDOW_SAMPLES:
            add_failure(report, f"session {session_id} sample count is not {WINDOW_SAMPLES}")
        flags = as_int(record.get("Flags"), "Flags")
        if (flags & TRACE_COMPLETE_FLAGS) != TRACE_COMPLETE_FLAGS:
            add_failure(report, f"session {session_id} lacks a complete request/CRC/ACK/STFT/NPU/CPU1 trace")
        if index > 0 and (flags & TRACE_CAPTURE_READY_FLAG) == 0:
            add_failure(report, f"prefetched session {session_id} lacks CAPTURE_READY timing")
        if as_int(record.get("Status", 0), "Status") != 0:
            add_failure(report, f"session {session_id} final status is not OK")
        for field in (
            "SequenceGaps", "Reordered", "InvalidPackets", "RingFullDrops",
            "RingOversizeDrops",
        ):
            if as_int(record.get(field, 0), field) != 0:
                add_failure(report, f"session {session_id} {field} is nonzero")
        packet_value = record.get("FirstToLastPacketMs")
        packet_ms = (
            as_float(packet_value, "FirstToLastPacketMs")
            if packet_value is not None else 0.0
        )
        if packet_ms <= 0.0:
            add_failure(report, f"session {session_id} has no positive DWT packet interval")
            rate = 0.0
        else:
            rate = WINDOW_PAYLOAD_BYTES * 8.0 / (packet_ms * 1000.0)
        rates.append(rate)
        tick_rates.append(
            as_int(record.get("PayloadMbpsX1000", 0), "PayloadMbpsX1000") / 1000.0
        )
        if minimum_payload is not None and rate < minimum_payload:
            add_failure(
                report,
                f"session {session_id} measured {rate:.3f} Mbps below explicit minimum {minimum_payload:.3f}",
            )
        load = as_int(record.get("Cpu0LoadPermille", 0), "Cpu0LoadPermille")
        cpu_load.append(load)
        if load == 0:
            add_failure(report, f"session {session_id} has no nonzero CPU0 load measurement")
        elif load > 1000:
            add_failure(report, f"session {session_id} CPU0 load permille is invalid: {load}")
        for field, target_list in (
            ("StftMs", stft), ("NpuMs", npu),
            ("RequestToNpuResultMs", request_to_npu),
            ("FirstPacketToNpuResultMs", first_to_npu),
            ("CaptureStartToNpuUpperMs", capture_upper),
            ("RemoteTuneMs", tune),
            ("RemoteCaptureMs", capture),
            ("FirstToLastPacketMs", network_window),
            ("LastPacketToCrcCompleteMs", crc_after_end),
            ("NpuToCpu1VisibleUpperMs", npu_to_cpu1),
            ("RequestToAckMs", request_to_ack),
            ("IqscStartToCaptureCompleteMs", iqsc_start_to_complete),
            ("CaptureCompleteToCreditAcceptedMs", complete_to_credit),
            ("AckToCreditAcceptedMs", ack_to_credit),
            ("RequestToIqscStartMs", request_to_iqsc_start),
        ):
            value = record.get(field)
            if value is None:
                add_failure(report, f"session {session_id} lacks {field}")
            else:
                target_list.append(as_float(value, field))
        for field, target_list in (
            ("RequestToCaptureReadyMs", request_to_ready),
            ("CaptureReadyToIqscStartMs", ready_to_iqsc_start),
        ):
            value = record.get(field)
            if value is None:
                if index > 0:
                    add_failure(report, f"prefetched session {session_id} lacks {field}")
            else:
                target_list.append(as_float(value, field))
        for raw_field in (
            "RequestTxCycles", "AckTxCycles", "NpuEndCycles",
            "Cpu1VisibleCycles", "CaptureCompleteCycles", "CreditAcceptedCycles",
            "IqscStartCycles",
        ):
            if record.get(raw_field) is None:
                add_failure(report, f"trace collector lacks raw field {raw_field}")
        if index > 0 and as_int(
            record.get("CaptureReadyCycles", 0), "CaptureReadyCycles"
        ) == 0:
            add_failure(report, f"prefetched session {session_id} has no CaptureReadyCycles")
    if actual_centers != expected_centers:
        add_failure(report, f"center order {actual_centers} does not match {expected_centers}")
    if len(sessions) != len(records) or 0 in sessions:
        add_failure(report, "session IDs are zero or reused within the selected run")
    if len(requests) != len(records) or 0 in requests:
        add_failure(report, "request IDs are zero or reused within the selected run")

    perf = report["performance"]
    perf["iq_payload_mbps"] = {
        "status": "measured", **series_summary(rates),
        "scope": "CPU0 first-data to last-data DWT interval",
        "basis": "2,361,344 payload bytes / measured 1 GHz DWT interval",
        "pacing_target_mbps": pacing_target,
        "minimum_required_mbps": minimum_payload,
    }
    perf["iq_payload_tick_mbps"] = {
        "status": "measured", **series_summary(tick_rates),
        "scope": "CPU0 RTOS tick telemetry",
        "warning": "millisecond quantization; diagnostic only",
    }
    perf["iq_800_plus_proven"] = {
        "status": "measured",
        "value": bool(rates and statistics.median(rates) >= 800.0),
        "basis": "median payload Mbps across selected hardware windows",
    }
    perf["stft_ms"] = {"status": "measured", **series_summary(stft)}
    perf["npu_ms"] = {"status": "measured", **series_summary(npu)}
    perf["sdr_tune_ms"] = {
        "status": "measured", **series_summary(tune), "clock": "SDR monotonic clock",
    }
    perf["sdr_capture_ms"] = {
        "status": "measured", **series_summary(capture), "clock": "SDR monotonic clock",
    }
    perf["first_to_last_packet_ms"] = {
        "status": "measured", **series_summary(network_window), "clock": "CPU0 DWT 1 GHz",
    }
    perf["last_packet_to_crc_complete_ms"] = {
        "status": "measured", **series_summary(crc_after_end), "clock": "CPU0 DWT 1 GHz",
    }
    perf["request_to_npu_result_ms"] = {
        "status": "measured", **series_summary(request_to_npu),
        "clock": "CPU0 DWT 1 GHz",
    }
    perf["first_packet_to_npu_result_ms"] = {
        "status": "measured", **series_summary(first_to_npu),
        "clock": "CPU0 DWT 1 GHz",
    }
    perf["capture_start_to_npu_upper_ms"] = {
        "status": "derived", **series_summary(capture_upper),
        "basis": "CPU0 request->NPU minus SDR request_rx->capture_start; includes unknown one-way control latency",
    }
    perf["npu_to_cpu1_visible_upper_ms"] = {
        "status": "measured", **series_summary(npu_to_cpu1), "clock": "CPU0 DWT 1 GHz",
    }
    perf["request_to_ack_ms"] = {
        "status": "measured", **series_summary(request_to_ack), "clock": "CPU0 DWT 1 GHz",
    }
    perf["request_to_capture_ready_ms"] = {
        "status": "measured", **series_summary(request_to_ready), "clock": "CPU0 DWT 1 GHz",
    }
    perf["capture_ready_to_iqsc_start_ms"] = {
        "status": "measured", **series_summary(ready_to_iqsc_start), "clock": "CPU0 DWT 1 GHz",
    }
    perf["iqsc_start_to_capture_complete_ms"] = {
        "status": "measured", **series_summary(iqsc_start_to_complete), "clock": "CPU0 DWT 1 GHz",
    }
    perf["capture_complete_to_credit_accepted_ms"] = {
        "status": "measured", **series_summary(complete_to_credit), "clock": "CPU0 DWT 1 GHz",
    }
    perf["ack_to_credit_accepted_ms"] = {
        "status": "measured", **series_summary(ack_to_credit), "clock": "CPU0 DWT 1 GHz",
    }
    perf["request_to_iqsc_start_ms"] = {
        "status": "measured", **series_summary(request_to_iqsc_start), "clock": "CPU0 DWT 1 GHz",
    }
    perf["cpu0_load_permille"] = {"status": "measured", **series_summary(cpu_load)}

    request_relative_to_ack: list[float] = []
    ready_relative_to_ack: list[float] = []
    ack_to_next_start: list[float] = []
    credit_to_next_start: list[float] = []
    npu_to_next_start: list[float] = []
    for previous, following in zip(records, records[1:]):
        previous_ack = as_int(previous["AckTxCycles"], "AckTxCycles")
        previous_credit = as_int(
            previous["CreditAcceptedCycles"], "CreditAcceptedCycles"
        )
        previous_npu = as_int(previous["NpuEndCycles"], "NpuEndCycles")
        following_request = as_int(following["RequestTxCycles"], "RequestTxCycles")
        following_ready = as_int(following["CaptureReadyCycles"], "CaptureReadyCycles")
        following_start = as_int(following["IqscStartCycles"], "IqscStartCycles")
        request_relative_to_ack.append(
            signed_cycle_ms(previous_ack, following_request)
        )
        ready_relative_to_ack.append(
            signed_cycle_ms(previous_ack, following_ready)
        )
        ack_handoff = signed_cycle_ms(previous_ack, following_start)
        credit_handoff = signed_cycle_ms(previous_credit, following_start)
        npu_handoff = signed_cycle_ms(previous_npu, following_start)
        if ack_handoff < 0.0:
            add_failure(report, "next IQSC START precedes the previous WINDOW_ACK")
        ack_to_next_start.append(ack_handoff)
        credit_to_next_start.append(credit_handoff)
        npu_to_next_start.append(npu_handoff)
    perf["next_request_relative_to_previous_ack_ms"] = {
        "status": "measured" if request_relative_to_ack else "missing",
        **series_summary(request_relative_to_ack),
        "clock": "CPU0 DWT 1 GHz",
        "basis": "signed next request TX minus previous ACK; negative proves prefetch",
    }
    perf["next_ready_relative_to_previous_ack_ms"] = {
        "status": "measured" if ready_relative_to_ack else "missing",
        **series_summary(ready_relative_to_ack),
        "clock": "CPU0 DWT 1 GHz",
        "basis": "signed next CAPTURE_READY minus previous ACK; negative means SDR preparation overlapped inference",
    }
    perf["previous_ack_to_next_iqsc_start_ms"] = {
        "status": "measured" if ack_to_next_start else "missing",
        **series_summary(ack_to_next_start),
        "clock": "CPU0 DWT 1 GHz",
        "basis": "successive-window credit handoff from previous ACK TX to next IQSC START",
    }
    perf["previous_credit_to_next_iqsc_start_ms"] = {
        "status": "measured" if credit_to_next_start else "missing",
        **series_summary(credit_to_next_start),
        "clock": "CPU0 DWT 1 GHz",
        "basis": "previous CREDIT_ACCEPTED RX to next IQSC START; negative means IQSC START itself proved the earlier ACK credit before the sparse control response arrived",
    }
    perf["previous_npu_to_next_iqsc_start_ms"] = {
        "status": "measured" if npu_to_next_start else "missing",
        **series_summary(npu_to_next_start),
        "clock": "CPU0 DWT 1 GHz",
        "basis": "previous NPU end to next IQSC START",
    }

    if all(record.get("NpuEndCycles") is not None for record in records) and len(records) >= 2:
        intervals = [
            cycle_ms(
                as_int(records[index - 1]["NpuEndCycles"], "NpuEndCycles"),
                as_int(records[index]["NpuEndCycles"], "NpuEndCycles"),
            )
            for index in range(1, len(records))
        ]
        elapsed_ms = sum(intervals)
        fps = (len(records) - 1) * 1000.0 / elapsed_ms if elapsed_ms > 0 else 0.0
        perf["steady_inference_fps"] = {
            "status": "measured",
            "value": round(fps, 6),
            "samples": len(records),
            "basis": "successive CPU0 NPU-end DWT timestamps with uint32 wrap handling",
        }

    coverage: list[float] = []
    for offset in range(0, len(records) - 3, 4):
        group = records[offset:offset + 4]
        if [as_int(item["CenterIndex"], "CenterIndex") for item in group] != [0, 1, 2, 3]:
            continue
        if group[0].get("RequestTxCycles") is None or group[3].get("NpuEndCycles") is None:
            continue
        coverage.append(cycle_ms(
            as_int(group[0]["RequestTxCycles"], "RequestTxCycles"),
            as_int(group[3]["NpuEndCycles"], "NpuEndCycles"),
        ))
    perf["four_frequency_coverage_ms"] = {
        "status": "measured" if coverage else "missing",
        **series_summary(coverage),
        "basis": "CPU0 first request TX through fourth-center NPU end",
    }


def matching_events(
    events: Sequence[dict[str, Any]], kind: str, request: int, session: int
) -> list[dict[str, Any]]:
    return [
        event for event in events
        if event["kind"] == kind and event.get("request") == request and
        event.get("session") == session
    ]


def agent_capture_receipts(events: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return events which prove that the SDR agent owns a capture request.

    New agents trace the received CAPTURE_REQ directly.  Older deployed agents
    emit only the successful CAPTURE_ACCEPTED response, which still carries the
    exact request/session identity and is a valid proof that the agent received
    and accepted that request.
    """
    return [
        event for event in events
        if (
            event["values"].get("direction") == "rx" and
            event["values"].get("event") == "CAPTURE_REQ"
        ) or (
            event["values"].get("direction") == "tx" and
            event["values"].get("event") == "CAPTURE_ACCEPTED" and
            event.get("status") == 0
        )
    ]


def validate_agent(
    report: dict[str, Any], records: Sequence[dict[str, Any]], events: Sequence[dict[str, Any]],
    spec: dict[str, Any]
) -> None:
    fault = str(spec.get("fault", "none"))
    selected = list(records[:as_int(spec["expected_windows"], "expected_windows")])
    all_selected_sessions = {as_int(item["SessionId"], "SessionId") for item in selected}
    adapter_block_setup: list[float] = []
    adapter_dma_wait: list[float] = []
    adapter_disable: list[float] = []
    adapter_copy: list[float] = []
    sender_crc_backends: set[str] = set()
    for record in selected:
        request = as_int(record["RequestId"], "RequestId")
        session = as_int(record["SessionId"], "SessionId")
        control = matching_events(events, "control", request, session)
        windows = matching_events(events, "window", request, session)
        capture_req = agent_capture_receipts(control)
        ack = [
            event for event in control
            if event["values"].get("direction") == "rx" and
            event["values"].get("event") == "WINDOW_ACK"
        ]
        credit = [
            event for event in control
            if event["values"].get("direction") == "tx" and
            event["values"].get("event") == "CREDIT_ACCEPTED"
        ]
        complete = [event for event in windows if event["values"].get("event") == "complete"]
        if not capture_req:
            add_failure(report, f"SDR log does not prove agent receipt for {request}/{session}")
        if not complete:
            add_failure(report, f"SDR log does not show completed capture/send for {request}/{session}")
        if not any(event.get("status") == 0 for event in ack):
            add_failure(report, f"SDR log does not show final OK WINDOW_ACK for {request}/{session}")
        if not credit:
            add_failure(report, f"SDR log does not show CREDIT_ACCEPTED for {request}/{session}")
        latest = max(complete, key=lambda event: event.get("attempt", -1), default=None)
        if latest:
            if latest.get("samples") != WINDOW_SAMPLES:
                add_failure(report, f"SDR window {request}/{session} sample count mismatch")
            if latest["values"].get("transport") != "udp_gso":
                add_failure(report, f"SDR window {request}/{session} did not use UDP GSO")
            if latest.get("gso_batches", 0) <= 0 or latest.get("gso_fallbacks", 0) != 0:
                add_failure(report, f"SDR window {request}/{session} GSO batches/fallbacks invalid")
            crc_backend = str(latest["values"].get("crc_backend", "")).lower()
            if crc_backend:
                sender_crc_backends.add(crc_backend)
            if crc_backend not in SENDER_CRC_BACKENDS:
                add_failure(
                    report,
                    f"SDR window {request}/{session} has unsupported sender CRC backend {crc_backend!r}",
                )
            sender_rate = latest.get("actual_mbps_x1000", 0) / 1000.0
            packet_value = record.get("FirstToLastPacketMs")
            packet_ms = (
                as_float(packet_value, "FirstToLastPacketMs")
                if packet_value is not None else 0.0
            )
            cpu0_rate = (
                WINDOW_PAYLOAD_BYTES * 8.0 / (packet_ms * 1000.0)
                if packet_ms > 0.0 else 0.0
            )
            tolerance = max(1.0, sender_rate * 0.02)
            if sender_rate <= 0.0 or abs(sender_rate - cpu0_rate) > tolerance:
                add_failure(
                    report,
                    f"SDR/CPU0 payload rates disagree for {request}/{session}: "
                    f"{sender_rate:.3f}/{cpu0_rate:.3f} Mbps",
                )
            # The human-readable adapter name contains spaces, while the KV
            # parser intentionally stops values at whitespace. Match the raw
            # trace so a formal mmap run cannot silently omit stage timing.
            if "block+mmap" in latest["raw"]:
                for field, target in (
                    ("adapter_block_setup_us", adapter_block_setup),
                    ("adapter_dma_wait_us", adapter_dma_wait),
                    ("adapter_disable_us", adapter_disable),
                    ("adapter_copy_us", adapter_copy),
                ):
                    value = latest.get(field)
                    if value is None:
                        add_failure(
                            report,
                            f"SDR mmap window {request}/{session} lacks {field}",
                        )
                    else:
                        target.append(float(value) / 1000.0)

    if adapter_dma_wait:
        report["performance"]["sdr_mmap_block_setup_ms"] = {
            "status": "measured", **series_summary(adapter_block_setup),
            "clock": "SDR monotonic clock",
        }
        report["performance"]["sdr_mmap_dma_wait_ms"] = {
            "status": "measured", **series_summary(adapter_dma_wait),
            "clock": "SDR monotonic clock",
        }
        report["performance"]["sdr_mmap_disable_ms"] = {
            "status": "measured", **series_summary(adapter_disable),
            "clock": "SDR monotonic clock",
        }
        report["performance"]["sdr_mmap_copy_ms"] = {
            "status": "measured", **series_summary(adapter_copy),
            "clock": "SDR monotonic clock",
        }

    selected_events = [
        event for event in events if event.get("session") in all_selected_sessions
    ]
    observed_mode: str | None = None
    overlap_pairs = 0
    serial_pairs = 0
    for previous, following in zip(selected, selected[1:]):
        prev_request = as_int(previous["RequestId"], "RequestId")
        prev_session = as_int(previous["SessionId"], "SessionId")
        next_request = as_int(following["RequestId"], "RequestId")
        next_session = as_int(following["SessionId"], "SessionId")
        prev_ack_lines = [
            event["line"] for event in selected_events
            if event.get("request") == prev_request and event.get("session") == prev_session and
            event["kind"] == "control" and event["values"].get("direction") == "rx" and
            event["values"].get("event") == "WINDOW_ACK" and event.get("status") == 0
        ]
        prev_credit_lines = [
            event["line"] for event in selected_events
            if event.get("request") == prev_request and event.get("session") == prev_session and
            event["kind"] == "control" and event["values"].get("direction") == "tx" and
            event["values"].get("event") == "CREDIT_ACCEPTED"
        ]
        next_control = [
            event for event in selected_events
            if event.get("request") == next_request and event.get("session") == next_session and
            event["kind"] == "control"
        ]
        next_req_lines = [event["line"] for event in agent_capture_receipts(next_control)]
        if not prev_ack_lines or not next_req_lines:
            continue
        if min(next_req_lines) < min(prev_ack_lines):
            overlap_pairs += 1
        elif prev_credit_lines and min(next_req_lines) > min(prev_credit_lines):
            serial_pairs += 1
    expected_mode = str(spec.get("pipeline_mode", ""))
    pair_count = max(0, len(selected) - 1)
    expected_overlap_pairs = 0
    expected_serial_pairs = pair_count
    if expected_mode == "overlap":
        expected_overlap_pairs = pair_count
        expected_serial_pairs = 0
    unclassified_pairs = pair_count - overlap_pairs - serial_pairs
    if expected_mode == "overlap":
        if overlap_pairs == expected_overlap_pairs and serial_pairs == expected_serial_pairs:
            observed_mode = "overlap"
        elif overlap_pairs or serial_pairs:
            observed_mode = "mixed"
        if unclassified_pairs or overlap_pairs != expected_overlap_pairs or serial_pairs != expected_serial_pairs:
            add_failure(
                report,
                "agent control order proves "
                f"overlap/serial/unclassified={overlap_pairs}/{serial_pairs}/{unclassified_pairs}, "
                f"expected {expected_overlap_pairs}/{expected_serial_pairs}/0",
            )
    elif expected_mode == "serial":
        if serial_pairs == pair_count and overlap_pairs == 0:
            observed_mode = "serial"
        elif overlap_pairs or serial_pairs:
            observed_mode = "mixed"
        if unclassified_pairs or overlap_pairs or serial_pairs != pair_count:
            add_failure(
                report,
                "agent control order proves "
                f"overlap/serial/unclassified={overlap_pairs}/{serial_pairs}/{unclassified_pairs}, "
                f"expected 0/{pair_count}/0",
            )
    elif pair_count:
        add_failure(report, f"unsupported expected pipeline mode {expected_mode!r}")
    report["control"]["observed_pipeline_mode"] = observed_mode
    report["control"]["overlap_pairs"] = overlap_pairs
    report["control"]["serial_pairs"] = serial_pairs
    report["control"]["unclassified_pairs"] = unclassified_pairs
    report["control"]["expected_overlap_pairs"] = expected_overlap_pairs
    report["control"]["expected_serial_pairs"] = expected_serial_pairs
    report.setdefault("sdr", {})["sender_crc_backends"] = sorted(sender_crc_backends)

    if fault != "none" and selected:
        first = selected[0]
        request = as_int(first["RequestId"], "RequestId")
        session = as_int(first["SessionId"], "SessionId")
        matched = matching_events(events, "control", request, session)
        windows = matching_events(events, "window", request, session)
        flags = as_int(first.get("Flags"), "Flags")
        if fault in {"crc", "drop", "request-timeout"} and (flags & TRACE_RETRY_FLAG) == 0:
            add_failure(report, f"{fault} run did not set the CPU0 retry trace flag")
        if fault in {"crc", "drop"} and max((event.get("attempt", 0) for event in windows), default=0) < 1:
            add_failure(report, f"{fault} run did not retransmit the cached window")
        if fault == "request-timeout":
            req_attempts = [
                event.get("attempt", 0) for event in matched
                if event in agent_capture_receipts(matched)
            ]
            if max(req_attempts, default=0) < 1:
                add_failure(report, "request-timeout run did not retry CAPTURE_REQ")
        if fault == "ack-timeout":
            ack_attempts = [
                event.get("attempt", 0) for event in matched
                if event["values"].get("event") == "WINDOW_ACK" and
                event["values"].get("direction") == "rx"
            ]
            if max(ack_attempts, default=0) < 1:
                add_failure(report, "ack-timeout run did not retry WINDOW_ACK")
            if len([event for event in windows if event["values"].get("event") == "complete"]) != 1:
                add_failure(report, "ack-timeout run recaptured data instead of applying ACK idempotently")
        if fault == "duplicate-request":
            attempts: dict[int, int] = {}
            for event in agent_capture_receipts(matched):
                attempt = event.get("attempt", 0)
                attempts[attempt] = attempts.get(attempt, 0) + 1
            if max(attempts.values(), default=0) < 2:
                add_failure(report, "duplicate-request run has no duplicated identical CAPTURE_REQ")
            if len([event for event in windows if event["values"].get("event") == "complete"]) != 1:
                add_failure(report, "duplicate CAPTURE_REQ caused more than one capture/send")


def campaign_proof_payload(document: dict[str, Any]) -> dict[str, Any] | None:
    """Extract a CPU1 campaign proof from ReadStatus or a saved summary."""
    for key in ("Proof", "proof", "CampaignProof", "campaign_proof", "Campaign", "campaign", "Summary", "summary"):
        value = document.get(key)
        if isinstance(value, dict) and (
            "WindowsVisible" in value or "windows_visible" in value
        ):
            return value
    if "WindowsVisible" in document or "windows_visible" in document:
        return document
    return None


def proof_value(proof: dict[str, Any], *names: str, default: Any = None) -> Any:
    for name in names:
        if name in proof:
            return proof[name]
    return default


def expected_campaign_mode(spec: dict[str, Any]) -> tuple[int, str]:
    centers = [as_int(value, "center index") for value in spec["expected_center_sequence"]]
    if centers and all(center == centers[0] for center in centers):
        return 2, "SINGLE"
    if str(spec.get("pipeline_mode")) == "serial":
        return 4, "FOUR_SERIAL"
    return 3, "FOUR_OVERLAP"


def validate_campaign_proof(
    report: dict[str, Any], document: dict[str, Any], selected: Sequence[dict[str, Any]],
    spec: dict[str, Any], runtime_status: dict[str, Any],
) -> bool:
    proof = campaign_proof_payload(document)
    if proof is None:
        add_failure(report, "CPU1 campaign proof has no proof/summary payload")
        return False
    expected_windows = as_int(spec["expected_windows"], "expected windows")
    visible = as_int(
        proof_value(proof, "WindowsVisible", "windows_visible"), "CPU1 proof WindowsVisible"
    )
    proof_expected = as_int(
        proof_value(proof, "WindowsExpected", "windows_expected"), "CPU1 proof WindowsExpected"
    )
    state = as_int(proof_value(proof, "State", "state", default=0), "CPU1 proof State")
    state_name = str(proof_value(proof, "StateName", "state_name", default="")).upper()
    complete = bool_value(proof_value(proof, "Complete", "complete", default=False))
    failed = bool_value(proof_value(proof, "Failed", "failed", default=False))
    expected_mode, expected_mode_name = expected_campaign_mode(spec)
    mode = as_int(proof_value(proof, "Mode", "mode", default=0), "CPU1 proof Mode")
    mode_name = str(proof_value(proof, "ModeName", "mode_name", default="")).upper()
    last_session = as_int(
        proof_value(proof, "LastSessionId", "last_session_id", default=0),
        "CPU1 proof LastSessionId",
    )
    last_center = as_int(
        proof_value(proof, "LastResultCenterIndex", "last_result_center_index", default=0),
        "CPU1 proof LastResultCenterIndex",
    )
    last_error = as_int(proof_value(proof, "LastError", "last_error", default=0), "CPU1 proof LastError")
    terminal_complete = str(
        proof_value(proof, "TerminalMagic", "terminal_magic", default="")
    ).upper() == "0X454E4F44"
    if proof_expected != expected_windows:
        add_failure(report, f"CPU1 proof expects {proof_expected} windows, not {expected_windows}")
    if visible < expected_windows:
        add_failure(report, f"CPU1 proof reports only {visible}/{expected_windows} visible windows")
    if failed or last_error != 0:
        add_failure(report, f"CPU1 campaign proof failed with error {last_error}")
    if not (complete or terminal_complete or state == 6 or state_name == "COMPLETE"):
        add_failure(report, "CPU1 campaign proof is not complete")
    if mode != expected_mode and mode_name != expected_mode_name:
        add_failure(
            report,
            f"CPU1 campaign proof mode {mode_name or mode} does not match {expected_mode_name}",
        )
    if selected:
        last_record = selected[-1]
        expected_session = as_int(last_record["SessionId"], "SessionId")
        expected_center = as_int(last_record["CenterIndex"], "CenterIndex")
        if last_session != expected_session:
            add_failure(
                report,
                f"CPU1 proof last session {last_session} does not match selected {expected_session}",
            )
        if last_center != expected_center:
            add_failure(
                report,
                f"CPU1 proof last center {last_center} does not match selected {expected_center}",
            )
    report["cpu1"] = {
        "source": "campaign-proof",
        "headless": bool_value(runtime_status.get("Headless", False)),
        "runtime_snapshot_valid": bool_value(runtime_status.get("Valid", False)),
        "windows_expected": proof_expected,
        "windows_visible": visible,
        "state": state,
        "state_name": state_name,
        "mode": mode,
        "mode_name": mode_name,
        "last_session_id": last_session,
        "last_center_index": last_center,
        "last_error": last_error,
        "model_placeholder": bool_value(runtime_status.get("ModelPlaceholder", False)),
    }
    return True


def validate_runtime(
    report: dict[str, Any], runtime: dict[str, Any], selected: Sequence[dict[str, Any]],
    spec: dict[str, Any], campaign_document: dict[str, Any],
) -> None:
    snapshot = last_snapshot(runtime, "runtime evidence")
    status = snapshot.get("Runtime", {})
    # This CPU1-owned proof is published only after the exact CPU0 result is
    # consumed. Lifetime runtime counters cannot identify the accepted window.
    validate_campaign_proof(report, campaign_document, selected, spec, status)


def verify_scenario(manifest_path: Path) -> dict[str, Any]:
    manifest = read_json(manifest_path)
    spec = manifest.get("scenario")
    if not isinstance(spec, dict):
        raise CampaignError("manifest has no scenario object")
    scenario_dir = manifest_path.parent
    required = {
        "before_net": scenario_dir / "before" / "net.json",
        "before_trace": scenario_dir / "before" / "trace.json",
        "before_runtime": scenario_dir / "before" / "runtime.json",
        "after_net": scenario_dir / "after" / "net.json",
        "after_trace": scenario_dir / "after" / "trace.json",
        "after_runtime": scenario_dir / "after" / "runtime.json",
        "cpu1_campaign": scenario_dir / "after" / "cpu1_campaign.json",
        "agent_log": scenario_dir / "agent.log",
        "flash_log": scenario_dir / "flash.log",
        "management_log": scenario_dir / "management.log",
    }
    missing = [label for label, path in required.items() if not path.is_file()]
    if missing:
        raise CampaignError(f"scenario evidence is incomplete: {', '.join(missing)}")
    documents = {
        label: read_json(path) for label, path in required.items()
        if label not in {"agent_log", "flash_log", "management_log"}
    }
    report: dict[str, Any] = {
        "tool": "ra8p1-board-campaign",
        "tool_version": TOOL_VERSION,
        "scenario_id": spec.get("id"),
        "verified_utc": utc_now(),
        "classification": "hardware evidence; values retain measured/derived labels",
        "failures": [],
        "warnings": [],
        "records_used": 0,
        "network": {},
        "control": {},
        "sdr": {},
        "performance": {},
    }
    before_time = nested(manifest, "phases.before.captured_utc")
    after_time = nested(manifest, "phases.after.captured_utc")
    if not isinstance(before_time, str) or not isinstance(after_time, str) or before_time >= after_time:
        add_failure(report, "before/after phase timestamps are absent or out of order")
    validate_hashes(report, manifest, documents.items())
    validate_sdr_artifacts(report, manifest, required["management_log"])
    before_net = last_snapshot(documents["before_net"], "before network evidence")
    after_net = last_snapshot(documents["after_net"], "after network evidence")
    clean = str(spec.get("fault", "none")) == "none"
    validate_network(report, before_net, after_net, clean)

    baseline = as_int(nested(documents["before_trace"], "Trace.LatestSequence", 0), "LatestSequence")
    selected = trace_records(documents["after_trace"], baseline)
    expected = as_int(spec["expected_windows"], "expected_windows")
    before_boot = as_int(nested(documents["before_trace"], "Trace.BootCount", 0), "BootCount")
    after_boot = as_int(nested(documents["after_trace"], "Trace.BootCount", 0), "BootCount")
    if before_boot == 0 or before_boot != after_boot:
        add_failure(report, "CPU0 boot identity changed or was not recorded between phases")
    validate_trace_interval(
        report, documents["before_trace"], documents["after_trace"], selected, expected
    )
    validate_trace(report, selected, spec)
    events = parse_agent_log(required["agent_log"])
    validate_agent(report, selected[:expected], events, spec)
    validate_runtime(
        report,
        documents["after_runtime"],
        selected[:expected],
        spec,
        documents["cpu1_campaign"],
    )

    management_issue = nested(manifest, "links.management.issue")
    if management_issue:
        report["warnings"].append(
            f"development-PC management link issue (not runtime-link evidence): {management_issue}"
        )
    report["links"] = {
        "programming": {
            "status": "recorded" if nested(manifest, "links.programming.evidence") else "missing-log",
            "path": "development PC -> J-Link/SWD -> RA8P1",
        },
        "runtime": {
            "status": "measured" if not report["failures"] else "failed-evidence-gates",
            "path": "SDR 192.168.31.10 <-> RA8P1 192.168.31.20",
            "host_ping_used": False,
        },
        "management": {
            "status": "recorded" if nested(manifest, "links.management.evidence") else "not-recorded",
            "runtime_prerequisite": False,
        },
    }
    report["status"] = "PASS" if not report["failures"] else "FAIL"
    return report


def markdown_report(report: dict[str, Any]) -> str:
    perf = report.get("performance", {})
    lines = [
        f"# Board campaign: {report.get('scenario_id')}", "",
        f"Status: **{report.get('status')}**", "",
        "## Link separation", "",
        "- Programming: development PC -> J-Link/SWD -> RA8P1.",
        "- Runtime: SDR 192.168.31.10 <-> RA8P1 192.168.31.20; host ping is not used.",
        "- Management: development PC -> SDR shell for /tmp agent deployment only.", "",
        "## Performance", "",
        "| Metric | Classification | p50/value | p95 | max |",
        "|---|---|---:|---:|---:|",
    ]
    for key in (
        "iq_payload_mbps", "sdr_tune_ms", "sdr_capture_ms",
        "first_to_last_packet_ms", "last_packet_to_crc_complete_ms",
        "stft_ms", "npu_ms", "request_to_npu_result_ms",
        "first_packet_to_npu_result_ms", "capture_start_to_npu_upper_ms",
        "npu_to_cpu1_visible_upper_ms", "request_to_ack_ms",
        "request_to_capture_ready_ms", "capture_ready_to_iqsc_start_ms",
        "iqsc_start_to_capture_complete_ms",
        "capture_complete_to_credit_accepted_ms", "ack_to_credit_accepted_ms",
        "request_to_iqsc_start_ms",
        "next_request_relative_to_previous_ack_ms",
        "next_ready_relative_to_previous_ack_ms",
        "previous_ack_to_next_iqsc_start_ms",
        "previous_credit_to_next_iqsc_start_ms",
        "previous_npu_to_next_iqsc_start_ms",
        "steady_inference_fps", "four_frequency_coverage_ms",
    ):
        value = perf.get(key, {})
        if not isinstance(value, dict):
            continue
        primary = value.get("value", value.get("p50"))
        lines.append(
            f"| {key} | {value.get('status', 'missing')} | {primary} | "
            f"{value.get('p95')} | {value.get('max')} |"
        )
    lines.extend(["", "## Failures", ""])
    if report.get("failures"):
        lines.extend(f"- {item}" for item in report["failures"])
    else:
        lines.append("- None.")
    lines.extend(["", "## Warnings", ""])
    if report.get("warnings"):
        lines.extend(f"- {item}" for item in report["warnings"])
    else:
        lines.append("- None.")
    return "\n".join(lines) + "\n"


def verify_command(args: argparse.Namespace) -> int:
    manifest = Path(args.manifest).resolve()
    report = verify_scenario(manifest)
    if args.output_json:
        write_json(Path(args.output_json), report)
    if args.output_md:
        Path(args.output_md).write_text(markdown_report(report), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 1


def commands_command(args: argparse.Namespace) -> int:
    plan = read_json(Path(args.plan))
    validate_plan(plan)
    spec = get_scenario(plan, args.scenario)
    centers = [as_int(item, "center") for item in spec["expected_center_sequence"]]
    if centers and all(item == centers[0] for item in centers):
        trigger_action = "Single"
        trigger_iterations = len(centers)
        trigger_center = centers[0]
    else:
        trigger_action = (
            "FourSerial" if spec["pipeline_mode"] == "serial" else "FourOverlap"
        )
        trigger_iterations = len(centers) // len(CENTERS_HZ)
        trigger_center = 0
    trigger = (
        "& .\\tools\\ra8p1-cpu1-campaign.ps1 "
        f"-Action {trigger_action} -CenterIndex {trigger_center} "
        f"-Iterations {trigger_iterations} -PayloadMbps {spec['target_mbps']} "
        f"-FaultFlags {spec['test_fault_flags']} -Cpu1Elf <exact-cpu1.elf>"
    )
    print(json.dumps({
        "scenario": spec,
        "cpu1_request_contract": {
            "owner": "CPU1 high-level scheduler",
            "center_sequence": spec["expected_center_sequence"],
            "sample_rate_hz": SAMPLE_RATE_HZ,
            "bandwidth_hz": BANDWIDTH_HZ,
            "sample_count": WINDOW_SAMPLES,
            "target_payload_mbps_x1000": as_int(spec["target_mbps"], "target") * 1000,
            "test_fault_flags": spec["test_fault_flags"],
        },
        "cpu1_jlink_trigger": {
            "command": trigger,
            "probe_serial": "1082495494",
            "jlink_target": "R7KA8P1KF_CPU0",
            "symbol_source": "exact CPU1 ELF",
            "writes_cpu0_command_mailbox": False,
        },
        "sdr_agent": {
            "location": "/tmp/sdr_capture_agent",
            "required_environment": {
                "RA8P1_SDR_UDP_GSO": "1",
            },
            "sender_crc_backend": {
                "allowed_values": sorted(SENDER_CRC_BACKENDS),
                "selection": "explicit operator choice; record crc_backend in every SDRC window_trace",
                "cpu0_gate": "CrcBackend=2 and CrcHardwareSelfTest=1 remain mandatory",
            },
            "diagnostics": "enable and retain SDRC control_trace/window_trace",
            "persistent_install": False,
        },
        "collection_order": [
            "capture --phase before",
            "run cpu1_jlink_trigger; it writes only the CPU1 campaign control object",
            "wait for the requested completed-window count",
            "retrieve the SDR agent log over the independent management path",
            "capture --phase after --agent-log <path>",
            "verify --manifest <scenario>/manifest.json",
        ],
    }, indent=2))
    return 0


def self_test() -> int:
    plan = default_plan(800)
    validate_plan(plan)
    if len(plan["scenarios"]) != 14:
        raise CampaignError("self-test plan scenario count changed")
    expected = ((0x000F4140 - 0xFFFFFF00) & 0xFFFFFFFF) / 1_000_000.0
    if cycle_ms(0xFFFFFF00, 0x000F4140) != round(expected, 6):
        raise CampaignError("self-test DWT wrap arithmetic failed")
    if not newer_u32(2, 0xFFFFFFFE) or newer_u32(0xFFFFFFFE, 2):
        raise CampaignError("self-test sequence wrap arithmetic failed")
    print("Self-test passed: plan coverage, link separation, and uint32 timing helpers.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="write the complete campaign plan")
    init.add_argument("--output", required=True)
    init.add_argument("--production-rate-mbps", type=int, default=800)

    commands = sub.add_parser("commands", help="show the CPU1/SDR activation contract")
    commands.add_argument("--plan", required=True)
    commands.add_argument("--scenario", required=True)

    capture = sub.add_parser("capture", help="capture one before/after SWD evidence phase")
    capture.add_argument("--plan", required=True)
    capture.add_argument("--scenario", required=True)
    capture.add_argument("--phase", choices=("before", "after"), required=True)
    capture.add_argument("--evidence-root", required=True)
    capture.add_argument("--probe-serial", required=True)
    capture.add_argument("--cpu0-elf", required=True)
    capture.add_argument("--cpu1-elf", required=True)
    capture.add_argument("--sdr-agent-artifact")
    capture.add_argument("--sdr-adapter-artifact")
    capture.add_argument("--jlink-exe")
    capture.add_argument(
        "--management-method",
        choices=("same-subnet", "USB", "serial", "existing-remote-shell", "unavailable"),
        default="unavailable",
    )
    capture.add_argument("--management-issue")
    capture.add_argument("--agent-log")
    capture.add_argument("--flash-log")
    capture.add_argument("--management-log")
    capture.add_argument("--dry-run", action="store_true")

    verify = sub.add_parser("verify", help="verify a captured hardware scenario")
    verify.add_argument("--manifest", required=True)
    verify.add_argument("--output-json")
    verify.add_argument("--output-md")

    sub.add_parser("self-test", help="run offline structural tests")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "init":
        if args.production_rate_mbps not in range(1, 941):
            raise CampaignError("production rate must be 1..940 Mbps")
        plan = default_plan(args.production_rate_mbps)
        validate_plan(plan)
        write_json(Path(args.output), plan)
        print(f"wrote campaign plan: {Path(args.output).resolve()}")
        return 0
    if args.command == "commands":
        return commands_command(args)
    if args.command == "capture":
        return capture_phase(args)
    if args.command == "verify":
        return verify_command(args)
    if args.command == "self-test":
        return self_test()
    raise CampaignError(f"unknown command: {args.command}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CampaignError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
