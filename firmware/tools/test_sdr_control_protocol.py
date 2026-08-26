#!/usr/bin/env python3
"""Host-side SDRC v3 byte-order, CRC, and integration contract checks."""

from __future__ import annotations

import struct
import unittest
from pathlib import Path

from project_layout import resolve_cpu0


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "shared" / "sdr_control_protocol.h"
CLIENT = (
    resolve_cpu0(ROOT)
    / "src"
    / "framework"
    / "sdr_control_client.c"
)
CLIENT_HEADER = CLIENT.with_suffix(".h")
PIPELINE = CLIENT.with_name("rf_pipeline.c")


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = ((crc >> 1) ^ (0x82F63B78 & mask)) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


def make_capture_request() -> bytes:
    wire = bytearray(164)
    struct.pack_into(
        "<IHHHHIIIQIIIIHHIIIII",
        wire,
        0,
        0x43524453,
        3,
        164,
        1,
        0x000F,
        0x11223344,
        0x55667788,
        2,
        5_760_000_000,
        60_000_000,
        56_000_000,
        590_336,
        390_000,
        4,
        3,
        1_000,
        3_000,
        1,
        4_096,
        0,
    )
    struct.pack_into(
        "<QIIIIII",
        wire,
        124,
        0x0123456789ABCDEF,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    struct.pack_into("<I", wire, 156, 0x0B)
    struct.pack_into("<I", wire, 160, crc32c(wire[:160]))
    return bytes(wire)


class SdrControlProtocolTests(unittest.TestCase):
    def test_crc32c_standard_vector(self) -> None:
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)

    def test_fixed_little_endian_layout(self) -> None:
        wire = make_capture_request()
        self.assertEqual(len(wire), 164)
        self.assertEqual(wire[:4], b"SDRC")
        self.assertEqual(wire[4:8], b"\x03\x00\xa4\x00")
        self.assertEqual(wire[12:16], b"\x44\x33\x22\x11")
        self.assertEqual(struct.unpack_from("<I", wire, 40)[0], 590_336)
        self.assertEqual(struct.unpack_from("<H", wire, 48)[0], 4)
        self.assertEqual(struct.unpack_from("<I", wire, 64)[0], 4_096)
        self.assertEqual(
            struct.unpack_from("<Q", wire, 124)[0], 0x0123456789ABCDEF
        )
        self.assertEqual(
            struct.unpack_from("<I", wire, 156)[0], 0x0B
        )
        self.assertEqual(
            struct.unpack_from("<I", wire, 160)[0], crc32c(wire[:160])
        )

    def test_crc_detects_corruption(self) -> None:
        wire = bytearray(make_capture_request())
        expected = struct.unpack_from("<I", wire, 160)[0]
        wire[40] ^= 0x01
        self.assertNotEqual(crc32c(wire[:160]), expected)

    def test_protocol_and_gate_contracts_are_present(self) -> None:
        protocol = PROTOCOL.read_text(encoding="ascii")
        client = CLIENT.read_text(encoding="ascii")
        client_header = CLIENT_HEADER.read_text(encoding="ascii")
        pipeline = PIPELINE.read_text(encoding="ascii")
        self.assertIn("RA8P1_SDR_CONTROL_PORT                 (5004U)", protocol)
        self.assertIn("RA8P1_SDR_CONTROL_CAPTURE_READY", protocol)
        self.assertIn("RA8P1_SDR_CONTROL_CREDIT_ACCEPTED", protocol)
        self.assertIn("uint64_t boot_epoch", protocol)
        self.assertIn("uint32_t ring_full_drops", protocol)
        self.assertIn("uint32_t test_fault_flags", protocol)
        self.assertIn("!evidence->payload_complete", client)
        self.assertNotIn("!evidence->cpu1_visible", client)
        self.assertIn("sdr_control_client_start_continuous_scan", client)
        self.assertIn("sdr_control_client_start_continuous_single", client)
        self.assertIn(
            "SDR_CONTROL_DEFAULT_SEND_BATCH         (16U)", client_header
        )
        self.assertIn("RA8P1_SDR_CONTROL_RING_SLOTS", client)
        self.assertIn("MSG_DONTWAIT", pipeline)
        self.assertIn("SDR_CONTROL_PEER_IP", pipeline)
        self.assertIn("RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS", pipeline)
        self.assertIn("sdr_control_client_start_continuous_scan", pipeline)
        self.assertIn("sdr_control_client_start_continuous_single", pipeline)
        self.assertNotIn(
            "((command->flags & RA8P1_COMMAND_FLAG_SCAN_ALL) == 0U))\n"
            "        failure = RA8P1_COMMAND_REASON_INVALID_FORMAT;",
            pipeline,
        )

    def test_expected_tuple_publication_is_irq_atomic(self) -> None:
        pipeline = PIPELINE.read_text(encoding="ascii")
        marker = "static void rf_pipeline_sdr_expected_publish("
        start = pipeline.index(marker)
        end = pipeline.index("\n}", start)
        publish = pipeline[start:end]
        ordered_tokens = (
            "rt_hw_interrupt_disable()",
            "g_sdr_expected_center_index = center_index",
            "g_sdr_expected_sample_count = sample_count",
            "g_sdr_expected_session_id = session_id",
            "rf_pipeline_barrier()",
            "rt_hw_interrupt_enable(irq_level)",
        )
        positions = [publish.index(token) for token in ordered_tokens]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(publish.count("g_sdr_expected_"), 3)
        self.assertIn("#if defined(RT_USING_SMP)", pipeline)
        self.assertIn(
            "RT_LWIP_ETHTHREAD_PRIORITY >= RF_PIPELINE_THREAD_PRIORITY",
            pipeline,
        )


if __name__ == "__main__":
    unittest.main()
