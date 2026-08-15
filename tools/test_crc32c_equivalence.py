#!/usr/bin/env python3
"""Bit-exact proof for sender and CPU0 reflected Castagnoli CRC32C paths."""

from __future__ import annotations

import random
import re
import unittest
from pathlib import Path

from project_layout import resolve_cpu0


ROOT = Path(__file__).resolve().parents[1]
CPU0_SOURCE = (
    resolve_cpu0(ROOT)
    / "src"
    / "eth_iq_fast.c"
)
RF_PIPELINE_SOURCE = (
    resolve_cpu0(ROOT)
    / "src"
    / "framework"
    / "rf_pipeline.c"
).read_text(encoding="utf-8")
SENDER_SOURCE = ROOT / "tools" / "sdr_iq_udp_stream.c"
CRC32C_POLYNOMIAL_REFLECTED = 0x82F63B78
CRC32C_INIT = 0xFFFFFFFF
CRC32C_XOROUT = 0xFFFFFFFF
LOW_LATENCY_WINDOW_BYTES = 590_336 * 4


def parse_c_table(source: str, name: str) -> tuple[int, ...]:
    match = re.search(
        rf"\b{re.escape(name)}\s*\[[^]]+\][^=]*=\s*\{{(?P<body>.*?)\}};",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"could not find C table {name}")
    return tuple(
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]{8})U", match.group("body"))
    )


def generate_byte_table() -> tuple[int, ...]:
    result = []
    for index in range(256):
        crc = index
        for _ in range(8):
            crc = (crc >> 1) ^ (
                CRC32C_POLYNOMIAL_REFLECTED if crc & 1 else 0
            )
        result.append(crc & 0xFFFFFFFF)
    return tuple(result)


def nibble_update(crc: int, payload: bytes, table: tuple[int, ...]) -> int:
    for octet in payload:
        crc ^= octet
        crc = (crc >> 4) ^ table[crc & 0x0F]
        crc = (crc >> 4) ^ table[crc & 0x0F]
    return crc & 0xFFFFFFFF


def byte_update(crc: int, payload: bytes, table: tuple[int, ...]) -> int:
    for octet in payload:
        crc = (crc >> 8) ^ table[(crc ^ octet) & 0xFF]
    return crc & 0xFFFFFFFF


def generate_slicing_tables(
    byte_table: tuple[int, ...],
) -> tuple[tuple[int, ...], ...]:
    tables = [byte_table]
    for _ in range(1, 8):
        previous = tables[-1]
        tables.append(
            tuple(
                ((value >> 8) ^ byte_table[value & 0xFF]) & 0xFFFFFFFF
                for value in previous
            )
        )
    return tuple(tables)


def slicing_update(
    crc: int,
    payload: bytes,
    tables: tuple[tuple[int, ...], ...],
) -> int:
    offset = 0
    while offset + 8 <= len(payload):
        first = int.from_bytes(payload[offset:offset + 4], "little") ^ crc
        second = int.from_bytes(payload[offset + 4:offset + 8], "little")
        crc = (
            tables[7][first & 0xFF]
            ^ tables[6][(first >> 8) & 0xFF]
            ^ tables[5][(first >> 16) & 0xFF]
            ^ tables[4][first >> 24]
            ^ tables[3][second & 0xFF]
            ^ tables[2][(second >> 8) & 0xFF]
            ^ tables[1][(second >> 16) & 0xFF]
            ^ tables[0][second >> 24]
        )
        offset += 8
    return byte_update(crc, payload[offset:], tables[0])


def aligned_instruction_update(
    crc: int,
    payload: bytes,
    start_alignment: int,
    table: tuple[int, ...],
) -> int:
    """Model the CPU CRC byte-prefix, 32-byte, word and byte-tail split."""
    offset = 0
    while ((start_alignment + offset) & 3) != 0 and offset < len(payload):
        crc = byte_update(crc, payload[offset:offset + 1], table)
        offset += 1
    while offset + 32 <= len(payload):
        for word in range(8):
            start = offset + (word * 4)
            crc = byte_update(crc, payload[start:start + 4], table)
        offset += 32
    while offset + 4 <= len(payload):
        crc = byte_update(crc, payload[offset:offset + 4], table)
        offset += 4
    return byte_update(crc, payload[offset:], table)


