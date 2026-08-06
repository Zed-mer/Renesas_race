#!/usr/bin/env python3
"""Loopback proof for the passive SDRC agent; no SDR or RA8 is modified."""

from __future__ import annotations

import argparse
import os
import signal
import socket
import struct
import subprocess
import threading
import time
from dataclasses import dataclass


SDRC_MAGIC = 0x43524453
SDRC_VERSION = 3
SDRC_BYTES = 164
SDRC_CRC_OFFSET = 160
SDRC_CAPTURE_REQ = 1
SDRC_WINDOW_ACK = 2
SDRC_ACCEPTED = 0x8001
SDRC_STARTED = 0x8002
SDRC_COMPLETE = 0x8003
SDRC_CREDIT = 0x8004
SDRC_READY = 0x8005
SDRC_ERROR = 0x80FF
SDRC_STATUS_OK = 0
SDRC_STATUS_RETRY = 13
SDRC_FLAG_RETRANSMIT = 1 << 4
SDRC_FAULT_CRC32C = 1 << 0
SDRC_FAULT_DROP_DATA_PACKET = 1 << 1
SDRC_FAULT_IGNORE_FIRST_REQUEST = 1 << 2
SDRC_FAULT_IGNORE_FIRST_ACK_RESPONSE = 1 << 3
IQ_MAGIC = 0x5149504B
IQ_FLAG_START = 1 << 3
IQ_FLAG_END = 1 << 4
IQ_FLAG_CRC = 1 << 6
QACK_MAGIC = 0x5141434B
QARS_MAGIC = 0x51415253
QACK_FLAGS_COMPLETE_CRC = (1 << 1) | (1 << 2) | (1 << 3)
SAMPLES = 590_336
PAYLOAD_BYTES = SAMPLES * 4
DATA_BYTES = 1440
PACKETS = (PAYLOAD_BYTES + DATA_BYTES - 1) // DATA_BYTES
CENTERS = (2_420_000_000, 2_464_000_000, 5_760_000_000, 5_816_000_000)


def make_crc32c_table() -> tuple[int, ...]:
    values = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        values.append(value)
    return tuple(values)


CRC32C_TABLE = make_crc32c_table()


