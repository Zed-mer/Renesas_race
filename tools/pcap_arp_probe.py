#!/usr/bin/env python3
"""Send link-local ARP probes through Npcap without changing host IP state."""

from __future__ import annotations

import argparse
import ctypes
import ipaddress
import json
import struct
import sys
import time
from pathlib import Path


PCAP_ERRBUF_SIZE = 256
DLT_EN10MB = 1


class Timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]


class PcapPacketHeader(ctypes.Structure):
    _fields_ = [
        ("ts", Timeval),
        ("caplen", ctypes.c_uint32),
        ("length", ctypes.c_uint32),
    ]


class BpfInsn(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_ushort),
        ("jt", ctypes.c_ubyte),
        ("jf", ctypes.c_ubyte),
        ("k", ctypes.c_uint32),
    ]


class BpfProgram(ctypes.Structure):
    _fields_ = [
        ("bf_len", ctypes.c_uint),
        ("bf_insns", ctypes.POINTER(BpfInsn)),
    ]


def parse_mac(value: str) -> bytes:
    compact = value.replace("-", "").replace(":", "")
    if len(compact) != 12:
        raise argparse.ArgumentTypeError("MAC address must contain 6 octets")
    try:
        result = bytes.fromhex(compact)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid MAC address") from exc
    if result == b"\x00" * 6 or result[0] & 1:
        raise argparse.ArgumentTypeError("source MAC must be nonzero unicast")
    return result


def format_mac(value: bytes) -> str:
    return "-".join(f"{octet:02X}" for octet in value)


def build_targets(explicit: list[str], subnets: list[str]) -> list[ipaddress.IPv4Address]:
    targets: set[ipaddress.IPv4Address] = set()
    for value in explicit:
        targets.add(ipaddress.IPv4Address(value))
    for value in subnets:
        network = ipaddress.IPv4Network(value, strict=True)
        if network.num_addresses > 1024:
            raise ValueError(f"subnet too large for an ARP probe: {network}")
        targets.update(network.hosts())
    return sorted(targets, key=int)


def load_wpcap(path: Path) -> ctypes.CDLL:
    if not path.is_file():
        raise FileNotFoundError(f"Npcap wpcap.dll not found: {path}")
    library = ctypes.CDLL(str(path))
    library.pcap_open_live.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
    ]
    library.pcap_open_live.restype = ctypes.c_void_p
    library.pcap_close.argtypes = [ctypes.c_void_p]
    library.pcap_datalink.argtypes = [ctypes.c_void_p]
    library.pcap_datalink.restype = ctypes.c_int
    library.pcap_sendpacket.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ubyte),
        ctypes.c_int,
    ]
    library.pcap_sendpacket.restype = ctypes.c_int
    library.pcap_next_ex.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(PcapPacketHeader)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
    ]
    library.pcap_next_ex.restype = ctypes.c_int
    library.pcap_geterr.argtypes = [ctypes.c_void_p]
    library.pcap_geterr.restype = ctypes.c_char_p
    library.pcap_compile.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(BpfProgram),
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_uint32,
    ]
    library.pcap_compile.restype = ctypes.c_int
    library.pcap_setfilter.argtypes = [ctypes.c_void_p, ctypes.POINTER(BpfProgram)]
    library.pcap_setfilter.restype = ctypes.c_int
    library.pcap_freecode.argtypes = [ctypes.POINTER(BpfProgram)]
    return library


def pcap_error(library: ctypes.CDLL, handle: int) -> str:
    message = library.pcap_geterr(handle)
    return message.decode("utf-8", errors="replace") if message else "unknown Npcap error"


def build_arp_request(source_mac: bytes,
                      source_ip: ipaddress.IPv4Address,
                      target_ip: ipaddress.IPv4Address) -> bytes:
    ethernet = b"\xff" * 6 + source_mac + struct.pack("!H", 0x0806)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1,
        0x0800,
        6,
        4,
        1,
        source_mac,
        source_ip.packed,
        b"\x00" * 6,
        target_ip.packed,
    )
    return ethernet + arp


