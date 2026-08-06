#!/usr/bin/env python3
"""Host/static checks for progressive real-waterfall tile publication."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from project_layout import resolve_cpu0, resolve_cpu1


ROOT = Path(__file__).resolve().parents[1]
CPU0 = resolve_cpu0(ROOT)
CPU1 = resolve_cpu1(ROOT)
ANALYSIS = CPU0 / "src" / "framework" / "analysis_pipeline.c"
CONTRACT = ROOT / "shared" / "analysis_contract.h"
DISPLAY_TILE = ROOT / "shared" / "display_tile.h"
LAYOUT = ROOT / "shared" / "resource_layout.h"
CPU0_IPC = CPU0 / "src" / "framework" / "ipc_bridge.c"
CPU1_IPC = CPU1 / "src" / "framework" / "ipc_bridge.c"
CPU1_DISPLAY_APP = CPU1 / "src" / "framework" / "display_app.c"
CPU1_LVGL_APP = CPU1 / "src" / "lvgl_app.c"


def literal_macro(path: Path, name: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+\(?(0x[0-9A-Fa-f]+|[0-9]+)(?:U|UL|ULL)?\)?\s*$",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"literal macro {name} not found in {path}")
    return int(match.group(1), 0)


TIME_BINS = literal_macro(CONTRACT, "RA8P1_ANALYSIS_DISPLAY_TIME_BINS")
TIME_POOL = literal_macro(CONTRACT, "RA8P1_ANALYSIS_DISPLAY_TIME_POOL")
FFT_SIZE = literal_macro(CONTRACT, "RA8P1_ANALYSIS_FFT_SIZE")
SAMPLE_RATE_HZ = literal_macro(CONTRACT, "RA8P1_ANALYSIS_SAMPLE_RATE_HZ")
BANDWIDTH_HZ = literal_macro(CONTRACT, "RA8P1_ANALYSIS_BANDWIDTH_HZ")
FREQ_BINS = literal_macro(CONTRACT, "RA8P1_ANALYSIS_DISPLAY_FREQ_BINS")
FREQ_POOL = literal_macro(CONTRACT, "RA8P1_ANALYSIS_DISPLAY_FREQ_POOL")
STFT_FRAMES = literal_macro(CONTRACT, "RA8P1_ANALYSIS_STFT_FRAMES")
TILE_SAMPLES = literal_macro(CONTRACT, "RA8P1_ANALYSIS_TILE_SAMPLES")
TILE_STRIDE_SAMPLES = literal_macro(CONTRACT, "RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES")
TILE_HEIGHT = literal_macro(DISPLAY_TILE, "RA8P1_DISPLAY_TILE_HEIGHT")
ROWS_PER_PUBLISH = literal_macro(ANALYSIS, "ANALYSIS_DISPLAY_ROWS_PER_PUBLISH")
OVERLAP_ROWS = TILE_HEIGHT // 2
BINS_PER_ROW = TIME_BINS // TILE_HEIGHT


def schedule(
    tile_index: int,
    completed_time_bins: int,
    rows_per_publish: int = ROWS_PER_PUBLISH,
    discontinuity: bool = False,
) -> tuple[list[tuple[int, int, int]], list[tuple[int, int]]]:
    """Return periodic (time_bin,start,count) events and final remainder events."""
    published = 0 if tile_index == 0 or discontinuity else OVERLAP_ROWS
    periodic: list[tuple[int, int, int]] = []
    for time_bin in range(1, completed_time_bins + 1):
        ready_rows = min(time_bin // BINS_PER_ROW, TILE_HEIGHT)
        novel_count = ready_rows - published
        if novel_count >= rows_per_publish:
            periodic.append((time_bin, published, rows_per_publish))
            published += rows_per_publish

    ready_rows = min(completed_time_bins // BINS_PER_ROW, TILE_HEIGHT)
    final: list[tuple[int, int]] = []
    if ready_rows > published:
        final.append((published, ready_rows - published))
    return periodic, final


class ProgressiveTileScheduleTests(unittest.TestCase):
    def test_one_display_row_means_eight_pooled_bins(self) -> None:
        self.assertEqual(8, BINS_PER_ROW)
        self.assertEqual(9, TIME_POOL)
        periodic, _ = schedule(0, TIME_BINS)
        self.assertEqual(list(range(8, 129, 8)), [event[0] for event in periodic])
        self.assertEqual(72, periodic[0][0] * TIME_POOL)
        self.assertEqual(STFT_FRAMES, TIME_BINS * TIME_POOL)

    def test_tile_zero_publishes_sixteen_one_row_deltas(self) -> None:
        periodic, final = schedule(0, TIME_BINS)
        self.assertEqual(
            [(start, 1) for start in range(16)],
            [(start, count) for _, start, count in periodic],
        )
        self.assertEqual([], final)

    def test_overlap_tiles_publish_only_rows_eight_through_fifteen(self) -> None:
        periodic, final = schedule(1, TIME_BINS)
        self.assertEqual(
            [(start, 1) for start in range(8, 16)],
            [(start, count) for _, start, count in periodic],
        )
        self.assertEqual([], final)

    def test_first_complete_window_after_gap_publishes_all_sixteen_rows(self) -> None:
        periodic, final = schedule(7, TIME_BINS, discontinuity=True)
        self.assertEqual(
            [(start, 1) for start in range(TILE_HEIGHT)],
            [(start, count) for _, start, count in periodic],
        )
        self.assertEqual([], final)

        source = ANALYSIS.read_text(encoding="utf-8")
        allocator = source.split("static analysis_lane_t *analysis_find_lane", 1)[1].split(
            "static uint8_t analysis_display_tile_count", 1
        )[0]
        self.assertIn(
            "const uint32_t discontinuity = g_analysis.discontinuity_pending",
            allocator,
        )
        self.assertIn(
            "((tile_index == 0U) || (discontinuity != 0U)) ?",
            allocator,
        )
        self.assertIn("0U : ANALYSIS_DISPLAY_OVERLAP_ROWS", allocator)
        self.assertLess(
            allocator.index("g_lanes[i].display_rows_published"),
            allocator.index("g_analysis.discontinuity_pending = 0U"),
        )

    def test_incomplete_display_row_is_not_published_or_duplicated(self) -> None:
        periodic, final = schedule(0, TIME_BINS - 1)
        covered = [
            row
            for _, start, count in periodic
            for row in range(start, start + count)
        ]
        for start, count in final:
            covered.extend(range(start, start + count))
        self.assertEqual(list(range(15)), covered)
        self.assertEqual([], final)
        self.assertEqual(len(covered), len(set(covered)))

    def test_one_row_deltas_reconstruct_the_legacy_two_row_result(self) -> None:
        current, current_final = schedule(0, TIME_BINS)
        legacy, legacy_final = schedule(0, TIME_BINS, rows_per_publish=2)

        def replay(events: list[tuple[int, int, int]], final: list[tuple[int, int]]) -> list[int]:
            reconstructed = [-1] * TILE_HEIGHT
            for _, start, count in events:
                reconstructed[start:start + count] = range(start, start + count)
            for start, count in final:
                reconstructed[start:start + count] = range(start, start + count)
            return reconstructed

        self.assertEqual(list(range(TILE_HEIGHT)), replay(current, current_final))
        self.assertEqual(replay(legacy, legacy_final), replay(current, current_final))

    def test_ring_retains_qualified_panel_period_and_refresh_wait_drains_it(self) -> None:
        slot_count = literal_macro(LAYOUT, "RA8P1_DISPLAY_TILE_SLOT_COUNT")
        slot_bytes = literal_macro(LAYOUT, "RA8P1_DISPLAY_TILE_SLOT_BYTES")
        tile_offset = literal_macro(LAYOUT, "RA8P1_DISPLAY_TILE_OFFSET")
        command_offset = literal_macro(LAYOUT, "RA8P1_IPC_COMMAND_OFFSET")
        iq_bytes_per_complex_sample = 4
        qualified_payload_bits_per_second = 500_000_000
        physical_link_bits_per_second = 1_000_000_000
        full_window_payload_bytes = TILE_SAMPLES * iq_bytes_per_complex_sample
        producer_rows_per_second = (
            qualified_payload_bits_per_second * TILE_HEIGHT /
            (full_window_payload_bytes * 8)
        )
        panel_period_ms = 1000.0 / 46.869
        ring_residence_ms = slot_count * 1000.0 / producer_rows_per_second
        physical_link_ring_residence_ms = (
            slot_count * 1000.0 /
            (physical_link_bits_per_second * TILE_HEIGHT /
             (full_window_payload_bytes * 8))
        )

        # A half-window stride produces eight novel rows, preserving the same
        # bytes-per-row bound as the first 16-row window.
        self.assertEqual(
            full_window_payload_bytes // TILE_HEIGHT,
            TILE_STRIDE_SAMPLES * iq_bytes_per_complex_sample // OVERLAP_ROWS,
        )
        self.assertEqual(slot_count, 16)
        self.assertEqual(slot_bytes, 256)
        self.assertEqual(slot_count & (slot_count - 1), 0)
        self.assertEqual(tile_offset % 32, 0)
        self.assertEqual(slot_bytes % 32, 0)
        self.assertLessEqual(
            tile_offset + slot_count * slot_bytes,
            command_offset,
        )
        self.assertLess(producer_rows_per_second * panel_period_ms / 1000, slot_count)
        self.assertGreater(ring_residence_ms, 37.0)
        self.assertGreater(ring_residence_ms, panel_period_ms)
        self.assertGreater(physical_link_ring_residence_ms, 5.0)

        display_app = CPU1_DISPLAY_APP.read_text(encoding="utf-8")
        drain_loop = re.search(
            r"for \(uint32_t tile_index = 0U;\s*"
            r"tile_index < RA8P1_DISPLAY_TILE_SLOT_COUNT;",
            display_app,
        )
        self.assertIsNotNone(drain_loop)
        self.assertIn("ipc_bridge_cpu1_display_tile_poll", display_app)

        lvgl_app = CPU1_LVGL_APP.read_text(encoding="utf-8")
        flush_wait = lvgl_app.split("static void lvgl_flush_wait_callback", 1)[1].split(
            "static void lvgl_touch_read_callback", 1
        )[0]
        self.assertIn("__WFE();", flush_wait)
        self.assertIn("display_app_drain_tiles();", flush_wait)
        self.assertIn("#define UI_WATERFALL_PRESENT_PERIOD_MS (5U)", lvgl_app)

    def test_transport_payload_carries_one_cache_aligned_row(self) -> None:
        display_tile = DISPLAY_TILE.read_text(encoding="utf-8")
        cpu0_ipc = CPU0_IPC.read_text(encoding="utf-8")
        self.assertEqual(literal_macro(DISPLAY_TILE, "RA8P1_DISPLAY_TILE_VERSION"), 7)
        tile_width = literal_macro(DISPLAY_TILE, "RA8P1_DISPLAY_TILE_WIDTH")
        slot_bytes = literal_macro(LAYOUT, "RA8P1_DISPLAY_TILE_SLOT_BYTES")
        self.assertEqual(tile_width, 192)
        self.assertGreaterEqual(FFT_SIZE, tile_width)
        self.assertLessEqual(36 + tile_width, slot_bytes - 8)
        self.assertEqual((slot_bytes - 8) - 36 - tile_width, 20)
        self.assertIn(
            "#define RA8P1_DISPLAY_TILE_ROW_BYTES   (RA8P1_DISPLAY_TILE_WIDTH)",
            display_tile,
        )
        self.assertIn("uint8_t levels[RA8P1_DISPLAY_TILE_ROW_BYTES]", display_tile)
        self.assertIn("ra8p1_display_tile_row_must_fit_payload", display_tile)
        self.assertIn("ra8p1_display_tile_after_display_stream", display_tile)
        self.assertIn("ra8p1_display_tile_before_commands", display_tile)
        self.assertIn("snapshot.novel_time_count != 1U", CPU1_IPC.read_text(encoding="utf-8"))
        self.assertIn(
            "memcpy(tile_payload.levels, display_row, RA8P1_DISPLAY_TILE_ROW_BYTES)",
            cpu0_ipc,
        )
        self.assertIn("tile_payload.novel_time_count = 1U", cpu0_ipc)
        self.assertIn("row_offset < novel_time_count", cpu0_ipc)

        analysis = ANALYSIS.read_text(encoding="utf-8")
        self.assertIn("analysis_display_frequency_bins_must_cover_tile", analysis)
        self.assertIn(
            "analysis_display_frequency_map_must_reserve_invalid_sentinel",
            analysis,
        )
        self.assertIn("g_display_raw_bin_map[ANALYSIS_FFT_SIZE]", analysis)
        self.assertIn(
            "g_display_power_divisor[RA8P1_DISPLAY_TILE_WIDTH]", analysis
        )
        self.assertIn(
            "display_power_sum[RA8P1_DISPLAY_TILE_WIDTH]", analysis
        )
        self.assertIn("analysis_store_display_row", analysis)
        self.assertEqual(FREQ_BINS, 128)
        self.assertIn("lane->model_input[offset] = q0", analysis)

    def test_configured_bandwidth_uses_every_transported_frequency_bin(self) -> None:
        tile_width = literal_macro(DISPLAY_TILE, "RA8P1_DISPLAY_TILE_WIDTH")

        def fft_bin_valid(shifted_bin: int) -> bool:
            distance = abs(shifted_bin - FFT_SIZE // 2)
            return distance * SAMPLE_RATE_HZ <= BANDWIDTH_HZ * (FFT_SIZE // 2)

        valid_raw_bins = [
            shifted_bin
            for shifted_bin in range(FFT_SIZE)
            if fft_bin_valid(shifted_bin)
        ]
        self.assertEqual((valid_raw_bins[0], valid_raw_bins[-1]), (35, 989))
        self.assertEqual(len(valid_raw_bins), 955)

        mapped = [
            valid_index * tile_width // len(valid_raw_bins)
            for valid_index in range(len(valid_raw_bins))
        ]
        group_sizes = [mapped.count(display_bin) for display_bin in range(tile_width)]
        self.assertEqual(set(mapped), set(range(tile_width)))
        self.assertEqual(set(group_sizes), {4, 5})
        self.assertEqual(group_sizes.count(5), 187)
        self.assertEqual(group_sizes.count(4), 5)
        self.assertTrue(
            all(left <= right for left, right in zip(mapped, mapped[1:]))
        )
        self.assertEqual(
            {size * TIME_POOL * BINS_PER_ROW for size in group_sizes},
            {288, 360},
        )

        analysis = ANALYSIS.read_text(encoding="utf-8")
        self.assertIn("memset(g_display_raw_bin_map, UINT8_MAX", analysis)
        self.assertIn("memset(g_display_power_divisor, 0", analysis)
        self.assertIn("valid_raw_bin_index * RA8P1_DISPLAY_TILE_WIDTH", analysis)
        self.assertIn("g_display_raw_bin_map[shifted_bin]", analysis)
        self.assertIn("g_display_power_divisor[display_bin] +=", analysis)
        self.assertIn("analysis_accumulate_display_power", analysis)

    def test_transport_sequence_still_detects_every_overwritten_delta(self) -> None:
        cpu0_ipc = CPU0_IPC.read_text(encoding="utf-8")
        cpu1_display = CPU1_DISPLAY_APP.read_text(encoding="utf-8")
        cpu1_ipc = CPU1_IPC.read_text(encoding="utf-8")
        self.assertIn("sequence = (g_tile_sequence + 2U) & ~1U", cpu0_ipc)
        self.assertIn(
            "((display_tile.sequence - g_last_tile_sequence) >> 1U) - 1U",
            cpu1_display,
        )
        self.assertIn("g_tile_sequence = oldest_sequence", cpu1_ipc)

        sequences = [2, 4, 8, 10]
        missed = sum(((current - previous) >> 1) - 1
                     for previous, current in zip(sequences, sequences[1:]))
        self.assertEqual(1, missed)

    def test_consumer_reads_the_expected_ring_slot_before_gap_scan(self) -> None:
        cpu1_ipc = CPU1_IPC.read_text(encoding="utf-8")
        poll = cpu1_ipc.split(
            "bool ipc_bridge_cpu1_display_tile_poll", 1
        )[1].split("bool ipc_bridge_cpu1_command_send", 1)[0]

        self.assertIn("(g_tile_sequence + 2U) & ~1U", poll)
        self.assertIn("((expected_sequence >> 1U) - 1U)", poll)
        self.assertIn("ipc_bridge_cpu1_display_tile_copy", poll)
        self.assertLess(
            poll.index("const uint32_t expected_sequence"),
            poll.index("for (index = 0U; index < RA8P1_DISPLAY_TILE_SLOT_COUNT"),
        )

        slot_count = literal_macro(LAYOUT, "RA8P1_DISPLAY_TILE_SLOT_COUNT")
        mapped = [(((sequence >> 1) - 1) & (slot_count - 1))
                  for sequence in range(2, 2 * slot_count + 1, 2)]
        self.assertEqual(list(range(slot_count)), mapped)
        self.assertEqual(0, (((2 * slot_count + 2) >> 1) - 1) &
                         (slot_count - 1))

    def test_firmware_guards_flags_proof_and_final_path(self) -> None:
        source = ANALYSIS.read_text(encoding="utf-8")
        helper_start = source.index("static void analysis_publish_ready_display_rows")
        helper_end = source.index("static ANALYSIS_HOT_CODE void analysis_store_pool_row", helper_start)
        helper = source[helper_start:helper_end]
        self.assertIn("g_stft_probe.active != 0U", helper)
        self.assertIn("g_analysis.synthetic != 0U", helper)
        self.assertNotIn("RA8P1_DISPLAY_FLAG_WINDOW_COMPLETE", helper)

        store_start = source.index("static void analysis_finish_processed_frame")
        store_end = source.index(
            "static ANALYSIS_HOT_CODE void analysis_process_frame", store_start
        )
        store = source[store_start:store_end]
        self.assertLess(store.index("lane->display_row_count++"), store.index(
            "analysis_publish_ready_display_rows(lane, false)"
        ))
        self.assertIn(
            "lane->display_frame_count == ANALYSIS_DISPLAY_FRAMES_PER_ROW", store
        )
        self.assertIn("analysis_iq_slot_cannot_cross_two_display_row_boundaries", source)
        self.assertIn("analysis_display_slots_must_retain_two_lane_publish_burst", source)

        publish_start = source.rindex("static void analysis_publish_lane")
        publish_end = source.index("static void analysis_complete_lanes", publish_start)
        publish = source[publish_start:publish_end]
        self.assertEqual(1, publish.count("analysis_publish_ready_display_rows(lane, true)"))


if __name__ == "__main__":
    unittest.main()
