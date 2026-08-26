#!/usr/bin/env python3
"""Host-side golden checks for the RA8P1 UDP/5003 IQSC v2 wire contract."""

from __future__ import annotations

import importlib.util
import re
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("replay_iq_capture.py")
PROTOCOL_PATH = Path(__file__).parents[1] / "shared" / "iq_protocol.h"
SDR_SENDER_PATH = Path(__file__).with_name("sdr_iq_udp_stream.c")
SPEC = importlib.util.spec_from_file_location("replay_iq_capture", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
replay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = replay
SPEC.loader.exec_module(replay)


class IqProtocolGoldenTest(unittest.TestCase):
    @staticmethod
    def _crc32c(payload: bytes) -> int:
        table = (
            0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1,
            0x417B1DBC, 0x5125DAD3, 0x61C69362, 0x7198540D,
            0x82F63B78, 0x92A8FC17, 0xA24BB5A6, 0xB21572C9,
            0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75,
        )
        crc = 0xFFFFFFFF
        for octet in payload:
            crc ^= octet
            crc = (crc >> 4) ^ table[crc & 0x0F]
            crc = (crc >> 4) ^ table[crc & 0x0F]
        return crc ^ 0xFFFFFFFF

    def test_formal_contract_derives_nineteen_tiles(self) -> None:
        self.assertEqual(replay.FORMAL_SAMPLE_RATE_HZ, 60_000_000)
        self.assertEqual(replay.FORMAL_SESSION_SAMPLES, 6_000_000)
        self.assertEqual(replay.MODEL_WINDOW_SAMPLES, 590_336)
        self.assertEqual(replay.MODEL_TILE_STRIDE_SAMPLES, 295_168)
        self.assertEqual(
            1 + (replay.FORMAL_SESSION_SAMPLES - replay.MODEL_WINDOW_SAMPLES)
            // replay.MODEL_TILE_STRIDE_SAMPLES,
            replay.MODEL_TILE_COUNT,
        )
        self.assertEqual(
            replay.FORMAL_SESSION_SAMPLES
            - (replay.MODEL_WINDOW_SAMPLES
               + (replay.MODEL_TILE_COUNT - 1) * replay.MODEL_TILE_STRIDE_SAMPLES),
            96_640,
        )

    def test_packet_carries_session_at_offset_twenty_four(self) -> None:
        packet = replay._packet(7, 360, b"\x01\x00\x02\x00", 0x20, 0x12345678)
        header = replay.IQ_HEADER.unpack(packet[: replay.IQ_HEADER_BYTES])
        self.assertEqual(header, (replay.IQ_MAGIC, 7, 4, 0x20, 360, 0x12345678, 1))

    def test_iqsc_v2_uses_stride_and_center_index(self) -> None:
        args = SimpleNamespace(
            sample_rate_hz=replay.FORMAL_SAMPLE_RATE_HZ,
            duration_ms=100,
            tone_hz=1_000_000.0,
            amplitude=1024,
            payload_mbps=0.0,
            center_hz=5_816_000_000,
            channel="a",
            window_samples=None,
            no_control=False,
        )
        plan = replay._build_synthetic_plan(args)
        replay._require_formal_session(plan, args)
        fields = replay.IQSC.unpack(replay._config_payload(plan, 0x10203040, "a"))
        self.assertEqual(fields[1], 2)
        self.assertEqual(fields[2], 68)
        self.assertEqual(fields[3], 0x10203040)
        self.assertEqual(fields[7], replay.FORMAL_BANDWIDTH_HZ)
        self.assertEqual(fields[8], replay.MODEL_WINDOW_SAMPLES)
        self.assertEqual(fields[9], replay.FORMAL_SESSION_SAMPLES)
        self.assertEqual(fields[11], replay.MODEL_TILE_STRIDE_SAMPLES)
        self.assertEqual(fields[16], 3)

    def test_formal_plan_packet_count(self) -> None:
        self.assertEqual(
            (replay.FORMAL_SESSION_SAMPLES * replay.IQ_BYTES_PER_COMPLEX_SAMPLE
             + replay.IQ_DATA_BYTES - 1) // replay.IQ_DATA_BYTES,
            16_667,
        )

    def test_optional_window_crc_and_ack_wire_abi(self) -> None:
        protocol = PROTOCOL_PATH.read_text(encoding="utf-8")
        self.assertRegex(
            protocol,
            r"#define\s+RA8P1_IQ_FLAG_WINDOW_CRC\s+\(1UL\s*<<\s*6\)",
        )
        self.assertIn("RA8P1_IQ_CRC32C_TRAILER_BYTES (4U)", protocol)
        self.assertIn("RA8P1_IQ_ACK_REQUEST_MAGIC", protocol)
        self.assertIn("RA8P1_IQ_ACK_RESPONSE_MAGIC", protocol)

        ack_request = struct.Struct("<IHHII")
        ack_response = struct.Struct("<IHHIIIIIQIIIIIIIII")
        self.assertEqual(ack_request.size, 16)
        self.assertEqual(ack_response.size, 72)
        packed = ack_response.pack(
            0x51415253, 1, 72, 7, 11, 0, 0x0E, 1640,
            2_361_344, 0, 0, 0, 0, 0, 4096,
            0xE3069283, 0xE3069283, 0,
        )
        self.assertEqual(struct.unpack_from("<Q", packed, 28)[0], 2_361_344)
        self.assertEqual(struct.unpack_from("<I", packed, 60)[0], 0xE3069283)

    def test_crc32c_castagnoli_known_vector(self) -> None:
        self.assertEqual(self._crc32c(b"123456789"), 0xE3069283)

    def test_end_payload_is_legacy_68_or_crc_72_bytes(self) -> None:
        self.assertEqual(replay.IQSC.size, 68)
        legacy_end = replay.IQSC.pack(*([0] * 17))
        crc_end = legacy_end + struct.pack("<I", self._crc32c(b"iq"))
        self.assertEqual(len(legacy_end), 68)
        self.assertEqual(len(crc_end), 72)
        self.assertEqual(struct.unpack_from("<I", crc_end, 68)[0], self._crc32c(b"iq"))

    def test_sdr_sender_ack_enables_crc_and_retries_same_session(self) -> None:
        sender = SDR_SENDER_PATH.read_text(encoding="utf-8")
        self.assertRegex(
            sender,
            r"if\s*\(options->ack_enabled\s*!=\s*0\)\s*\{\s*"
            r"options->window_crc\s*=\s*1;",
        )
        self.assertIn("IQ_ACK_PORT                   5002U", sender)
        self.assertIn("IQSC_BYTES +", sender)
        self.assertIn(
            "put_u32(packet, IQ_HEADER_BYTES + IQSC_BYTES, transmitted_crc);",
            sender,
        )
        self.assertIn("(final_crc ^ 1U) : final_crc", sender)
        self.assertIn("attempt <= options->ack_retries", sender)
        # send_session_once now accepts an explicit optional precomputed-CRC
        # argument between the immutable retry cache and its result object.
        # Keep asserting that retries reuse the same cached IQ/session path,
        # without pinning this protocol test to the old function arity.
        self.assertRegex(
            sender,
            r"cached_rx1,\s*retry_cache,\s*(?:NULL,\s*)?&session_result",
        )
        self.assertIn(
            "memcpy(&packet[IQ_HEADER_BYTES], retry_cache + payload_bytes, wanted);",
            sender,
        )
        self.assertIn(
            "(options->source_mode == SOURCE_FILE) && (retry_cache == NULL)",
            sender,
        )
        self.assertIn("logical_udp_packets = data_packets + 2U;", sender)
        self.assertIn("result.ring_free != 4096U", sender)

    def test_shared_ack_status_values_are_stable(self) -> None:
        protocol = PROTOCOL_PATH.read_text(encoding="utf-8")
        statuses = dict(
            re.findall(r"RA8P1_IQ_ACK_STATUS_([A-Z_]+)\s*=\s*(\d+)U", protocol)
        )
        self.assertEqual(statuses["OK"], "0")
        self.assertEqual(statuses["ACTIVE"], "1")
        self.assertEqual(statuses["CRC_MISMATCH"], "4")
        self.assertEqual(statuses["RING_DROP"], "6")
        self.assertEqual(statuses["INVALID_REQUEST"], "8")


if __name__ == "__main__":
    unittest.main()