def finish(crc: int) -> int:
    return crc ^ CRC32C_XOROUT


def s12_container_to_q15(value: int) -> int:
    signed = value if value < 0x8000 else value - 0x10000
    return max(-32768, min(32767, signed * 16))


def fused_word_update(
    crc: int, payload: bytes, table: tuple[int, ...]
) -> tuple[int, tuple[int, ...]]:
    if len(payload) % 4:
        raise ValueError("fused payload must contain complete IQ words")
    output: list[int] = []
    for offset in range(0, len(payload), 4):
        word = int.from_bytes(payload[offset:offset + 4], "little")
        crc = byte_update(crc, payload[offset:offset + 4], table)
        output.append(s12_container_to_q15(word & 0xFFFF))
        output.append(s12_container_to_q15(word >> 16))
    return crc, tuple(output)


class Crc32cEquivalenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpu0_source = CPU0_SOURCE.read_text(encoding="utf-8")
        cls.sender_source = SENDER_SOURCE.read_text(encoding="utf-8")
        cls.byte_table = parse_c_table(
            cls.cpu0_source, "g_iq_crc32c_byte_table"
        )
        cls.nibble_table = parse_c_table(
            cls.sender_source, "g_crc32c_nibble_table"
        )
        cls.slicing_tables = generate_slicing_tables(cls.byte_table)

    def assert_payload_equivalent(self, payload: bytes) -> None:
        nibble = nibble_update(CRC32C_INIT, payload, self.nibble_table)
        byte = byte_update(CRC32C_INIT, payload, self.byte_table)
        slicing = slicing_update(CRC32C_INIT, payload, self.slicing_tables)
        self.assertEqual(byte, nibble)
        self.assertEqual(slicing, nibble)

    def test_tables_match_reflected_castagnoli_polynomial(self) -> None:
        self.assertEqual(len(self.byte_table), 256)
        self.assertEqual(len(self.nibble_table), 16)
        self.assertEqual(self.byte_table, generate_byte_table())
        self.assertEqual(
            self.nibble_table,
            tuple(self.byte_table[index * 16] for index in range(16)),
        )

    def test_standard_vectors(self) -> None:
        self.assertEqual(
            finish(byte_update(CRC32C_INIT, b"", self.byte_table)), 0x00000000
        )
        self.assertEqual(
            finish(byte_update(CRC32C_INIT, b"123456789", self.byte_table)),
            0xE3069283,
        )
        self.assert_payload_equivalent(bytes(range(256)))
        self.assert_payload_equivalent(b"\x00" * 4096)
        self.assert_payload_equivalent(b"\xFF" * 4096)

    def test_ra8p1_self_test_constant_is_independent_and_correct(self) -> None:
        match = re.search(
            r"#define\s+IQ_CRC32C_SELF_TEST_EXPECTED\s+\(0x([0-9A-Fa-f]{8})U\)",
            self.cpu0_source,
        )
        self.assertIsNotNone(match)
        expected = int(match.group(1), 16)
        self.assertEqual(expected, 0x9F787F65)
        self.assertEqual(
            byte_update(CRC32C_INIT, b"12345678", self.byte_table),
            expected,
        )

    def test_packet_and_unroll_boundaries(self) -> None:
        rng = random.Random(0xC32C_590336)
        for length in (
            0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32,
            63, 64, 255, 256, 1439, 1440, 1441, 1472,
        ):
            with self.subTest(length=length):
                payload = rng.randbytes(length)
                self.assert_payload_equivalent(payload)
                expected = byte_update(CRC32C_INIT, payload, self.byte_table)
                for alignment in range(4):
                    self.assertEqual(
                        aligned_instruction_update(
                            CRC32C_INIT, payload, alignment, self.byte_table
                        ),
                        expected,
                    )

    def test_normal_iq_payload_exercises_two_byte_alignment_prefix(self) -> None:
        ethernet_header = 14
        ipv4_header = 20
        udp_header = 8
        iq_header = 32
        payload_offset = (
            ethernet_header + ipv4_header + udp_header + iq_header
        )
        self.assertEqual(payload_offset, 74)
        self.assertEqual(payload_offset & 3, 2)

    def test_random_payloads_and_incremental_packet_updates(self) -> None:
        rng = random.Random(0x82F63B78)
        for iteration in range(200):
            payload = rng.randbytes(rng.randrange(0, 16_384))
            expected = nibble_update(CRC32C_INIT, payload, self.nibble_table)
            crc = CRC32C_INIT
            offset = 0
            while offset < len(payload):
                chunk = rng.randrange(1, 2049)
                crc = byte_update(crc, payload[offset:offset + chunk], self.byte_table)
                offset += chunk
            self.assertEqual(crc, expected, f"iteration {iteration}")

    def test_fused_crc_and_s12_q15_word_path_is_bit_exact(self) -> None:
        boundaries = (
            -32768, -2049, -2048, -2047, -1, 0, 1,
            2046, 2047, 2048, 32767,
        )
        samples = list(boundaries)
        rng = random.Random(0x512_15)
        samples.extend(rng.randint(-32768, 32767) for _ in range(4096))
        if len(samples) & 1:
            samples.append(0)
        payload = b"".join(
            int(sample & 0xFFFF).to_bytes(2, "little") for sample in samples
        )
        fused_crc, converted = fused_word_update(
            CRC32C_INIT, payload, self.byte_table
        )
        self.assertEqual(
            fused_crc,
            byte_update(CRC32C_INIT, payload, self.byte_table),
        )
        self.assertEqual(
            converted,
            tuple(
                max(-32768, min(32767, sample * 16))
                for sample in samples
            ),
        )

    def test_complete_low_latency_window(self) -> None:
        rng = random.Random(0x590336)
        payload = rng.randbytes(LOW_LATENCY_WINDOW_BYTES)
        nibble = nibble_update(CRC32C_INIT, payload, self.nibble_table)
        crc = CRC32C_INIT
        for offset in range(0, len(payload), 1440):
            crc = byte_update(crc, payload[offset:offset + 1440], self.byte_table)
        self.assertEqual(crc, nibble)
        self.assertEqual(finish(crc), finish(nibble))

    def test_cpu0_source_keeps_hardware_and_table_paths(self) -> None:
        for marker in (
            "IQ_CRC32C_HAS_HW",
            "IQ_CRC32C_HAS_SLICING",
            "R_CRC->CRCDIR = words[0]",
            "R_CRC->CRCDIR = iq_read_le32(&data[28])",
            "IQ_CRC32C_RA8P1_GPS",
            "iq_crc32c_ra8p1_prepare",
            "__crc32cw(crc,",
            "__crc32cb(crc, *data++)",
            "g_iq_crc32c_slicing_table[7][first & 0xFFU]",
            "iq_crc32c_prepare_slicing_table();",
            "iq_crc32c_reference_update(crc, data, length)",
            'section(".dtcm")',
        ):
            self.assertIn(marker, self.cpu0_source)
        self.assertNotIn("g_iq_crc32c_nibble_table", self.cpu0_source)

    def test_rf_worker_uses_the_single_pass_s12_q15_path(self) -> None:
        for marker in (
            "eth_iq_fast_crc_consume_s12_q15",
            "iq_crc32c_convert_word",
            "iq_s12_to_q15",
        ):
            self.assertIn(marker, self.cpu0_source)
        self.assertIn(
            "eth_iq_fast_crc_consume_s12_q15(", RF_PIPELINE_SOURCE
        )
        self.assertIn("analysis_pipeline_ingest_q15(q15_payload", RF_PIPELINE_SOURCE)

    def test_cpu0_exports_end_to_crc_completion_timing(self) -> None:
        for marker in (
            "end_packet_cpu0_cycles",
            "crc_complete_cpu0_cycles",
            "crc_after_end_cycles",
            "ETH_IQ_FAST_CRC_TIMING_END_VALID",
            "ETH_IQ_FAST_CRC_TIMING_COMPLETE_VALID",
        ):
            self.assertIn(marker, self.cpu0_source + CPU0_SOURCE.with_suffix(".h").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
