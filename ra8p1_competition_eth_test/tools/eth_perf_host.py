#!/usr/bin/env python3
"""Host-side controller for the RA8P1 UDP Ethernet performance service."""

import argparse
import socket
import struct
import time


PERF_PORT = 5001
PACKET_MAGIC = 0x46505445


def make_socket(local_ip: str) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    sock.bind((local_ip, 0))
    sock.settimeout(1.0)
    return sock


def request_text(sock: socket.socket, target: tuple[str, int], command: str) -> str:
    for _ in range(4):
        sock.sendto(command.encode("ascii"), target)
        try:
            data, _ = sock.recvfrom(2048)
        except TimeoutError:
            continue
        if data.startswith(b"PERF "):
            return data.decode("ascii", errors="replace")
    raise TimeoutError(f"no response to {command!r}")


def run_info(sock: socket.socket, target: tuple[str, int]) -> None:
    print(request_text(sock, target, "PERF INFO"))


def run_board_tx(sock: socket.socket, target: tuple[str, int], seconds: float, payload: int) -> None:
    duration_ms = max(100, int(seconds * 1000))
    sock.sendto(f"PERF TX {duration_ms} {payload}".encode("ascii"), target)

    packets = 0
    payload_bytes = 0
    first_data = None
    last_data = None
    board_report = None
    deadline = time.perf_counter() + seconds + 8.0
    while time.perf_counter() < deadline:
        try:
            data, _ = sock.recvfrom(65535)
        except TimeoutError:
            continue

        if data.startswith(b"PERF TXDONE"):
            board_report = data.decode("ascii", errors="replace")
            break
        if len(data) >= 8 and struct.unpack_from("<I", data, 0)[0] == PACKET_MAGIC:
            now = time.perf_counter()
            first_data = now if first_data is None else first_data
            last_data = now
            packets += 1
            payload_bytes += len(data)

    elapsed = max(0.000001, (last_data - first_data) if last_data and first_data else seconds)
    mbps = payload_bytes * 8.0 / elapsed / 1_000_000.0
    print(f"HOST RX packets={packets} bytes={payload_bytes} elapsed_s={elapsed:.3f} payload_mbps={mbps:.3f}")
    print(board_report or "PERF TXDONE report not received")


def run_board_rx(sock: socket.socket, target: tuple[str, int], seconds: float, payload: int) -> None:
    print(request_text(sock, target, "PERF RXSTART"))
    packet = bytearray([0x5A]) * payload
    struct.pack_into("<I", packet, 0, PACKET_MAGIC)
    packets = 0
    sent_bytes = 0
    start = time.perf_counter()
    deadline = start + seconds
    while time.perf_counter() < deadline:
        struct.pack_into("<I", packet, 4, packets)
        sent = sock.sendto(packet, target)
        if sent == payload:
            packets += 1
            sent_bytes += sent

    elapsed = time.perf_counter() - start
    for _ in range(6):
        sock.sendto(b"PERF RXSTOP", target)
        time.sleep(0.05)

    report = None
    end_wait = time.perf_counter() + 8.0
    while time.perf_counter() < end_wait:
        try:
            data, _ = sock.recvfrom(2048)
        except TimeoutError:
            continue
        if data.startswith(b"PERF RXDONE"):
            report = data.decode("ascii", errors="replace")
            break

    mbps = sent_bytes * 8.0 / elapsed / 1_000_000.0
    print(f"HOST TX packets={packets} bytes={sent_bytes} elapsed_s={elapsed:.3f} payload_mbps={mbps:.3f}")
    print(report or "PERF RXDONE report not received")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("info", "board-tx", "board-rx"))
    parser.add_argument("--target", default="169.254.139.109")
    parser.add_argument("--local", default="169.254.139.8")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--payload", type=int, default=1472)
    args = parser.parse_args()

    if not 64 <= args.payload <= 1472:
        parser.error("--payload must be between 64 and 1472")

    target = (args.target, PERF_PORT)
    with make_socket(args.local) as sock:
        if args.mode == "info":
            run_info(sock, target)
        elif args.mode == "board-tx":
            run_board_tx(sock, target, args.seconds, args.payload)
        else:
            run_board_rx(sock, target, args.seconds, args.payload)


if __name__ == "__main__":
    main()
