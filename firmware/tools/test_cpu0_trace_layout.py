import ctypes
import pathlib
import re
import unittest

from project_layout import resolve_cpu0


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = resolve_cpu0(ROOT) / "src" / "framework" / "cpu0_trace.h"


class TraceControl(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("record_bytes", ctypes.c_uint16),
        ("capacity", ctypes.c_uint32),
        ("cpu_cycle_hz", ctypes.c_uint32),
        ("records_started", ctypes.c_uint32),
        ("records_overwritten", ctypes.c_uint32),
        ("latest_sequence", ctypes.c_uint32),
        ("boot_count", ctypes.c_uint32),
    ]


class TraceRecord(ctypes.Structure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("request_id", ctypes.c_uint32),
        ("session_id", ctypes.c_uint32),
        ("center_index", ctypes.c_uint32),
        ("window_index", ctypes.c_uint32),
        ("sample_count", ctypes.c_uint32),
        ("state_flags", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("request_tx_cycles", ctypes.c_uint32),
        ("first_packet_cycles", ctypes.c_uint32),
        ("last_packet_cycles", ctypes.c_uint32),
        ("crc_complete_cycles", ctypes.c_uint32),
        ("ack_tx_cycles", ctypes.c_uint32),
        ("stft_start_cycles", ctypes.c_uint32),
        ("stft_end_cycles", ctypes.c_uint32),
        ("npu_start_cycles", ctypes.c_uint32),
        ("npu_end_cycles", ctypes.c_uint32),
        ("cpu1_visible_cycles", ctypes.c_uint32),
        ("agent_request_rx_us", ctypes.c_uint64),
        ("tune_start_us", ctypes.c_uint64),
        ("tune_complete_us", ctypes.c_uint64),
        ("capture_start_us", ctypes.c_uint64),
        ("capture_complete_us", ctypes.c_uint64),
        ("actual_payload_mbps_x1000", ctypes.c_uint32),
        ("window_crc32c", ctypes.c_uint32),
        ("crc_cycles", ctypes.c_uint32),
        ("sequence_gaps", ctypes.c_uint32),
        ("reordered", ctypes.c_uint32),
        ("invalid_packets", ctypes.c_uint32),
        ("ring_full_drops", ctypes.c_uint32),
        ("ring_oversize_drops", ctypes.c_uint32),
        ("ring_high_watermark", ctypes.c_uint32),
        ("ring_free", ctypes.c_uint32),
        ("cpu0_load_permille", ctypes.c_uint32),
        ("capture_ready_cycles", ctypes.c_uint32),
        ("capture_complete_cycles", ctypes.c_uint32),
        ("credit_accepted_cycles", ctypes.c_uint32),
        ("iqsc_start_cycles", ctypes.c_uint32),
        ("v2_input_copy_cycles", ctypes.c_uint32),
        ("v2_invoke_cycles", ctypes.c_uint32),
        ("v2_output_copy_cycles", ctypes.c_uint32),
        ("v3_input_copy_cycles", ctypes.c_uint32),
        ("v3_invoke_cycles", ctypes.c_uint32),
        ("v3_output_copy_cycles", ctypes.c_uint32),
        ("postprocess_cycles", ctypes.c_uint32),
        ("end_sequence", ctypes.c_uint32),
    ]


def macro(text: str, name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+\((0x[0-9A-Fa-f]+|[0-9]+)(?:U|UL)?\)", text, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing literal macro {name}")
    return int(match.group(1), 0)


class Cpu0TraceLayoutTests(unittest.TestCase):
    def test_abi_and_capacity(self):
        text = HEADER.read_text(encoding="ascii")
        self.assertEqual(32, ctypes.sizeof(TraceControl))
        self.assertEqual(3, macro(text, "CPU0_TRACE_VERSION"))
        self.assertEqual(208, ctypes.sizeof(TraceRecord))
        self.assertEqual(128, macro(text, "CPU0_TRACE_CAPACITY"))
        self.assertEqual(ctypes.sizeof(TraceControl), macro(text, "CPU0_TRACE_CONTROL_BYTES"))
        self.assertEqual(ctypes.sizeof(TraceRecord), macro(text, "CPU0_TRACE_RECORD_BYTES"))
        self.assertEqual(156, TraceRecord.capture_ready_cycles.offset)
        self.assertEqual(160, TraceRecord.capture_complete_cycles.offset)
        self.assertEqual(164, TraceRecord.credit_accepted_cycles.offset)
        self.assertEqual(168, TraceRecord.iqsc_start_cycles.offset)
        self.assertEqual(172, TraceRecord.v2_input_copy_cycles.offset)
        self.assertEqual(176, TraceRecord.v2_invoke_cycles.offset)
        self.assertEqual(180, TraceRecord.v2_output_copy_cycles.offset)
        self.assertEqual(184, TraceRecord.v3_input_copy_cycles.offset)
        self.assertEqual(188, TraceRecord.v3_invoke_cycles.offset)
        self.assertEqual(192, TraceRecord.v3_output_copy_cycles.offset)
        self.assertEqual(196, TraceRecord.postprocess_cycles.offset)
        self.assertEqual(200, TraceRecord.end_sequence.offset)

    def test_noncached_sdram_and_seqlock(self):
        source = HEADER.with_suffix(".c").read_text(encoding="ascii")
        self.assertIn('section(".sdram_noinit_nocache")', source)
        self.assertIn("destination->begin_sequence = sequence | 1U", source)
        self.assertIn("destination->end_sequence = sequence", source)
        self.assertIn("destination->begin_sequence = sequence", source)


if __name__ == "__main__":
    unittest.main()
