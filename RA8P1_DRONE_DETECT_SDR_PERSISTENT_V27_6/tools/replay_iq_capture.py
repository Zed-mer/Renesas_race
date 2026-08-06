#!/usr/bin/env python3
"""Replay SDR-Dataset-Collector S16 IQ captures to the RA8P1.

The wire format is the stream protocol consumed by the current CPU0 fast
receiver: IPv4/UDP (default port 5003), a 32-byte little-endian IQ header,
and S16 little-endian interleaved I/Q payloads.  BEGIN and END are ordinary
IQ packets marked with STREAM_START/STREAM_END.  BEGIN carries the packed
68-byte IQSC stream configuration; no second control port is required.

This utility is intentionally host-side only.  It does not modify either
CPU project and can be used with ``--dry-run`` when the board is unavailable.
The ``synthetic`` command is a strict formal-session test: 60 MSPS, 56 MHz,
6,000,000 complex samples, 19 overlapping model slices, and one RX1 stream.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import secrets
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Iterator, Optional

try:
    import numpy as np
except ImportError:  # Slow replay and synthetic mode do not need NumPy.
    np = None  # type: ignore[assignment]


DEFAULT_CAPTURE_ROOT = Path(__file__).resolve().parent / "captures"

IQ_MAGIC = 0x5149504B  # "KPIQ" as a little-endian integer.
IQ_FORMAT_S16 = 1
IQ_HEADER = struct.Struct("<IIIIQII")
IQ_HEADER_BYTES = IQ_HEADER.size
IQ_UDP_PAYLOAD_BYTES = 1472
IQ_DATA_BYTES = IQ_UDP_PAYLOAD_BYTES - IQ_HEADER_BYTES
IQ_BYTES_PER_COMPLEX_SAMPLE = 4
DEFAULT_SAFE_PAYLOAD_MBPS = 80.0
FORMAL_SAMPLE_RATE_HZ = 60_000_000
FORMAL_BANDWIDTH_HZ = 56_000_000
FORMAL_SESSION_SAMPLES = 6_000_000
MODEL_WINDOW_SAMPLES = 590_336
MODEL_TILE_STRIDE_SAMPLES = 295_168
MODEL_TILE_COUNT = 19
FORMAL_CENTERS_HZ = (2_420_000_000, 2_464_000_000, 5_760_000_000, 5_816_000_000)

IQ_FLAG_SYNTHETIC = 1 << 0
IQ_FLAG_DISCONTINUITY = 1 << 1
IQ_FLAG_FREQUENCY_B = 1 << 2
IQ_FLAG_STREAM_START = 1 << 3
IQ_FLAG_STREAM_END = 1 << 4
IQ_FLAG_VALID_BITS_12 = 1 << 5

IQSC_MAGIC = 0x49515343  # "IQSC" as a little-endian integer.
IQSC_VERSION = 2
# packed/aligned(4) C struct; center_frequency_hz starts at byte offset 20.
IQSC = struct.Struct("<IHHIIIQIIIIIIIIII")

assert IQ_HEADER_BYTES == 32
assert IQSC.size == 68
assert IQ_DATA_BYTES == 1440


class ReplayError(RuntimeError):
    """A user-facing input or replay configuration error."""


@dataclass(frozen=True)
class Capture:
    metadata_path: Path
    bin_path: Path
    capture_id: str
    dataset_name: str
    label_json: str
    label_folder: str
    label: str
    center_frequency_hz: int
    sample_rate_hz: int
    bandwidth_hz: int
    sample_sets: int
    byte_count: int
    sha256: str
    sample_format: str
    component_order: tuple[str, ...]
    duration_ms: int
    manual_gain_db: Optional[float]


@dataclass(frozen=True)
class ReplayPlan:
    description: str
    capture_id: str
    label: str
    center_frequency_hz: int
    source_sample_rate_hz: int
    logical_sample_rate_hz: int
    bandwidth_hz: int
    window_samples: int
    total_samples: int
    tile_stride_samples: int
    config_flags: int
    data_flags: int
    payload_mbps: float
    expected_sha256: Optional[str]
    payload_bytes: int
    payload_factory: Callable[[], Iterable[bytes]]


def _folder_label(folder: Path) -> str:
    lowered = folder.name.lower()
    if "djmini3pro" in lowered or "dji" in lowered:
        return "djmini3pro"
    if "__bg" in lowered or "background" in lowered or lowered.endswith("_bg"):
        return "bg"
    return "unknown"


def _choose_label(json_label: str, folder_label: str, source: str) -> str:
    if source == "json":
        return json_label
    if source == "folder":
        return folder_label
    if folder_label != "unknown" and folder_label != json_label:
        return folder_label
    return json_label


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _metadata_path_from_source(source: str, captures_root: Path) -> Path:
    candidate = Path(source)
    if candidate.exists():
        if candidate.is_dir():
            direct = candidate / "capture_001.json"
            if direct.exists():
                return direct
            matches = sorted(
                p for p in candidate.glob("capture_*.json")
                if ".preview." not in p.name
            )
            if len(matches) == 1:
                return matches[0]
            raise ReplayError(f"Cannot choose one capture metadata file in {candidate}")
        if candidate.name.lower() == "manifest.json":
            try:
                manifest = json.loads(candidate.read_text(encoding="utf-8"))
                captures = manifest.get("captures") or []
                if not captures:
                    raise ReplayError(f"Manifest has no captures: {candidate}")
                return candidate.parent / str(captures[0]["metadata_file"])
            except (OSError, json.JSONDecodeError, KeyError) as exc:
                raise ReplayError(f"Cannot read manifest {candidate}: {exc}") from exc
        if candidate.suffix.lower() == ".bin":
            metadata = candidate.with_suffix(".json")
            if metadata.exists():
                return metadata
            raise ReplayError(f"Missing metadata next to binary file: {metadata}")
        if candidate.suffix.lower() == ".json" and ".preview." not in candidate.name:
            return candidate
        raise ReplayError(f"Unsupported capture source: {candidate}")

    if not captures_root.exists():
        raise ReplayError(f"Capture root does not exist: {captures_root}")
    matches: list[Path] = []
    for manifest_path in captures_root.rglob("manifest.json"):
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if manifest.get("id") == source or manifest_path.parent.name.startswith(source):
            captures = manifest.get("captures") or []
            if captures:
                matches.append(manifest_path.parent / str(captures[0]["metadata_file"]))
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise ReplayError(f"Capture id or path not found below {captures_root}: {source}")
    raise ReplayError(f"Capture source is ambiguous: {source}")


def load_capture(metadata_path: Path, label_source: str = "auto", verify_sha: bool = True) -> Capture:
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReplayError(f"Cannot read metadata {metadata_path}: {exc}") from exc

    bin_path = metadata_path.parent / str(metadata.get("file", ""))
    if not bin_path.exists():
        raise ReplayError(f"Capture binary is missing: {bin_path}")
    component_order = tuple(str(v) for v in metadata.get("component_order", []))
    if component_order != ("rx1_i", "rx1_q"):
        raise ReplayError(
            "This replayer accepts one RX1 stream in rx1_i,rx1_q order; "
            f"got {component_order} in {metadata_path}"
        )
    sample_format = str(metadata.get("sample_format", ""))
    if "little-endian signed 16-bit" not in sample_format.lower():
        raise ReplayError(f"Unsupported sample format in {metadata_path}: {sample_format}")

    expected_bytes = int(metadata.get("sample_sets_written", 0)) * IQ_BYTES_PER_COMPLEX_SAMPLE
    actual_bytes = bin_path.stat().st_size
    if int(metadata.get("bytes", -1)) != actual_bytes or expected_bytes != actual_bytes:
        raise ReplayError(
            f"Size mismatch for {bin_path}: metadata={metadata.get('bytes')}, "
            f"sample-derived={expected_bytes}, actual={actual_bytes}"
        )
    if int(metadata.get("sample_count_requested", -1)) != int(metadata.get("sample_sets_written", -2)):
        raise ReplayError(f"Requested/written sample count mismatch in {metadata_path}")

    manifest_path = metadata_path.parent / "manifest.json"
    if manifest_path.exists():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ReplayError(f"Cannot read manifest {manifest_path}: {exc}") from exc
        if manifest.get("status") not in (None, "completed"):
            raise ReplayError(f"Capture is not completed: {manifest_path}")
        if int(manifest.get("bytes_written", actual_bytes)) != actual_bytes:
            raise ReplayError(f"Manifest byte count mismatch: {manifest_path}")

    recorded_sha = str(metadata.get("sha256", "")).lower()
    if len(recorded_sha) != 64:
        raise ReplayError(f"Missing SHA-256 in {metadata_path}")
    if verify_sha and _sha256_file(bin_path) != recorded_sha:
        raise ReplayError(f"SHA-256 mismatch: {bin_path}")

    label_json = str(metadata.get("label", "unknown"))
    label_folder = _folder_label(metadata_path.parent)
    duration_ms = int(metadata.get("automatic_labels", {}).get("duration_ms", 0))
    manual_gain = metadata.get("manual_gain_db")
    return Capture(
        metadata_path=metadata_path,
        bin_path=bin_path,
        capture_id=metadata_path.parent.name.split("__", 1)[0],
        dataset_name=str(metadata.get("dataset_name", metadata_path.parent.name)),
        label_json=label_json,
        label_folder=label_folder,
        label=_choose_label(label_json, label_folder, label_source),
        center_frequency_hz=int(metadata.get("center_frequency_hz", 0)),
        sample_rate_hz=int(metadata.get("sample_rate_hz", 0)),
        bandwidth_hz=int(metadata.get("rf_bandwidth_hz", 0)),
        sample_sets=int(metadata.get("sample_sets_written", 0)),
        byte_count=actual_bytes,
        sha256=recorded_sha,
        sample_format=sample_format,
        component_order=component_order,
        duration_ms=duration_ms,
        manual_gain_db=float(manual_gain) if manual_gain is not None else None,
    )


def discover_captures(captures_root: Path, label_source: str = "auto") -> list[Capture]:
    captures: list[Capture] = []
    for manifest_path in sorted(captures_root.rglob("manifest.json")):
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            entries = manifest.get("captures") or []
            if not entries:
                continue
            metadata_path = manifest_path.parent / str(entries[0]["metadata_file"])
            captures.append(load_capture(metadata_path, label_source=label_source, verify_sha=False))
        except (OSError, KeyError, json.JSONDecodeError, ReplayError) as exc:
            print(f"warning: skipped {manifest_path}: {exc}", file=sys.stderr)
    return captures


def _iter_file_payloads(path: Path) -> Iterator[bytes]:
    with path.open("rb") as stream:
        while True:
            block = stream.read(IQ_DATA_BYTES)
            if not block:
                return
            if len(block) % IQ_BYTES_PER_COMPLEX_SAMPLE:
                raise ReplayError(f"Non-complex tail in {path}: {len(block)} bytes")
            yield block


def _iter_buffer_payloads(data: bytes) -> Iterator[bytes]:
    for offset in range(0, len(data), IQ_DATA_BYTES):
        block = data[offset : offset + IQ_DATA_BYTES]
        if len(block) % IQ_BYTES_PER_COMPLEX_SAMPLE:
            raise ReplayError(f"Internal non-complex tail: {len(block)} bytes")
        yield block


def _require_numpy(reason: str):
    if np is None:
        raise ReplayError(f"NumPy is required for {reason}; install numpy on the replay host")
    return np


def _factor3_bytes(capture: Capture, taps: int, cutoff_ratio: float) -> bytes:
    numpy = _require_numpy("factor3 replay")
    if taps < 15 or taps % 2 == 0:
        raise ReplayError("--fir-taps must be an odd integer >= 15")
    if not (0.0 < cutoff_ratio < (1.0 / 3.0)):
        raise ReplayError("--cutoff-ratio must be between 0 and 1/3 for factor3")
    raw = numpy.fromfile(capture.bin_path, dtype="<i2")
    if raw.size == 0 or raw.size % 2:
        raise ReplayError(f"Invalid S16 IQ length in {capture.bin_path}")
    pairs = raw.reshape((-1, 2)).astype(numpy.float32)
    n = numpy.arange(taps, dtype=numpy.float32) - ((taps - 1) / 2.0)
    cutoff_cycles_per_sample = cutoff_ratio / 2.0
    h = 2.0 * cutoff_cycles_per_sample * numpy.sinc(2.0 * cutoff_cycles_per_sample * n)
    h *= numpy.hamming(taps).astype(numpy.float32)
    h /= numpy.sum(h)
    filtered_i = numpy.convolve(pairs[:, 0], h, mode="same")[::3]
    filtered_q = numpy.convolve(pairs[:, 1], h, mode="same")[::3]
    # AD936x data uses signed 12-bit values inside an S16 container.
    filtered_i = numpy.clip(numpy.rint(filtered_i), -2048, 2047).astype("<i2")
    filtered_q = numpy.clip(numpy.rint(filtered_q), -2048, 2047).astype("<i2")
    output = numpy.empty((filtered_i.size, 2), dtype="<i2")
    output[:, 0] = filtered_i
    output[:, 1] = filtered_q
    return output.tobytes(order="C")


def _synthetic_payloads(
    sample_rate_hz: int, duration_ms: int, tone_hz: float, amplitude: int
) -> tuple[int, Callable[[], Iterable[bytes]]]:
    if sample_rate_hz <= 0 or duration_ms <= 0:
        raise ReplayError("Synthetic sample rate and duration must be positive")
    if amplitude <= 0 or amplitude > 2047:
        raise ReplayError("Synthetic amplitude must be in 1..2047")
    total_samples = int(round(sample_rate_hz * duration_ms / 1000.0))

    def factory() -> Iterator[bytes]:
        if np is not None:
            index = 0
            while index < total_samples:
                count = min(IQ_DATA_BYTES // IQ_BYTES_PER_COMPLEX_SAMPLE, total_samples - index)
                sample_index = np.arange(index, index + count, dtype=np.float64)
                phase = (2.0 * math.pi * tone_hz * sample_index) / sample_rate_hz
                i_values = np.rint(amplitude * np.cos(phase)).astype("<i2")
                q_values = np.rint(amplitude * np.sin(phase)).astype("<i2")
                output = np.empty((count, 2), dtype="<i2")
                output[:, 0] = i_values
                output[:, 1] = q_values
                yield output.tobytes(order="C")
                index += count
            return

        index = 0
        while index < total_samples:
            count = min(IQ_DATA_BYTES // IQ_BYTES_PER_COMPLEX_SAMPLE, total_samples - index)
            block = bytearray()
            for offset in range(count):
                phase = 2.0 * math.pi * tone_hz * (index + offset) / sample_rate_hz
                block.extend(
                    struct.pack(
                        "<hh",
                        int(round(amplitude * math.cos(phase))),
                        int(round(amplitude * math.sin(phase))),
                    )
                )
            yield bytes(block)
            index += count

    return total_samples, factory


def _window_samples(logical_rate_hz: int, requested: Optional[int]) -> int:
    if requested is not None and requested != MODEL_WINDOW_SAMPLES:
        raise ReplayError(f"--window-samples must equal the formal {MODEL_WINDOW_SAMPLES}")
    if logical_rate_hz <= 0:
        raise ReplayError("logical sample rate must be positive")
    return MODEL_WINDOW_SAMPLES


def _channel_mask(channel: str) -> int:
    if channel == "a":
        return 1
    if channel == "b":
        return 2
    return 3


def _base_flags(channel: str, synthetic: bool) -> int:
    flags = IQ_FLAG_VALID_BITS_12 | (IQ_FLAG_SYNTHETIC if synthetic else 0)
    if channel == "b":
        flags |= IQ_FLAG_FREQUENCY_B
    return flags


def _build_capture_plan(args: argparse.Namespace, capture: Capture) -> ReplayPlan:
    channel = args.channel
    if args.mode == "slow":
        payload_mbps = float(
            args.payload_mbps if args.payload_mbps is not None else DEFAULT_SAFE_PAYLOAD_MBPS
        )
        if payload_mbps < 0.0:
            raise ReplayError("--payload-mbps cannot be negative")
        if args.logical_rate_hz is not None:
            logical_rate = int(args.logical_rate_hz)
        else:
            # Pacing is wall-clock transport policy.  Keep the capture's RF
            # timebase by default so a recorded 10 ms file remains one 10 ms
            # analysis window even when delivery takes much longer.
            logical_rate = capture.sample_rate_hz
        if logical_rate <= 0:
            raise ReplayError("slow mode needs a positive logical sample rate")
        return ReplayPlan(
            description=f"slow raw replay ({capture.sample_sets:,} complex samples)",
            capture_id=capture.capture_id,
            label=capture.label,
            center_frequency_hz=capture.center_frequency_hz,
            source_sample_rate_hz=capture.sample_rate_hz,
            logical_sample_rate_hz=logical_rate,
            bandwidth_hz=capture.bandwidth_hz,
            window_samples=_window_samples(logical_rate, args.window_samples),
            total_samples=capture.sample_sets,
            tile_stride_samples=MODEL_TILE_STRIDE_SAMPLES,
            config_flags=IQ_FLAG_VALID_BITS_12,
            data_flags=_base_flags(channel, False),
            payload_mbps=payload_mbps,
            expected_sha256=capture.sha256,
            payload_bytes=capture.byte_count,
            payload_factory=lambda: _iter_file_payloads(capture.bin_path),
        )

    if args.logical_rate_hz is not None:
        raise ReplayError("factor3 mode fixes logical sample rate to source_rate/3")
    output = _factor3_bytes(capture, args.fir_taps, args.cutoff_ratio)
    if len(output) % IQ_BYTES_PER_COMPLEX_SAMPLE:
        raise ReplayError("factor3 output is not a whole number of complex samples")
    logical_rate = capture.sample_rate_hz // 3
    payload_mbps = float(
        args.payload_mbps if args.payload_mbps is not None else DEFAULT_SAFE_PAYLOAD_MBPS
    )
    if payload_mbps < 0.0:
        raise ReplayError("--payload-mbps cannot be negative")
    effective_bandwidth = min(
        capture.bandwidth_hz,
        int(round(capture.sample_rate_hz * args.cutoff_ratio)),
        logical_rate,
    )
    return ReplayPlan(
        description=f"factor3 filtered replay ({len(output) // 4:,} complex samples)",
        capture_id=capture.capture_id,
        label=capture.label,
        center_frequency_hz=capture.center_frequency_hz,
        source_sample_rate_hz=capture.sample_rate_hz,
        logical_sample_rate_hz=logical_rate,
        bandwidth_hz=effective_bandwidth,
        window_samples=_window_samples(logical_rate, args.window_samples),
        total_samples=len(output) // IQ_BYTES_PER_COMPLEX_SAMPLE,
        tile_stride_samples=MODEL_TILE_STRIDE_SAMPLES,
        config_flags=IQ_FLAG_VALID_BITS_12,
        data_flags=_base_flags(channel, False),
        payload_mbps=payload_mbps,
        expected_sha256=hashlib.sha256(output).hexdigest(),
        payload_bytes=len(output),
        payload_factory=lambda: _iter_buffer_payloads(output),
    )


def _build_synthetic_plan(args: argparse.Namespace) -> ReplayPlan:
    total_samples, factory = _synthetic_payloads(
        args.sample_rate_hz, args.duration_ms, args.tone_hz, args.amplitude
    )
    payload_mbps = float(
        args.payload_mbps
        if args.payload_mbps is not None
        else min(DEFAULT_SAFE_PAYLOAD_MBPS,
                 args.sample_rate_hz * 32.0 / 1_000_000.0)
    )
    if payload_mbps < 0.0:
        raise ReplayError("--payload-mbps cannot be negative")
    return ReplayPlan(
        description=f"synthetic tone ({total_samples:,} complex samples)",
        capture_id="synthetic",
        label="synthetic",
        center_frequency_hz=args.center_hz,
        source_sample_rate_hz=args.sample_rate_hz,
        logical_sample_rate_hz=args.sample_rate_hz,
        bandwidth_hz=FORMAL_BANDWIDTH_HZ,
        window_samples=_window_samples(args.sample_rate_hz, args.window_samples),
        total_samples=total_samples,
        tile_stride_samples=MODEL_TILE_STRIDE_SAMPLES,
        config_flags=IQ_FLAG_SYNTHETIC | IQ_FLAG_VALID_BITS_12,
        data_flags=_base_flags(args.channel, True),
        payload_mbps=payload_mbps,
        expected_sha256=None,
        payload_bytes=total_samples * IQ_BYTES_PER_COMPLEX_SAMPLE,
        payload_factory=factory,
    )


def _require_formal_session(plan: ReplayPlan, args: argparse.Namespace) -> None:
    if args.no_control:
        raise ReplayError("formal sessions require STREAM_START and STREAM_END")
    if plan.source_sample_rate_hz != FORMAL_SAMPLE_RATE_HZ:
        raise ReplayError(f"formal sessions require {FORMAL_SAMPLE_RATE_HZ} source samples/s")
    if plan.logical_sample_rate_hz != FORMAL_SAMPLE_RATE_HZ:
        raise ReplayError(f"formal sessions require {FORMAL_SAMPLE_RATE_HZ} analysis samples/s")
    if plan.bandwidth_hz != FORMAL_BANDWIDTH_HZ:
        raise ReplayError(f"formal sessions require {FORMAL_BANDWIDTH_HZ} Hz bandwidth")
    if plan.total_samples != FORMAL_SESSION_SAMPLES:
        raise ReplayError(f"formal sessions require {FORMAL_SESSION_SAMPLES} complex samples")
    if plan.window_samples != MODEL_WINDOW_SAMPLES:
        raise ReplayError("formal model window does not match the analysis contract")
    if plan.tile_stride_samples != MODEL_TILE_STRIDE_SAMPLES:
        raise ReplayError("formal model stride does not match the analysis contract")
    derived_tiles = 1 + (plan.total_samples - plan.window_samples) // plan.tile_stride_samples
    if derived_tiles != MODEL_TILE_COUNT:
        raise ReplayError(f"formal session derives {derived_tiles} tiles, expected {MODEL_TILE_COUNT}")


def _config_payload(plan: ReplayPlan, session_id: int, channel: str) -> bytes:
    total_low = plan.total_samples & 0xFFFFFFFF
    total_high = (plan.total_samples >> 32) & 0xFFFFFFFF
    try:
        center_index = FORMAL_CENTERS_HZ.index(plan.center_frequency_hz)
    except ValueError as exc:
        raise ReplayError(
            f"formal center frequency is not supported: {plan.center_frequency_hz}"
        ) from exc
    return IQSC.pack(
        IQSC_MAGIC,
        IQSC_VERSION,
        IQSC.size,
        session_id & 0xFFFFFFFF,
        plan.source_sample_rate_hz & 0xFFFFFFFF,
        plan.logical_sample_rate_hz & 0xFFFFFFFF,
        plan.center_frequency_hz & 0xFFFFFFFFFFFFFFFF,
        plan.bandwidth_hz & 0xFFFFFFFF,
        plan.window_samples & 0xFFFFFFFF,
        total_low,
        total_high,
        plan.tile_stride_samples & 0xFFFFFFFF,
        IQ_FORMAT_S16,
        12,
        _channel_mask(channel),
        plan.config_flags & 0xFFFFFFFF,
        center_index,
    )


def _packet(
    sequence: int,
    sample_index: int,
    payload: bytes,
    flags: int,
    session_id: int,
) -> bytes:
    if len(payload) > IQ_DATA_BYTES or len(payload) % IQ_BYTES_PER_COMPLEX_SAMPLE:
        raise ReplayError(f"Invalid packet payload length: {len(payload)}")
    if session_id <= 0 or session_id > 0xFFFFFFFF:
        raise ReplayError("session_id must be a non-zero uint32")
    return IQ_HEADER.pack(
        IQ_MAGIC,
        sequence & 0xFFFFFFFF,
        len(payload),
        flags & 0xFFFFFFFF,
        sample_index & 0xFFFFFFFFFFFFFFFF,
        session_id & 0xFFFFFFFF,
        IQ_FORMAT_S16,
    ) + payload


class _Pacer:
    def __init__(self, payload_mbps: float, enabled: bool) -> None:
        self.enabled = enabled and payload_mbps > 0.0
        self.rate_bps = payload_mbps * 1_000_000.0
        self.started = time.perf_counter()

    def wait_for(self, payload_bytes: int) -> None:
        if not self.enabled:
            return
        target = (payload_bytes * 8.0) / self.rate_bps
        while True:
            remaining = target - (time.perf_counter() - self.started)
            if remaining <= 0.0:
                return
            if remaining > 0.002:
                time.sleep(remaining - 0.001)
            elif remaining > 0.0001:
                time.sleep(0)
            else:
                # A short spin avoids Windows' ~1 ms sleep quantum at high rates.
                pass


def _packet_channel(channel: str, packet_index: int) -> int:
    if channel == "b":
        return 1
    if channel == "alternate":
        return packet_index & 1
    return 0


def _run_session(
    plan: ReplayPlan,
    target: str,
    data_port: int,
    channel: str,
    session_id: int,
    args: argparse.Namespace,
    repeat_index: int,
) -> None:
    if data_port < 1 or data_port > 65535:
        raise ReplayError("--data-port must be between 1 and 65535")
    dry_run = bool(args.dry_run)
    sock: Optional[socket.socket] = None
    destination = (target, data_port)
    if not dry_run:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        except OSError as exc:
            raise ReplayError(f"Cannot create IQ UDP socket: {exc}") from exc

    data_sequence = 0 if args.no_control else 1
    channel_samples = [0, 0]
    data_packets = 0
    udp_packets = 0
    payload_bytes = 0
    digest = hashlib.sha256()
    last_payload_size = 0

    def send(packet: bytes) -> None:
        nonlocal udp_packets
        if sock is not None:
            try:
                sent = sock.sendto(packet, destination)
            except OSError as exc:
                raise ReplayError(f"UDP send to {destination[0]}:{destination[1]} failed: {exc}") from exc
            if sent != len(packet):
                raise ReplayError(f"Short UDP send: {sent}/{len(packet)}")
        udp_packets += 1

    try:
        if not args.no_control:
            begin_flags = plan.config_flags | IQ_FLAG_STREAM_START
            begin = _packet(
                0,
                0,
                _config_payload(plan, session_id, channel),
                begin_flags,
                session_id,
            )
            send(begin)

        pace_enabled = not args.no_pace and (not dry_run or args.pace_dry_run)
        pacer = _Pacer(plan.payload_mbps, pace_enabled)
        started = time.perf_counter()
        for payload in plan.payload_factory():
            packet_channel = _packet_channel(channel, data_packets)
            flags = plan.data_flags
            if packet_channel:
                flags |= IQ_FLAG_FREQUENCY_B
            packet = _packet(
                data_sequence,
                channel_samples[packet_channel],
                payload,
                flags,
                session_id,
            )
            send(packet)
            digest.update(payload)
            payload_bytes += len(payload)
            last_payload_size = len(payload)
            channel_samples[packet_channel] += len(payload) // IQ_BYTES_PER_COMPLEX_SAMPLE
            data_packets += 1
            data_sequence = (data_sequence + 1) & 0xFFFFFFFF
            pacer.wait_for(payload_bytes)

        elapsed = max(time.perf_counter() - started, 1e-9)
        actual_sha = digest.hexdigest()
        if plan.expected_sha256 and actual_sha != plan.expected_sha256:
            raise ReplayError(
                f"Output SHA mismatch for {plan.description}: expected {plan.expected_sha256}, got {actual_sha}"
            )
        if not args.no_control:
            end_channel = _packet_channel(channel, max(data_packets - 1, 0))
            end_flags = plan.config_flags | IQ_FLAG_STREAM_END
            if end_channel:
                end_flags |= IQ_FLAG_FREQUENCY_B
            end = _packet(
                data_sequence,
                channel_samples[end_channel],
                _config_payload(plan, session_id, channel),
                end_flags,
                session_id,
            )
            send(end)

        mode = "dry-run" if dry_run else "sent"
        measured_mbps = payload_bytes * 8.0 / elapsed / 1_000_000.0
        tail_bytes = last_payload_size if payload_bytes and (payload_bytes % IQ_DATA_BYTES) else 0
        print(
            f"{mode} repeat={repeat_index + 1} session=0x{session_id:08x} "
            f"data_packets={data_packets} udp_packets={udp_packets} "
            f"samples={plan.total_samples} bytes={payload_bytes} tail_bytes={tail_bytes} "
            f"elapsed={elapsed:.3f}s payload_mbps={measured_mbps:.3f} sha256={actual_sha}"
        )
    finally:
        if sock is not None:
            sock.close()


def _parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def _add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("target", help="RA8P1 IPv4 address or hostname")
    parser.add_argument("--data-port", type=int, default=5003, help="IQ UDP port (default: 5003)")
    parser.add_argument(
        "--channel",
        choices=("a",),
        default="a",
        help="RF channel (current CPU0 implementation accepts channel A only)",
    )
    parser.add_argument("--repeat", type=int, default=1, help="Number of replay sessions")
    parser.add_argument(
        "--payload-mbps",
        type=float,
        default=None,
        help="IQ payload pacing target in Mbps; replay default is 80, 0 disables pacing",
    )
    parser.add_argument("--window-samples", type=int, default=None)
    parser.add_argument("--session-id", type=_parse_int, default=None, help="Base 32-bit session id")
    parser.add_argument("--no-control", action="store_true", help="Omit inline STREAM_START/STREAM_END packets")
    parser.add_argument("--dry-run", action="store_true", help="Validate and build packets without sending")
    parser.add_argument("--pace-dry-run", action="store_true", help="Keep pacing while using --dry-run")
    parser.add_argument("--no-pace", action="store_true", help="Send as fast as the host allows")


def _cmd_list(args: argparse.Namespace) -> int:
    root = Path(args.captures_root)
    if not root.exists():
        raise ReplayError(f"Capture root does not exist: {root}")
    captures = discover_captures(root, args.label_source)
    print("relative_folder | bin_bytes | samples | center_hz | sample_rate_hz | json_label | folder_label | selected_label")
    for capture in captures:
        relative = capture.metadata_path.parent.relative_to(root)
        mismatch = "*" if capture.label_json != capture.label_folder and capture.label_folder != "unknown" else ""
        print(
            f"{relative} | {capture.byte_count} | {capture.sample_sets} | "
            f"{capture.center_frequency_hz} | {capture.sample_rate_hz} | "
            f"{capture.label_json} | {capture.label_folder}{mismatch} | {capture.label}"
        )
    print(f"captures={len(captures)} root={root}")
    return 0


def _cmd_replay(args: argparse.Namespace) -> int:
    if args.repeat < 1:
        raise ReplayError("--repeat must be positive")
    root = Path(args.captures_root)
    metadata_path = _metadata_path_from_source(args.capture, root)
    capture = load_capture(metadata_path, args.label_source, verify_sha=not args.skip_hash)
    plan = _build_capture_plan(args, capture)
    if not args.dry_run:
        _require_formal_session(plan, args)
    print(
        f"source={capture.metadata_path} label={capture.label} mode={args.mode} "
        f"logical_rate_hz={plan.logical_sample_rate_hz} window_samples={plan.window_samples} "
        f"payload_target_mbps={plan.payload_mbps:.6f} config_bytes={IQSC.size}"
    )
    base_session = args.session_id if args.session_id is not None else secrets.randbits(32)
    base_session &= 0xFFFFFFFF
    if base_session == 0:
        base_session = 1
    for repeat_index in range(args.repeat):
        session = (base_session + repeat_index) & 0xFFFFFFFF or 1
        _run_session(plan, args.target, args.data_port, args.channel, session, args, repeat_index)
    return 0


def _cmd_synthetic(args: argparse.Namespace) -> int:
    if args.repeat < 1:
        raise ReplayError("--repeat must be positive")
    plan = _build_synthetic_plan(args)
    _require_formal_session(plan, args)
    print(
        f"source=synthetic mode=synthetic logical_rate_hz={plan.logical_sample_rate_hz} "
        f"window_samples={plan.window_samples} payload_target_mbps={plan.payload_mbps:.6f} config_bytes={IQSC.size}"
    )
    base_session = args.session_id if args.session_id is not None else secrets.randbits(32)
    base_session &= 0xFFFFFFFF
    if base_session == 0:
        base_session = 1
    for repeat_index in range(args.repeat):
        session = (base_session + repeat_index) & 0xFFFFFFFF or 1
        _run_session(plan, args.target, args.data_port, args.channel, session, args, repeat_index)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    list_parser = subparsers.add_parser("list", help="List capture metadata below a root")
    list_parser.add_argument("--captures-root", default=str(DEFAULT_CAPTURE_ROOT))
    list_parser.add_argument("--label-source", choices=("auto", "json", "folder"), default="auto")
    list_parser.set_defaults(handler=_cmd_list)

    replay_parser = subparsers.add_parser("replay", help="Replay one historical capture")
    _add_common_args(replay_parser)
    replay_parser.add_argument("capture", help="Capture directory, metadata JSON, BIN, id, or folder prefix")
    replay_parser.add_argument("--captures-root", default=str(DEFAULT_CAPTURE_ROOT))
    replay_parser.add_argument("--label-source", choices=("auto", "json", "folder"), default="auto")
    replay_parser.add_argument("--skip-hash", action="store_true", help="Skip input SHA-256 verification")
    replay_parser.add_argument("--mode", choices=("slow", "factor3"), default="slow")
    replay_parser.add_argument(
        "--logical-rate-hz",
        type=int,
        default=None,
        help="Logical RF sample rate in slow mode; defaults to capture metadata",
    )
    replay_parser.add_argument("--fir-taps", type=int, default=63)
    replay_parser.add_argument("--cutoff-ratio", type=float, default=0.30,
                               help="factor3 cutoff normalized to input Nyquist")
    replay_parser.set_defaults(handler=_cmd_replay)

    synthetic_parser = subparsers.add_parser("synthetic", help="Replay a deterministic synthetic S16 tone")
    _add_common_args(synthetic_parser)
    synthetic_parser.add_argument("--sample-rate-hz", type=int, default=FORMAL_SAMPLE_RATE_HZ)
    synthetic_parser.add_argument("--duration-ms", type=int, default=100)
    synthetic_parser.add_argument("--tone-hz", type=float, default=1_000_000.0)
    synthetic_parser.add_argument("--amplitude", type=int, default=1024)
    synthetic_parser.add_argument("--center-hz", type=int, default=2_420_000_000)
    synthetic_parser.set_defaults(handler=_cmd_synthetic)
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.handler(args))
    except ReplayError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