def parse_arp_reply(packet: bytes) -> tuple[ipaddress.IPv4Address, bytes] | None:
    if len(packet) < 42:
        return None
    ethertype = struct.unpack_from("!H", packet, 12)[0]
    arp_offset = 14
    if ethertype in (0x8100, 0x88A8):
        if len(packet) < 46:
            return None
        ethertype = struct.unpack_from("!H", packet, 16)[0]
        arp_offset = 18
    if ethertype != 0x0806 or len(packet) < arp_offset + 28:
        return None
    htype, protocol, hlen, plen, operation = struct.unpack_from(
        "!HHBBH", packet, arp_offset
    )
    if (htype, protocol, hlen, plen, operation) != (1, 0x0800, 6, 4, 2):
        return None
    source_mac = packet[arp_offset + 8:arp_offset + 14]
    source_ip = ipaddress.IPv4Address(packet[arp_offset + 14:arp_offset + 18])
    return source_ip, source_mac


def probe(library: ctypes.CDLL,
          device: str,
          source_mac: bytes,
          source_ip: ipaddress.IPv4Address,
          targets: list[ipaddress.IPv4Address],
          retries: int,
          timeout_ms: int) -> dict[str, str]:
    error_buffer = ctypes.create_string_buffer(PCAP_ERRBUF_SIZE)
    handle = library.pcap_open_live(
        device.encode("ascii"), 65536, 1, 50, error_buffer
    )
    if not handle:
        detail = error_buffer.value.decode("utf-8", errors="replace")
        raise RuntimeError(f"pcap_open_live failed for {device}: {detail}")
    try:
        if library.pcap_datalink(handle) != DLT_EN10MB:
            raise RuntimeError("selected Npcap device is not an Ethernet interface")
        program = BpfProgram()
        if library.pcap_compile(handle, ctypes.byref(program), b"arp", 1, 0xFFFFFFFF) != 0:
            raise RuntimeError(f"pcap_compile failed: {pcap_error(library, handle)}")
        try:
            if library.pcap_setfilter(handle, ctypes.byref(program)) != 0:
                raise RuntimeError(f"pcap_setfilter failed: {pcap_error(library, handle)}")
        finally:
            library.pcap_freecode(ctypes.byref(program))

        for _ in range(retries):
            for target in targets:
                frame = build_arp_request(source_mac, source_ip, target)
                buffer = (ctypes.c_ubyte * len(frame)).from_buffer_copy(frame)
                if library.pcap_sendpacket(handle, buffer, len(frame)) != 0:
                    raise RuntimeError(f"pcap_sendpacket failed: {pcap_error(library, handle)}")
            time.sleep(0.05)

        responses: dict[str, str] = {}
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            header = ctypes.POINTER(PcapPacketHeader)()
            data = ctypes.POINTER(ctypes.c_ubyte)()
            status = library.pcap_next_ex(handle, ctypes.byref(header), ctypes.byref(data))
            if status == 0:
                continue
            if status == -2:
                break
            if status < 0:
                raise RuntimeError(f"pcap_next_ex failed: {pcap_error(library, handle)}")
            packet = ctypes.string_at(data, header.contents.caplen)
            parsed = parse_arp_reply(packet)
            if parsed is None:
                continue
            reply_ip, reply_mac = parsed
            if reply_ip in targets:
                responses[str(reply_ip)] = format_mac(reply_mac)
        return responses
    finally:
        library.pcap_close(handle)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help=r"Npcap device, for example \\Device\\NPF_{GUID}")
    parser.add_argument("--source-mac", required=True, type=parse_mac)
    parser.add_argument("--source-ip", required=True, type=ipaddress.IPv4Address)
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--subnet", action="append", default=[])
    parser.add_argument("--retries", type=int, default=2, choices=range(1, 6))
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument(
        "--wpcap",
        type=Path,
        default=Path(r"C:\Windows\System32\Npcap\wpcap.dll"),
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.timeout_ms < 100 or args.timeout_ms > 30000:
        parser.error("--timeout-ms must be between 100 and 30000")
    if not args.target and not args.subnet:
        parser.error("at least one --target or --subnet is required")
    return args


def main() -> int:
    args = parse_args()
    try:
        targets = build_targets(args.target, args.subnet)
        library = load_wpcap(args.wpcap)
        responses = probe(
            library,
            args.device,
            args.source_mac,
            args.source_ip,
            targets,
            args.retries,
            args.timeout_ms,
        )
    except (FileNotFoundError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    result = {
        "device": args.device,
        "source_mac": format_mac(args.source_mac),
        "source_ip": str(args.source_ip),
        "targets": len(targets),
        "responses": responses,
        "host_ip_changed": False,
    }
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"Probed {len(targets)} targets; {len(responses)} replied")
        for address, mac in responses.items():
            print(f"{address} {mac}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
