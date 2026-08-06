#!/usr/bin/env python3
"""Host contract checks for the fixed 512-byte display stream slots."""

from __future__ import annotations

import ctypes
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "shared" / "display_stream.h"
RESOURCE = ROOT / "shared" / "resource_layout.h"
ANALYSIS_PIPELINE = ROOT / "cpu0" / "src" / "framework" / "analysis_pipeline.c"


def literal_macro(path: pathlib.Path, name: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+\(?(0x[0-9A-Fa-f]+|[0-9]+)(?:U|UL|ULL)?\)?\s*(?:/\*.*\*/)?$",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"literal macro {name} not found in {path}")
    return int(match.group(1), 0)


class DetectionBox(ctypes.Structure):
    _fields_ = [
        ("frequency_start_q8", ctypes.c_uint8),
        ("time_start_q8", ctypes.c_uint8),
        ("frequency_span_q8", ctypes.c_uint8),
        ("time_span_q8", ctypes.c_uint8),
        ("class_id", ctypes.c_uint8),
        ("score", ctypes.c_uint8),
        ("metadata", ctypes.c_uint16),
    ]


class AnalysisExtension(ctypes.Structure):
    _fields_ = [
        ("window_sequence", ctypes.c_uint32),
        ("sample_index_low", ctypes.c_uint32),
        ("sample_index_high", ctypes.c_uint32),
        ("window_sample_count", ctypes.c_uint32),
        ("stft_frame_count", ctypes.c_uint32),
        ("stft_cycles", ctypes.c_uint32),
        ("npu_cycles", ctypes.c_uint32),
        ("end_to_end_cycles", ctypes.c_uint32),
        ("npu_inference_count", ctypes.c_uint32),
        ("npu_class", ctypes.c_uint32),
        ("npu_score_q15", ctypes.c_int32),
        ("queue_depth", ctypes.c_uint32),
        ("ingress_drops", ctypes.c_uint32),
        ("npu_ready", ctypes.c_uint32),
        ("mask_width_height", ctypes.c_uint32),
        ("box_count", ctypes.c_uint32),
        ("center_frequency_low", ctypes.c_uint32),
        ("center_frequency_high", ctypes.c_uint32),
        ("source_sample_rate_hz", ctypes.c_uint32),
        ("valid_bits", ctypes.c_uint32),
        ("mask_bits", ctypes.c_uint8 * 64),
        ("boxes", DetectionBox * 4),
        ("timing_flags", ctypes.c_uint32),
        ("presence_q15", ctypes.c_uint16 * 4),
        ("center_index", ctypes.c_uint8),
        ("tile_index", ctypes.c_uint8),
        ("tile_count", ctypes.c_uint8),
        ("reserved8", ctypes.c_uint8),
        ("model_flags", ctypes.c_uint32),
    ]


class DisplayFrame(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("session_id", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("sample_rate_hz", ctypes.c_uint32),
        ("fft_size", ctypes.c_uint32),
        ("channel_mask", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("peak_bin", ctypes.c_uint32 * 2),
        ("peak_power_q16", ctypes.c_uint32 * 2),
        ("spectrum", (ctypes.c_uint8 * 256) * 1),
        ("publish_tick", ctypes.c_uint32),
        ("analysis", AnalysisExtension),
    ]


class DisplayStreamSlot(ctypes.Structure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("payload", DisplayFrame),
        ("end_sequence", ctypes.c_uint32),
    ]


class DisplayStreamLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = HEADER.read_text(encoding="utf-8")

    def test_v4_dimensions_are_explicit_and_not_conflated(self) -> None:
        self.assertEqual(4, literal_macro(HEADER, "RA8P1_DISPLAY_STREAM_VERSION"))
        self.assertEqual(2, literal_macro(HEADER, "RA8P1_DISPLAY_PEAK_CHANNEL_COUNT"))
        self.assertEqual(1, literal_macro(HEADER, "RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT"))
        self.assertEqual(256, literal_macro(HEADER, "RA8P1_DISPLAY_SPECTRUM_BINS"))
        self.assertRegex(
            self.source,
            r"#define\s+RA8P1_DISPLAY_CHANNEL_COUNT\s+\(RA8P1_DISPLAY_PEAK_CHANNEL_COUNT\)",
        )
        self.assertIn("peak_bin[RA8P1_DISPLAY_PEAK_CHANNEL_COUNT]", self.source)
        self.assertIn("peak_power_q16[RA8P1_DISPLAY_PEAK_CHANNEL_COUNT]", self.source)
        self.assertIn(
            "spectrum[RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT][RA8P1_DISPLAY_SPECTRUM_BINS]",
            self.source,
        )
        self.assertNotIn("spectrum[RA8P1_DISPLAY_CHANNEL_COUNT]", self.source)

    def test_v4_boxes_are_physical_rf_geometry_without_growing(self) -> None:
        self.assertEqual(8, ctypes.sizeof(DetectionBox))
        self.assertEqual(256, literal_macro(HEADER, "RA8P1_DISPLAY_RF_COORD_SCALE"))
        for field in (
            "frequency_start_q8",
            "time_start_q8",
            "frequency_span_q8",
            "time_span_q8",
            "metadata",
        ):
            self.assertIn(field, self.source)
        self.assertIn("RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID", self.source)
        self.assertIn("analysis.window_sequence", self.source)

        pipeline = ANALYSIS_PIPELINE.read_text(encoding="utf-8")
        encoder = pipeline.split("static bool analysis_event_to_display_box", 1)[
            1
        ].split("static void analysis_apply_detector_result", 1)[0]
        self.assertIn("RF_V12_RELIABLE_BANDWIDTH_HZ", encoder)
        self.assertIn("event->frequency_low_offset_hz", encoder)
        self.assertIn("event->frequency_high_offset_hz", encoder)
        self.assertIn("event->visible_start_sample", encoder)
        self.assertIn("event->visible_end_sample", encoder)
        self.assertIn("RA8P1_DISPLAY_RF_COORD_SCALE", encoder)
        self.assertIn("box->frequency_start_q8", encoder)
        self.assertIn("box->time_start_q8", encoder)
        self.assertIn("box->frequency_span_q8", encoder)
        self.assertIn("box->time_span_q8", encoder)
        self.assertIn("RA8P1_DISPLAY_BOX_FLAGS_SHIFT", encoder)

    def test_frame_offsets_and_array_shapes_are_exact(self) -> None:
        expected_offsets = {
            "magic": 0,
            "version": 4,
            "size": 6,
            "session_id": 8,
            "sequence": 12,
            "sample_rate_hz": 16,
            "fft_size": 20,
            "channel_mask": 24,
            "flags": 28,
            "peak_bin": 32,
            "peak_power_q16": 40,
            "spectrum": 48,
            "publish_tick": 304,
            "analysis": 308,
        }
        for field, offset in expected_offsets.items():
            with self.subTest(field=field):
                self.assertEqual(offset, getattr(DisplayFrame, field).offset)
                self.assertRegex(
                    self.source,
                    rf"offsetof\(ra8p1_display_frame_t,\s*{field}\)\s*==\s*{offset}U",
                )

        frame = DisplayFrame()
        self.assertEqual(2, len(frame.peak_bin))
        self.assertEqual(2, len(frame.peak_power_q16))
        self.assertEqual(1, len(frame.spectrum))
        self.assertEqual(256, len(frame.spectrum[0]))
        self.assertEqual(196, ctypes.sizeof(AnalysisExtension))
        self.assertEqual(504, ctypes.sizeof(DisplayFrame))

    def test_slot_remains_512_bytes_and_region_capacity_is_unchanged(self) -> None:
        self.assertEqual(4, DisplayStreamSlot.payload.offset)
        self.assertEqual(508, DisplayStreamSlot.end_sequence.offset)
        self.assertEqual(512, ctypes.sizeof(DisplayStreamSlot))
        self.assertEqual(512, literal_macro(HEADER, "RA8P1_DISPLAY_STREAM_SLOT_BYTES"))
        self.assertEqual(4, literal_macro(HEADER, "RA8P1_DISPLAY_STREAM_SLOT_COUNT"))
        self.assertEqual(512, literal_macro(HEADER, "RA8P1_DISPLAY_STREAM_CONTROL_BYTES"))
        self.assertEqual(0x0A00, literal_macro(RESOURCE, "RA8P1_DISPLAY_STREAM_BYTES"))


if __name__ == "__main__":
    unittest.main()