def crc32c(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    for byte in data:
        crc = (crc >> 8) ^ CRC32C_TABLE[(crc ^ byte) & 0xFF]
    return crc


def put_message(
    *,
    command: int,
    request_id: int,
    session_id: int,
    center_index: int,
    flags: int = 0x0F,
    attempt: int = 0,
    status: int = SDRC_STATUS_OK,
    credit: int = 1,
    ring_free: int = 4096,
    window_crc32c: int = 0,
    boot_epoch: int = 0x1122334455667788,
    sequence_gaps: int = 0,
    reordered: int = 0,
    invalid_packets: int = 0,
    ring_full_drops: int = 0,
    ring_oversize_drops: int = 0,
    crc_errors: int = 0,
    test_fault_flags: int = 0,
    # Keep the Python loopback receiver below its scheduling limit. Production
    # rates are exercised on the RA8P1 hardware, not inferred from this test.
    target_payload_mbps_x1000: int = 20_000,
    send_batch: int = 1,
    retry_limit: int = 2,
    ack_timeout_ms: int = 1000,
    request_timeout_ms: int = 5000,
) -> bytes:
    wire = bytearray(SDRC_BYTES)
    struct.pack_into("<IHHHHIIIQIIIIHHIIIII", wire, 0,
                     SDRC_MAGIC, SDRC_VERSION, SDRC_BYTES, command, flags,
                     request_id, session_id, center_index,
                     CENTERS[center_index], 60_000_000, 56_000_000,
                     SAMPLES, target_payload_mbps_x1000, send_batch,
                     retry_limit, ack_timeout_ms, request_timeout_ms,
                     credit, ring_free, status)
    struct.pack_into("<I", wire, 72, attempt)
    struct.pack_into("<I", wire, 116, window_crc32c)
    struct.pack_into("<QIIIIIII", wire, 124, boot_epoch, sequence_gaps,
                     reordered, invalid_packets, ring_full_drops,
                     ring_oversize_drops, crc_errors, test_fault_flags)
    struct.pack_into("<I", wire, SDRC_CRC_OFFSET,
                     crc32c(wire[:SDRC_CRC_OFFSET]) ^ 0xFFFFFFFF)
    return bytes(wire)


def decode_message(wire: bytes) -> dict[str, int]:
    if len(wire) != SDRC_BYTES:
        raise AssertionError(f"SDRC size {len(wire)}")
    expected = crc32c(wire[:SDRC_CRC_OFFSET]) ^ 0xFFFFFFFF
    actual = struct.unpack_from("<I", wire, SDRC_CRC_OFFSET)[0]
    if expected != actual:
        raise AssertionError("SDRC CRC mismatch")
    return {
        "command": struct.unpack_from("<H", wire, 8)[0],
        "flags": struct.unpack_from("<H", wire, 10)[0],
        "request_id": struct.unpack_from("<I", wire, 12)[0],
        "session_id": struct.unpack_from("<I", wire, 16)[0],
        "status": struct.unpack_from("<I", wire, 68)[0],
        "attempt": struct.unpack_from("<I", wire, 72)[0],
        "tune_start_us": struct.unpack_from("<Q", wire, 84)[0],
        "tune_complete_us": struct.unpack_from("<Q", wire, 92)[0],
        "capture_start_us": struct.unpack_from("<Q", wire, 100)[0],
        "capture_complete_us": struct.unpack_from("<Q", wire, 108)[0],
        "window_crc32c": struct.unpack_from("<I", wire, 116)[0],
        "actual_mbps_x1000": struct.unpack_from("<I", wire, 120)[0],
        "boot_epoch": struct.unpack_from("<Q", wire, 124)[0],
        "sequence_gaps": struct.unpack_from("<I", wire, 132)[0],
        "reordered": struct.unpack_from("<I", wire, 136)[0],
        "invalid_packets": struct.unpack_from("<I", wire, 140)[0],
        "ring_full_drops": struct.unpack_from("<I", wire, 144)[0],
        "ring_oversize_drops": struct.unpack_from("<I", wire, 148)[0],
        "crc_errors": struct.unpack_from("<I", wire, 152)[0],
        "test_fault_flags": struct.unpack_from("<I", wire, 156)[0],
    }


@dataclass(frozen=True)
class Window:
    session_id: int
    packets: int
    payload_bytes: int
    crc32c: int
    advertised_crc32c: int
    sequence_gaps: int


def assert_clean_window(window: Window) -> None:
    if (window.packets != PACKETS or
            window.payload_bytes != PAYLOAD_BYTES or
            window.sequence_gaps != 0 or
            window.advertised_crc32c != window.crc32c):
        raise AssertionError(f"IQSC window is not clean: {window}")


class MockRa8:
    def __init__(self) -> None:
        self.data_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ack_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.data_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                    16 * 1024 * 1024)
        self.data_socket.bind(("127.0.0.1", 5003))
        self.ack_socket.bind(("127.0.0.1", 5002))
        self.data_socket.settimeout(0.2)
        self.ack_socket.settimeout(0.2)
        self.stop = threading.Event()
        self.condition = threading.Condition()
        self.windows: dict[int, list[Window]] = {}
        self.started_sessions: set[int] = set()
        self.errors: list[str] = []
        self.threads = [
            threading.Thread(target=self._receive_data, daemon=True),
            threading.Thread(target=self._serve_ack, daemon=True),
        ]

    def start(self) -> None:
        for thread in self.threads:
            thread.start()

    def close(self) -> None:
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=2)
        self.data_socket.close()
        self.ack_socket.close()
        if self.errors:
            raise AssertionError("; ".join(self.errors))

    def wait_window(self, session_id: int, count: int,
                    timeout: float = 10.0) -> Window:
        deadline = time.monotonic() + timeout
        with self.condition:
            while len(self.windows.get(session_id, [])) < count:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"IQSC session {session_id} timeout")
                self.condition.wait(remaining)
            return self.windows[session_id][count - 1]

    def assert_no_window(self, session_id: int, timeout: float = 0.25) -> None:
        deadline = time.monotonic() + timeout
        with self.condition:
            while not self.windows.get(session_id):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                self.condition.wait(remaining)
            raise AssertionError(
                f"IQSC session {session_id} sent before WINDOW_ACK credit")

    def wait_session_start(self, session_id: int,
                           timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        with self.condition:
            while session_id not in self.started_sessions:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(
                        f"IQSC session {session_id} START timeout")
                self.condition.wait(remaining)

    def assert_window_in_progress(self, session_id: int) -> None:
        with self.condition:
            if session_id not in self.started_sessions:
                raise AssertionError(f"IQSC session {session_id} not started")
            if self.windows.get(session_id):
                raise AssertionError(
                    "capture B did not complete while IQSC A was sending")

    def _receive_data(self) -> None:
        active_session = 0
        sequence = 0
        sample_index = 0
        packets = 0
        payload_bytes = 0
        accumulator = 0xFFFFFFFF
        sequence_gaps = 0
        try:
            while not self.stop.is_set():
                try:
                    datagram, _ = self.data_socket.recvfrom(2048)
                except socket.timeout:
                    continue
                if len(datagram) < 32:
                    raise AssertionError("short IQSC datagram")
                magic, got_sequence, length, flags = struct.unpack_from(
                    "<IIII", datagram, 0)
                got_sample_index = struct.unpack_from("<Q", datagram, 16)[0]
                session_id = struct.unpack_from("<I", datagram, 24)[0]
                payload = datagram[32:]
                if magic != IQ_MAGIC or length != len(payload):
                    raise AssertionError("invalid IQSC header")
                if flags & IQ_FLAG_START:
                    if active_session or got_sequence != 0 or got_sample_index:
                        raise AssertionError("invalid IQSC START")
                    active_session = session_id
                    sequence = 1
                    sample_index = 0
                    packets = 0
                    payload_bytes = 0
                    accumulator = 0xFFFFFFFF
                    sequence_gaps = 0
                    with self.condition:
                        self.started_sessions.add(session_id)
                        self.condition.notify_all()
                    continue
                if session_id != active_session:
                    raise AssertionError(
                        "IQSC session mismatch "
                        f"got={session_id} expected={active_session}")
                if got_sequence > sequence:
                    # Deliberate DATA loss is a valid fault-injection trace.
                    # Keep receiving so the control ACK can request a whole
                    # window retransmission.
                    sequence_gaps += got_sequence - sequence
                    sequence = got_sequence
                elif got_sequence != sequence:
                    raise AssertionError(
                        "IQSC sequence/reorder mismatch "
                        f"got={got_sequence} expected={sequence}")
                if got_sample_index > sample_index:
                    sample_index = got_sample_index
                elif got_sample_index != sample_index:
                    raise AssertionError(
                        "IQSC sample-index/reorder mismatch "
                        f"got={got_sample_index} expected={sample_index}")
                sequence += 1
                if flags & IQ_FLAG_END:
                    expected_crc = struct.unpack_from("<I", payload, 68)[0]
                    actual_crc = accumulator ^ 0xFFFFFFFF
                    if not flags & IQ_FLAG_CRC:
                        raise AssertionError("IQSC END missing CRC flag")
                    if got_sample_index != SAMPLES:
                        raise AssertionError("IQSC END sample count mismatch")
                    window = Window(session_id, packets, payload_bytes,
                                    actual_crc, expected_crc, sequence_gaps)
                    with self.condition:
                        self.windows.setdefault(session_id, []).append(window)
                        self.condition.notify_all()
                    active_session = 0
                    continue
                if not flags & IQ_FLAG_CRC or not payload or len(payload) > DATA_BYTES:
                    raise AssertionError("invalid IQSC DATA")
                accumulator = crc32c(payload, accumulator)
                packets += 1
                payload_bytes += len(payload)
                sample_index += len(payload) // 4
        except Exception as exc:  # pragma: no cover - reported to main thread
            self.errors.append(str(exc))
            self.stop.set()

    def _serve_ack(self) -> None:
        try:
            while not self.stop.is_set():
                try:
                    request, peer = self.ack_socket.recvfrom(64)
                except socket.timeout:
                    continue
                if len(request) != 16 or struct.unpack_from("<I", request)[0] != QACK_MAGIC:
                    raise AssertionError("invalid QACK request")
                request_id, session_id = struct.unpack_from("<II", request, 8)
                window = self.wait_window(session_id, 1)
                response = bytearray(72)
                struct.pack_into("<IHH", response, 0, QARS_MAGIC, 1, 72)
                struct.pack_into("<IIIIIQIIIIIIIII", response, 8,
                                 request_id, session_id, 0,
                                 QACK_FLAGS_COMPLETE_CRC, window.packets,
                                 window.payload_bytes, 0, 0, 0, 0, 0, 4096,
                                 window.crc32c, window.crc32c, 0)
                self.ack_socket.sendto(response, peer)
        except Exception as exc:  # pragma: no cover - reported to main thread
            self.errors.append(str(exc))
            self.stop.set()


def receive_control(sock: socket.socket, expected: int,
                    request_id: int, session_id: int,
                    expect_ok: bool = True,
                    ignore: tuple[tuple[int, int, int], ...] = ()) -> dict[str, int]:
    while True:
        wire, _ = sock.recvfrom(256)
        message = decode_message(wire)
        identity = (message["command"], message["request_id"],
                    message["session_id"])
        if identity in ignore:
            continue
        break
    if (message["command"] != expected or
            message["request_id"] != request_id or
            message["session_id"] != session_id or
            (expect_ok and message["status"] != SDRC_STATUS_OK) or
            (not expect_ok and message["status"] == SDRC_STATUS_OK)):
        raise AssertionError(f"unexpected SDRC response {message}")
    return message


def assert_no_control(sock: socket.socket, timeout: float = 0.25) -> None:
    previous_timeout = sock.gettimeout()
    sock.settimeout(timeout)
    try:
        wire, _ = sock.recvfrom(256)
    except TimeoutError:
        return
    finally:
        sock.settimeout(previous_timeout)
    raise AssertionError(
        f"unexpected SDRC response while fault should be silent: "
        f"{decode_message(wire)}")


def exercise_retryable_data_fault(
    control: socket.socket,
    peer: tuple[str, int],
    mock: MockRa8,
    *,
    request_id: int,
    session_id: int,
    center_index: int,
    fault: int,
) -> None:
    request = put_message(
        command=SDRC_CAPTURE_REQ,
        request_id=request_id,
        session_id=session_id,
        center_index=center_index,
        credit=0,
        test_fault_flags=fault,
    )
    control.sendto(request, peer)
    receive_control(control, SDRC_ACCEPTED, request_id, session_id)
    receive_control(control, SDRC_STARTED, request_id, session_id)
    complete = receive_control(control, SDRC_COMPLETE,
                               request_id, session_id)
    damaged = mock.wait_window(session_id, 1)
    if complete["window_crc32c"] != damaged.advertised_crc32c:
        raise AssertionError("faulted control/data CRC disagreement")
    if fault == SDRC_FAULT_CRC32C:
        if (damaged.sequence_gaps != 0 or damaged.packets != PACKETS or
                damaged.payload_bytes != PAYLOAD_BYTES or
                damaged.advertised_crc32c == damaged.crc32c):
            raise AssertionError(f"CRC fault was not isolated: {damaged}")
        retry_diagnostics = {"crc_errors": 1}
    elif fault == SDRC_FAULT_DROP_DATA_PACKET:
        if (damaged.sequence_gaps != 1 or damaged.packets != PACKETS - 1 or
                damaged.payload_bytes >= PAYLOAD_BYTES or
                damaged.advertised_crc32c == damaged.crc32c):
            raise AssertionError(f"DATA-drop fault was not isolated: {damaged}")
        retry_diagnostics = {"sequence_gaps": 1}
    else:  # pragma: no cover - helper contract
        raise AssertionError(f"unsupported data fault 0x{fault:x}")

    retry_ack = put_message(
        command=SDRC_WINDOW_ACK,
        request_id=request_id,
        session_id=session_id,
        center_index=center_index,
        status=SDRC_STATUS_RETRY,
        credit=0,
        test_fault_flags=fault,
        **retry_diagnostics,
    )
    control.sendto(retry_ack, peer)
    receive_control(control, SDRC_CREDIT, request_id, session_id)

    retry_request = put_message(
        command=SDRC_CAPTURE_REQ,
        request_id=request_id,
        session_id=session_id,
        center_index=center_index,
        flags=0x0F | SDRC_FLAG_RETRANSMIT,
        attempt=1,
        credit=1,
        test_fault_flags=fault,
    )
    control.sendto(retry_request, peer)
    receive_control(control, SDRC_ACCEPTED, request_id, session_id)
    retry_complete = receive_control(control, SDRC_COMPLETE,
                                     request_id, session_id)
    recovered = mock.wait_window(session_id, 2)
    assert_clean_window(recovered)
    expected_recovered_crc = (
        damaged.crc32c if fault == SDRC_FAULT_CRC32C
        else damaged.advertised_crc32c
    )
    if (retry_complete["window_crc32c"] != recovered.crc32c or
            recovered.crc32c != expected_recovered_crc):
        raise AssertionError("cached whole-window retransmit changed data")

    final_ack = put_message(
        command=SDRC_WINDOW_ACK,
        request_id=request_id,
        session_id=session_id,
        center_index=center_index,
        flags=0x0F | SDRC_FLAG_RETRANSMIT,
        attempt=1,
        credit=1,
        window_crc32c=recovered.crc32c,
        test_fault_flags=fault,
    )
    control.sendto(final_ack, peer)
    receive_control(control, SDRC_CREDIT, request_id, session_id)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent", required=True)
    parser.add_argument("--adapter", required=True)
    parser.add_argument("--mock-libiio", required=True)
    parser.add_argument("--trace", action="store_true",
                        help="enable and validate the optional agent trace")
    parser.add_argument("--udp-gso", action="store_true",
                        help="request Linux UDP_SEGMENT and validate its trace")
    args = parser.parse_args()
    control_port = 15004
    request_id = 0x1001
    session_id = 0x2001
    mock = MockRa8()
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(("127.0.0.1", 0))
    control.settimeout(10)
    environment = os.environ.copy()
    environment["RA8P1_LIBIIO_PATH"] = os.path.abspath(args.mock_libiio)
    environment["RA8P1_SDR_UDP_GSO"] = "1" if args.udp_gso else "0"
    command = [args.agent, "127.0.0.1", "--adapter", args.adapter,
               "--control-port", str(control_port)]
    if args.trace:
        command.append("--trace")
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        env=environment,
    )
    mock.start()
    try:
        deadline = time.monotonic() + 10
        ready_line = ""
        while time.monotonic() < deadline:
            ready_line = process.stdout.readline() if process.stdout else ""
            if "SDRC passive agent ready" in ready_line:
                break
            if process.poll() is not None:
                raise AssertionError(process.stderr.read() if process.stderr else "agent exited")
        else:
            raise AssertionError("agent readiness timeout")

        peer = ("127.0.0.1", control_port)
        request = put_message(command=SDRC_CAPTURE_REQ,
                              request_id=request_id, session_id=session_id,
                              center_index=0)
        control.sendto(request, peer)
        receive_control(control, SDRC_ACCEPTED, request_id, session_id)
        receive_control(control, SDRC_STARTED, request_id, session_id)
        mock.wait_session_start(session_id)

        # Request B only after the IQSC START for A.  B must finish capture and
        # publish READY before A's END arrives, proving actual send-A/capture-B
        # overlap.  B still cannot emit IQ until A's valid ACK grants credit.
        second_request_id = request_id + 1
        second_session_id = session_id + 1
        second_request = put_message(
            command=SDRC_CAPTURE_REQ, request_id=second_request_id,
            session_id=second_session_id, center_index=1, credit=0)
        control.sendto(second_request, peer)
        receive_control(control, SDRC_ACCEPTED, second_request_id,
                        second_session_id)
        receive_control(control, SDRC_STARTED, second_request_id,
                        second_session_id)
        receive_control(control, SDRC_READY, second_request_id,
                        second_session_id)
        mock.assert_window_in_progress(session_id)
        mock.assert_no_window(second_session_id)
        # Duplicate prefetch is replay-only: it must not retune or recapture.
        control.sendto(second_request, peer)
        receive_control(control, SDRC_READY, second_request_id,
                        second_session_id)

        first_complete = receive_control(control, SDRC_COMPLETE,
                                         request_id, session_id,
                                         ignore=((SDRC_READY,
                                                  second_request_id,
                                                  second_session_id),))
        first_window = mock.wait_window(session_id, 1)
        assert_clean_window(first_window)
        if first_complete["window_crc32c"] != first_window.crc32c:
            raise AssertionError("control/data CRC disagreement")
        if not (first_complete["tune_start_us"] <=
                first_complete["tune_complete_us"] <=
                first_complete["capture_start_us"] <=
                first_complete["capture_complete_us"]):
            raise AssertionError("invalid tune/capture timeline")

        first_ack = put_message(
            command=SDRC_WINDOW_ACK, request_id=request_id,
            session_id=session_id, center_index=0, credit=1,
            window_crc32c=first_window.crc32c)
        control.sendto(first_ack, peer)
        receive_control(control, SDRC_CREDIT, request_id, session_id)
        second_complete = receive_control(control, SDRC_COMPLETE,
                                          second_request_id,
                                          second_session_id)
        second_window = mock.wait_window(second_session_id, 1)
        assert_clean_window(second_window)
        if second_complete["window_crc32c"] != second_window.crc32c:
            raise AssertionError("prefetched control/data CRC disagreement")

        # A bad clean ACK is rejected, while an explicit retry diagnosis is
        # accepted and causes a whole-window cached retransmission.
        bad_ack = put_message(
            command=SDRC_WINDOW_ACK, request_id=second_request_id,
            session_id=second_session_id, center_index=1,
            window_crc32c=second_window.crc32c, crc_errors=1)
        control.sendto(bad_ack, peer)
        receive_control(control, SDRC_ERROR, second_request_id,
                        second_session_id, expect_ok=False)
        retry_ack = put_message(
            command=SDRC_WINDOW_ACK, request_id=second_request_id,
            session_id=second_session_id, center_index=1,
            status=SDRC_STATUS_RETRY, window_crc32c=0, credit=0,
            crc_errors=1)
        control.sendto(retry_ack, peer)
        receive_control(control, SDRC_CREDIT, second_request_id,
                        second_session_id)
        retry_request = put_message(
            command=SDRC_CAPTURE_REQ, request_id=second_request_id,
            session_id=second_session_id, center_index=1,
            flags=0x0F | SDRC_FLAG_RETRANSMIT, attempt=1, credit=1)
        control.sendto(retry_request, peer)
        receive_control(control, SDRC_ACCEPTED, second_request_id,
                        second_session_id)
        retry_complete = receive_control(control, SDRC_COMPLETE,
                                         second_request_id,
                                         second_session_id)
        retry_window = mock.wait_window(second_session_id, 2)
        assert_clean_window(retry_window)
        if (retry_window.crc32c != second_window.crc32c or
                retry_complete["capture_start_us"] !=
                second_complete["capture_start_us"] or
                retry_complete["capture_complete_us"] !=
                second_complete["capture_complete_us"]):
            raise AssertionError("retransmit did not preserve cached window")

        final_ack = put_message(
            command=SDRC_WINDOW_ACK, request_id=second_request_id,
            session_id=second_session_id, center_index=1, credit=1,
            window_crc32c=retry_window.crc32c)
        control.sendto(final_ack, peer)
        receive_control(control, SDRC_CREDIT, second_request_id,
                        second_session_id)
        # A duplicate ACK is replayed without granting credit twice.
        control.sendto(final_ack, peer)
        receive_control(control, SDRC_CREDIT, second_request_id,
                        second_session_id)

        exercise_retryable_data_fault(
            control, peer, mock,
            request_id=request_id + 10,
            session_id=session_id + 10,
            center_index=2,
            fault=SDRC_FAULT_CRC32C,
        )
        exercise_retryable_data_fault(
            control, peer, mock,
            request_id=request_id + 11,
            session_id=session_id + 11,
            center_index=3,
            fault=SDRC_FAULT_DROP_DATA_PACKET,
        )

        # The first request datagram is deliberately ignored. A retransmit
        # with the same identity and contract must perform the first capture,
        # not be rejected as an unknown retry.
        ignored_request_id = request_id + 12
        ignored_session_id = session_id + 12
        ignored_request = put_message(
            command=SDRC_CAPTURE_REQ,
            request_id=ignored_request_id,
            session_id=ignored_session_id,
            center_index=0,
            credit=0,
            test_fault_flags=SDRC_FAULT_IGNORE_FIRST_REQUEST,
        )
        control.sendto(ignored_request, peer)
        assert_no_control(control)
        ignored_retry = put_message(
            command=SDRC_CAPTURE_REQ,
            request_id=ignored_request_id,
            session_id=ignored_session_id,
            center_index=0,
            flags=0x0F | SDRC_FLAG_RETRANSMIT,
            attempt=1,
            credit=0,
            test_fault_flags=SDRC_FAULT_IGNORE_FIRST_REQUEST,
        )
        control.sendto(ignored_retry, peer)
        receive_control(control, SDRC_ACCEPTED,
                        ignored_request_id, ignored_session_id)
        receive_control(control, SDRC_STARTED,
                        ignored_request_id, ignored_session_id)
        ignored_complete = receive_control(
            control, SDRC_COMPLETE, ignored_request_id, ignored_session_id)
        ignored_window = mock.wait_window(ignored_session_id, 1)
        assert_clean_window(ignored_window)
        if ignored_complete["window_crc32c"] != ignored_window.crc32c:
            raise AssertionError("ignored-request recovery CRC mismatch")
        ignored_ack = put_message(
            command=SDRC_WINDOW_ACK,
            request_id=ignored_request_id,
            session_id=ignored_session_id,
            center_index=0,
            flags=0x0F | SDRC_FLAG_RETRANSMIT,
            attempt=1,
            credit=1,
            window_crc32c=ignored_window.crc32c,
            test_fault_flags=SDRC_FAULT_IGNORE_FIRST_REQUEST,
        )
        control.sendto(ignored_ack, peer)
        receive_control(control, SDRC_CREDIT,
                        ignored_request_id, ignored_session_id)

        # Ownership is applied once even when the first CREDIT_ACCEPTED
        # response is hidden. Repeating the identical ACK must only replay the
        # response and must not grant credit twice.
        ignored_ack_request_id = request_id + 13
        ignored_ack_session_id = session_id + 13
        ack_fault_request = put_message(
            command=SDRC_CAPTURE_REQ,
            request_id=ignored_ack_request_id,
            session_id=ignored_ack_session_id,
            center_index=1,
            credit=0,
            test_fault_flags=SDRC_FAULT_IGNORE_FIRST_ACK_RESPONSE,
        )
        control.sendto(ack_fault_request, peer)
        receive_control(control, SDRC_ACCEPTED,
                        ignored_ack_request_id, ignored_ack_session_id)
        receive_control(control, SDRC_STARTED,
                        ignored_ack_request_id, ignored_ack_session_id)
        ack_fault_complete = receive_control(
            control, SDRC_COMPLETE,
            ignored_ack_request_id, ignored_ack_session_id)
        ack_fault_window = mock.wait_window(ignored_ack_session_id, 1)
        assert_clean_window(ack_fault_window)
        if ack_fault_complete["window_crc32c"] != ack_fault_window.crc32c:
            raise AssertionError("ignored-ACK setup CRC mismatch")
        ack_fault_ack = put_message(
            command=SDRC_WINDOW_ACK,
            request_id=ignored_ack_request_id,
            session_id=ignored_ack_session_id,
            center_index=1,
            credit=1,
            window_crc32c=ack_fault_window.crc32c,
            test_fault_flags=SDRC_FAULT_IGNORE_FIRST_ACK_RESPONSE,
        )
        control.sendto(ack_fault_ack, peer)
        assert_no_control(control)
        control.sendto(ack_fault_ack, peer)
        receive_control(control, SDRC_CREDIT,
                        ignored_ack_request_id, ignored_ack_session_id)

        print("passive SDRC v3 overlap, ACK/credit gate, CRC, cached retry, four fault injections and idempotence tests passed")
        return 0
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        control.close()
        mock.close()
        if args.trace:
            trace_output = process.stderr.read() if process.stderr else ""
            required_trace_fields = (
                "SDRC window_trace",
                "send_batch=",
                "target_mbps=",
                "tune_elapsed_us=",
                "capture_elapsed_us=",
                "send_elapsed_us=",
                "pacing_rebases=",
                "pacing_max_late_us=",
                "transport=",
                "gso_requested=",
                "gso_attempts=",
                "gso_batches=",
                "gso_packets=",
                "gso_fallbacks=",
                "gso_ineligible=",
                "gso_errno=",
                "adapter_flags=",
                "fastlock_profiles=",
                "fastlock_recall_count=",
                "fallback_count=",
                "adapter_block_setup_us=",
                "adapter_dma_wait_us=",
                "adapter_disable_us=",
                "adapter_copy_us=",
                "SDRC control_trace direction=tx event=CAPTURE_ACCEPTED",
                "SDRC control_trace direction=tx event=CAPTURE_STARTED",
                "SDRC control_trace direction=tx event=CAPTURE_READY",
                "SDRC control_trace direction=tx event=CAPTURE_COMPLETE",
                "SDRC control_trace direction=rx event=WINDOW_ACK",
                "SDRC control_trace direction=tx event=CREDIT_ACCEPTED",
                "request=",
                "session=",
                "slot=",
                "status=",
            )
            if any(field not in trace_output
                   for field in required_trace_fields):
                raise AssertionError(
                    "optional SDRC trace is missing required fields: "
                    + trace_output[:2048])
            if args.udp_gso and (
                    "gso_requested=1" not in trace_output or
                    ("transport=udp_gso" not in trace_output and
                     "transport=sendmmsg_gso_fallback" not in trace_output)):
                raise AssertionError(
                    "UDP GSO mode was not attempted or reported: "
                    + trace_output[:2048])


if __name__ == "__main__":
    raise SystemExit(main())
