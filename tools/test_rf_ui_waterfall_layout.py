#!/usr/bin/env python3
import hashlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RF_UI_C = (ROOT / "cpu1/src/ui/rf_ui.c").read_text(encoding="utf-8")
RF_UI_H = (ROOT / "cpu1/src/ui/rf_ui.h").read_text(encoding="utf-8")
RF_DEMO_H = (ROOT / "cpu1/src/ui/rf_demo_data.h").read_text(encoding="utf-8")
RF_DEMO_C = (ROOT / "cpu1/src/ui/rf_demo_data.c").read_text(encoding="utf-8")
RF_DEVICE_C = (ROOT / "cpu1/src/ui/rf_device_thumbnails.c").read_text(
    encoding="utf-8"
)
RF_DEVICE_H = (ROOT / "cpu1/src/ui/rf_device_thumbnails.h").read_text(
    encoding="utf-8"
)
RF_GLYPHS = (ROOT / "cpu1/src/ui/rf_ui_glyphs.txt").read_text(
    encoding="utf-8"
)
RF_FONT_C = (ROOT / "cpu1/src/ui/rf_ui_font_zh_14.c").read_text(
    encoding="utf-8"
)
LVGL_APP_C = (ROOT / "cpu1/src/lvgl_app.c").read_text(encoding="utf-8")
LV_REFR_C = (ROOT / "cpu1/ra/lvgl/lvgl/src/core/lv_refr.c").read_text(
    encoding="utf-8"
)
LV_DISPLAY_C = (ROOT / "cpu1/ra/lvgl/lvgl/src/display/lv_display.c").read_text(
    encoding="utf-8"
)
LV_IMAGE_C = (ROOT / "cpu1/ra/lvgl/lvgl/src/widgets/image/lv_image.c").read_text(
    encoding="utf-8"
)
LV_IMAGE_H = (ROOT / "cpu1/ra/lvgl/lvgl/src/widgets/image/lv_image.h").read_text(
    encoding="utf-8"
)
DISPLAY_APP_C = (ROOT / "cpu1/src/framework/display_app.c").read_text(
    encoding="utf-8"
)
DISPLAY_BRINGUP_C = (ROOT / "cpu1/src/display_bringup.c").read_text(
    encoding="utf-8"
)
DISPLAY_BRINGUP_H = (ROOT / "cpu1/src/display_bringup.h").read_text(
    encoding="utf-8"
)
LIVE_MONITOR_PS1 = (ROOT / "tools/run_rf_ui_live_monitor.ps1").read_text(
    encoding="utf-8"
)


def integer_define(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{name}\s+\(?([0-9]+)[uU]?\)?", source,
                      re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing integer define {name}")
    return int(match.group(1))


WATERFALL_HISTORY_COLS = integer_define(
    RF_UI_H, "RF_UI_WATERFALL_HISTORY_COLS"
)


def model_guard_mutation_ranges(
    old_block: int, old_count: int, new_head: int, alignment: int = 128
) -> list[tuple[int, int]]:
    """Return half-open physical ranges changed by one guard update."""
    new_block = new_head & ~(alignment - 1)
    new_count = new_head - new_block
    if old_block == new_block:
        if new_count > old_count:
            return [(old_block + old_count, new_count - old_count)]
        if new_count < old_count:
            return [(old_block + new_count, old_count - new_count)]
        return []
    ranges = []
    if old_count:
        ranges.append((old_block, old_count))
    if new_count:
        ranges.append((new_block, new_count))
    return ranges


def model_range_hits_view(start: int, count: int, view_start: int,
                          view_width: int = 800) -> bool:
    return start < view_start + view_width and start + count > view_start


def model_sequence_events(tiles: list[tuple[int, int]]) -> list[str]:
    """Model the visible ordering for (session_id, even tile sequence)."""
    events: list[str] = []
    last_session: int | None = None
    last_sequence = 0
    for session_id, sequence in tiles:
        gap_count = 0
        if session_id != last_session:
            if 2 <= sequence < 0x80000000 and sequence % 2 == 0:
                gap_count = (sequence >> 1) - 1
            last_session = session_id
            last_sequence = sequence
        else:
            sequence_delta = (sequence - last_sequence) & 0xFFFFFFFF
            if 0 < sequence_delta < 0x80000000 and sequence_delta % 2 == 0:
                gap_count = max(0, (sequence_delta >> 1) - 1)
                last_sequence = sequence
        events.extend(["gap"] * min(gap_count, WATERFALL_HISTORY_COLS))
        events.append("data")
    return events


def model_center_guard_events(
    tiles: list[tuple[int, int, int]],
) -> list[str]:
    """Model gap ownership for (session, even sequence, center)."""
    events: list[str] = []
    last_session: int | None = None
    last_sequence = 0
    session_centers: set[int] = set()
    for session_id, sequence, center in tiles:
        gap_count = 0
        if session_id != last_session:
            session_centers = {center}
            if 2 <= sequence < 0x80000000 and sequence % 2 == 0:
                gap_count = (sequence >> 1) - 1
            last_session = session_id
            last_sequence = sequence
        else:
            session_centers.add(center)
            sequence_delta = (sequence - last_sequence) & 0xFFFFFFFF
            if 0 < sequence_delta < 0x80000000 and sequence_delta % 2 == 0:
                gap_count = max(0, (sequence_delta >> 1) - 1)
                last_sequence = sequence

        if gap_count and len(session_centers) > 1:
            centers = ",".join(str(index) for index in sorted(session_centers))
            events.append(f"unknown:{centers}")
        else:
            events.extend(
                [f"gap:{center}"] * min(gap_count, WATERFALL_HISTORY_COLS)
            )
        events.append(f"data:{center}")
    return events


def model_logical_history_resets(
    tiles: list[tuple[int, int, int, int, int, bool]],
) -> list[tuple[int, int, int]]:
    """Model resets for (center, session, window, row, sequence, flag)."""
    state: dict[int, dict[str, object]] = {}
    resets: list[tuple[int, int, int]] = []
    last_transport_session: int | None = None
    last_transport_sequence = 0

    for center, session_id, window_sequence, time_start, sequence, flag in tiles:
        gap_columns = 0
        if session_id != last_transport_session:
            if 2 <= sequence < 0x80000000 and sequence % 2 == 0:
                gap_columns = (sequence >> 1) - 1
            last_transport_session = session_id
            last_transport_sequence = sequence
        else:
            sequence_delta = (sequence - last_transport_sequence) & 0xFFFFFFFF
            if 0 < sequence_delta < 0x80000000 and sequence_delta % 2 == 0:
                gap_columns = max(0, (sequence_delta >> 1) - 1)
                last_transport_sequence = sequence

        center_state = state.get(center)
        if center_state is None or center_state["session"] != session_id:
            center_state = {
                "session": session_id,
                "runs": {},
                "max_window": None,
                "discontinuity": False,
            }
            state[center] = center_state

        runs = center_state["runs"]
        assert isinstance(runs, dict)
        reset = False
        previous_row = runs.get(window_sequence)
        max_window = center_state["max_window"]
        if previous_row is not None:
            if time_start <= previous_row or gap_columns:
                reset = True
        elif max_window is not None:
            delta = (window_sequence - int(max_window)) & 0xFFFFFFFF
            signed_delta = delta if delta < 0x80000000 else delta - 0x100000000
            if signed_delta < 0:
                reset = True

        if reset:
            runs.clear()
            center_state["max_window"] = None
            center_state["discontinuity"] = False

        if window_sequence not in runs and len(runs) >= 2:
            completed = next(
                (key for key, row in runs.items() if row == 15),
                next(iter(runs)),
            )
            del runs[completed]
        runs[window_sequence] = time_start

        max_window = center_state["max_window"]
        if max_window is None:
            center_state["max_window"] = window_sequence
        else:
            delta = (window_sequence - int(max_window)) & 0xFFFFFFFF
            if 0 < delta < 0x80000000:
                center_state["max_window"] = window_sequence

        if flag:
            if not center_state["discontinuity"]:
                reset = True
            center_state["discontinuity"] = True
        else:
            center_state["discontinuity"] = False

        if reset:
            resets.append((center, session_id, window_sequence))
    return resets


class WaterfallLayoutRegressionTest(unittest.TestCase):
    def test_vertical_geometry_fills_exactly_600_pixels(self) -> None:
        header_height = integer_define(RF_UI_C, "RF_HEADER_HEIGHT")
        target_strip_y = integer_define(RF_UI_C, "RF_TARGET_STRIP_Y")
        target_strip_height = integer_define(RF_UI_C, "RF_TARGET_STRIP_HEIGHT")
        waterfall_y = integer_define(RF_UI_C, "RF_WATERFALL_Y")
        waterfall_height = integer_define(RF_UI_C, "RF_WATERFALL_HEIGHT")
        spectrum_y = integer_define(RF_UI_C, "RF_SPECTRUM_Y")
        spectrum_height = integer_define(RF_UI_C, "RF_SPECTRUM_HEIGHT")
        bottom_y = integer_define(RF_UI_C, "RF_BOTTOM_Y")
        bottom_height = integer_define(RF_UI_C, "RF_BOTTOM_HEIGHT")
        metrics_y = integer_define(RF_UI_C, "RF_METRICS_Y")
        metrics_height = integer_define(RF_UI_C, "RF_METRICS_HEIGHT")
        sidebar_y = integer_define(RF_UI_C, "RF_SIDEBAR_Y")
        sidebar_height = integer_define(RF_UI_C, "RF_SIDEBAR_HEIGHT")

        self.assertEqual(header_height, 54)
        self.assertEqual((target_strip_y, target_strip_height), (54, 58))
        self.assertEqual(target_strip_y, header_height)
        self.assertEqual((waterfall_y, waterfall_height), (112, 312))
        self.assertEqual(waterfall_y, target_strip_y + target_strip_height)
        self.assertEqual((spectrum_y, spectrum_height), (424, 104))
        self.assertEqual((bottom_y, bottom_height), (528, 48))
        self.assertEqual((metrics_y, metrics_height), (576, 24))
        self.assertEqual(spectrum_y, waterfall_y + waterfall_height)
        self.assertEqual(bottom_y, spectrum_y + spectrum_height)
        self.assertEqual(metrics_y, bottom_y + bottom_height)
        self.assertEqual(metrics_y + metrics_height, 600)
        self.assertEqual((sidebar_y, sidebar_height), (112, 464))
        self.assertEqual(sidebar_y + sidebar_height, metrics_y)

    def test_daylight_header_controls_are_touchable_without_overlap(self) -> None:
        mode_x = integer_define(RF_UI_C, "RF_MODE_X")
        mode_y = integer_define(RF_UI_C, "RF_MODE_Y")
        mode_width = integer_define(RF_UI_C, "RF_MODE_WIDTH")
        mode_height = integer_define(RF_UI_C, "RF_MODE_HEIGHT")
        mode_gap = integer_define(RF_UI_C, "RF_MODE_GAP")
        transport_x = integer_define(RF_UI_C, "RF_TRANSPORT_X")
        transport_y = integer_define(RF_UI_C, "RF_TRANSPORT_Y")
        transport_width = integer_define(RF_UI_C, "RF_TRANSPORT_WIDTH")
        transport_height = integer_define(RF_UI_C, "RF_TRANSPORT_HEIGHT")
        live_button_x = integer_define(RF_UI_C, "RF_LIVE_BUTTON_X")
        live_button_width = integer_define(RF_UI_C, "RF_LIVE_BUTTON_WIDTH")
        older_x = integer_define(RF_UI_C, "RF_HISTORY_OLDER_X")
        history_width = integer_define(RF_UI_C, "RF_HISTORY_BUTTON_WIDTH")
        timeline_x = integer_define(RF_UI_C, "RF_HISTORY_TIMELINE_X")
        timeline_width = integer_define(RF_UI_C, "RF_HISTORY_TIMELINE_WIDTH")
        newer_x = integer_define(RF_UI_C, "RF_HISTORY_NEWER_X")
        source_x = integer_define(RF_UI_C, "RF_SOURCE_BADGE_X")
        source_width = integer_define(RF_UI_C, "RF_SOURCE_BADGE_WIDTH")
        header_height = integer_define(RF_UI_C, "RF_HEADER_HEIGHT")
        touch_target = integer_define(RF_UI_C, "RF_TOUCH_TARGET")

        self.assertGreaterEqual(mode_height, touch_target)
        self.assertGreaterEqual(transport_height, touch_target)
        self.assertGreaterEqual(live_button_width, touch_target)
        self.assertGreaterEqual(touch_target, 44)
        self.assertLessEqual(mode_y + mode_height, header_height)
        self.assertLessEqual(transport_y + transport_height, header_height)
        self.assertLessEqual(mode_x + 2 * mode_width + mode_gap, transport_x)
        self.assertLessEqual(transport_x + transport_width, 1024)
        self.assertEqual(live_button_x + live_button_width, older_x)
        self.assertEqual(older_x + history_width, timeline_x)
        self.assertEqual(timeline_x + timeline_width, newer_x)
        self.assertEqual(newer_x + history_width, source_x)
        self.assertEqual(source_x + source_width, transport_width)
        self.assertIn('"全频扫描", "重点锁定"', RF_UI_C)

    def test_decorative_boxes_do_not_swallow_parent_button_hits(self) -> None:
        create_box = RF_UI_C.split(
            "static lv_obj_t * create_box", 1
        )[1].split("static lv_obj_t * create_label", 1)[0]
        self.assertIn(
            "lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE)", create_box
        )
        for control in (
            "g_ui.live_button",
            "g_ui.waterfall_image",
            "g_ui.history_slider",
        ):
            self.assertIn(
                f"lv_obj_add_flag({control}, LV_OBJ_FLAG_CLICKABLE)", RF_UI_C
            )
        self.assertIn("lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE)", RF_UI_C)

    def test_touch_poll_runs_before_bounded_display_work(self) -> None:
        init = LVGL_APP_C.split("fsp_err_t lvgl_app_init", 1)[1].split(
            "void lvgl_app_step", 1
        )[0]
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1].split(
            "void lvgl_app_runtime_metrics_get", 1
        )[0]
        touch_step = LVGL_APP_C.split(
            "static void lvgl_touch_poll_step", 1
        )[1].split("static uint32_t lvgl_tick_get_callback", 1)[0]

        self.assertEqual(integer_define(LVGL_APP_C, "UI_TOUCH_POLL_PERIOD_MS"), 10)
        self.assertIn("lv_timer_pause(touch_read_timer)", init)
        self.assertIn("lv_indev_read(g_touch_input)", touch_step)
        self.assertLess(
            owner.index("lvgl_touch_poll_step();"),
            owner.index("if (UI_SINGLE_FLOW_ENABLED)"),
        )
        self.assertIn("g_lvgl_app_input_diag", LVGL_APP_C)
        self.assertIn("g_rf_ui_input_diag", RF_UI_C)

    def test_b2_left_plot_and_right_status_columns_are_pinned(self) -> None:
        panel_width = integer_define(RF_UI_C, "RF_PANEL_WIDTH")
        sidebar_x = integer_define(RF_UI_C, "RF_SIDEBAR_X")
        sidebar_width = integer_define(RF_UI_C, "RF_SIDEBAR_WIDTH")
        waterfall_width = integer_define(RF_UI_C, "RF_WATERFALL_DISPLAY_WIDTH")
        waterfall_height = integer_define(RF_UI_C, "RF_WATERFALL_DISPLAY_HEIGHT")
        spectrum_width = integer_define(RF_UI_C, "RF_SPECTRUM_DISPLAY_WIDTH")
        spectrum_height = integer_define(RF_UI_C, "RF_SPECTRUM_DISPLAY_HEIGHT")
        plot_x = integer_define(RF_UI_C, "RF_PLOT_X")

        self.assertEqual((panel_width, sidebar_x, sidebar_width), (864, 864, 160))
        self.assertEqual(panel_width + sidebar_width, 1024)
        self.assertEqual((waterfall_width, waterfall_height), (800, 252))
        self.assertEqual((spectrum_width, spectrum_height), (800, 66))
        self.assertLessEqual(plot_x + waterfall_width, panel_width)
        self.assertLessEqual(plot_x + spectrum_width, panel_width)
        self.assertIn("static void create_sidebar(void)", RF_UI_C)
        self.assertIn('"当前目标"', RF_UI_C)
        self.assertIn('"回放"', RF_UI_C)
        self.assertIn("history_button_event", RF_UI_C)
        create = RF_UI_C.split("void rf_ui_create(void)", 1)[1].split(
            "void rf_ui_set_page", 1
        )[0]
        self.assertNotIn("create_compare_overlay();", create)

    def test_v27_3_plot_axes_and_channel_deck_match_the_studio_spec(self) -> None:
        spectrum = RF_UI_C.split("static void create_spectrum_panel", 1)[
            1
        ].split("static void create_waterfall_panel", 1)[0]
        waterfall = RF_UI_C.split("static void create_waterfall_panel", 1)[
            1
        ].split("static void create_compare_overlay", 1)[0]

        self.assertIn("lv_obj_set_pos(g_ui.spectrum_image, RF_PLOT_X, 20)", spectrum)
        self.assertIn('"-30", "-45", "-60", "-75", "-90"', spectrum)
        self.assertIn('"-98 ms", "-74 ms", "-49 ms", "-25 ms", "当前"', waterfall)
        self.assertIn("g_ui.spectrum_frequency_labels[index]", spectrum)
        self.assertIn("g_ui.waterfall_frequency_labels[index]", waterfall)
        self.assertIn("g_ui.waterfall_time_labels[index]", waterfall)
        self.assertEqual(integer_define(RF_UI_C, "RF_CHANNEL_DECK_WIDTH"), 864)
        self.assertEqual(integer_define(RF_UI_C, "RF_CHANNEL_CARD_WIDTH"), 216)

    def test_v27_3_rgb565_device_assets_match_the_approved_manifest(self) -> None:
        expected = {
            "rf_device_dji_mini_3_pro_strip": (
                52, 46, "15274E54B94E8313EF824CB2F0B737FB967713B75E7FA6282E00464468846F4C"
            ),
            "rf_device_dji_mini_3_pro_detail": (
                128, 112, "676D225F61F8354FDFDAFD1CB345CD442C5930DE404F29CF9C7CD2555F990332"
            ),
            "rf_device_xiaobawang_strip": (
                52, 46, "9AE73A0723E5BDDB4BCC3BF2F003363E75819BB4E70609181519AB3CCE8556C1"
            ),
            "rf_device_xiaobawang_detail": (
                128, 112, "D7FB671ED1A9ED0C77291291EC505AADC865BE4A1CE0C67629DBCC1C3BF4209C"
            ),
            "rf_device_radiolink_at9s_strip": (
                52, 46, "8D9E664CD41480BBE7D215031633CDC3C2C522B3F0B777ED07C1CC6510406C9A"
            ),
            "rf_device_radiolink_at9s_detail": (
                128, 112, "E546C442126436E036293EF84AA12DB9F3BB6E39396B6863D722B78293687051"
            ),
            "rf_device_yunzhuo_t12_strip": (
                52, 46, "E1C3C051A32C4F8670F464D0BB89DEF6C88B6494FE2D598216525216B73C2151"
            ),
            "rf_device_yunzhuo_t12_detail": (
                128, 112, "279CC095FC9CB6C3EF3835118E9F8B84E34BEE022AF4AF0C62BD5247908C2E66"
            ),
        }
        total_bytes = 0
        for symbol, (width, height, sha256) in expected.items():
            raw_match = re.search(
                rf"{symbol}_map\[\]\s*=\s*\{{(.*?)\}};",
                RF_DEVICE_C,
                re.DOTALL,
            )
            self.assertIsNotNone(raw_match, symbol)
            raw = bytes(
                int(value, 16)
                for value in re.findall(r"0x([0-9A-Fa-f]{2})", raw_match.group(1))
            )
            self.assertEqual(len(raw), width * height * 2, symbol)
            self.assertEqual(hashlib.sha256(raw).hexdigest().upper(), sha256, symbol)
            total_bytes += len(raw)

            descriptor = RF_DEVICE_C.split(
                f"const lv_image_dsc_t {symbol}", 1
            )[1].split("};", 1)[0]
            self.assertIn(".header.cf = LV_COLOR_FORMAT_RGB565", descriptor)
            self.assertIn(f".header.w = {width}", descriptor)
            self.assertIn(f".header.h = {height}", descriptor)
            self.assertIn(f".data = {symbol}_map", descriptor)
            self.assertIn(f"LV_IMAGE_DECLARE({symbol});", RF_DEVICE_H)

        self.assertEqual(total_bytes, 133824)
        firmware_ui = RF_UI_C + RF_DEVICE_C + RF_DEVICE_H
        for web_token in ("React", "className=", "<header", ".device-header"):
            self.assertNotIn(web_token, firmware_ui)

    def test_simhei_subset_covers_every_firmware_cjk_label(self) -> None:
        literals = re.findall(r'"(?:\\.|[^"\\])*"', RF_UI_C)
        used = set(re.findall(r"[\u3400-\u9fff]", "".join(literals)))
        self.assertEqual(used - set(RF_GLYPHS), set())
        self.assertIn(RF_GLYPHS.strip(), RF_FONT_C[:4096])

    def test_spectrum_ui_contract_accepts_only_native_256_points(self) -> None:
        self.assertEqual(integer_define(RF_UI_H, "RF_UI_SPECTRUM_BINS"), 256)
        self.assertEqual(integer_define(RF_DEMO_H, "RF_DEMO_SPECTRUM_BINS"), 128)
        self.assertEqual(integer_define(RF_UI_C, "RF_SPECTRUM_TEXTURE_WIDTH"), 400)
        self.assertEqual(
            integer_define(RF_UI_C, "RF_SPECTRUM_TEXTURE_STRIDE_PIXELS"), 416
        )
        self.assertIn("sizeof(g_spectrum_pixels[0]) == 0x8200u", RF_UI_C)
        self.assertIn("sizeof(g_spectrum_pixels) == 0x10400u", RF_UI_C)
        update = RF_UI_C.split("static bool update_spectrum_internal", 1)[1].split(
            "bool rf_ui_present_spectrum", 1
        )[0]
        self.assertIn("bin_count != RF_UI_SPECTRUM_BINS", update)
        self.assertIn("memcpy(g_spectrum_data[channel_index]", update)
        self.assertNotIn("RF_SPECTRUM_COMPAT_INPUT_BINS", RF_UI_C)
        self.assertIn("Lower-resolution inputs are", RF_UI_H)

    def test_dual_mapped_ring_dimensions_are_pinned(self) -> None:
        self.assertEqual(integer_define(RF_UI_H, "RF_UI_WATERFALL_FREQ_BINS"), 192)
        self.assertEqual(integer_define(RF_UI_H, "RF_UI_WATERFALL_COLS"), 160)
        self.assertEqual(
            integer_define(RF_UI_H, "RF_UI_WATERFALL_HISTORY_COLS"), 256
        )
        self.assertIn(
            "#define RF_UI_WATERFALL_STORAGE_COLS (RF_UI_WATERFALL_HISTORY_COLS * 2u)",
            RF_UI_H,
        )
        self.assertIn(
            "rows[RF_UI_WATERFALL_FREQ_BINS][RF_UI_WATERFALL_STORAGE_COLS]",
            RF_UI_C,
        )
        self.assertIn("sizeof(g_waterfall_rings) == 0xC0000u", RF_UI_C)
        self.assertIn("sizeof(g_waterfall_pause_snapshot) == 0x18000u", RF_UI_C)
        self.assertIn("sizeof(g_waterfall_render_rings[0]) == 0xC5440u", RF_UI_C)
        self.assertIn("sizeof(g_waterfall_render_rings) == 0x18A880u", RF_UI_C)
        self.assertIn("== 0xCD640u", RF_UI_C)
        self.assertNotIn("memmove(", RF_UI_C)

    def test_each_real_partial_tile_row_generates_one_time_column(self) -> None:
        append = LVGL_APP_C.split(
            "static bool ui_flow_append_waterfall_tile", 1
        )[1].split("static void ui_update_class_strip", 1)[0]
        self.assertIn("rf_ui_update_waterfall_rows", append)
        self.assertIn("tile->levels", append)
        self.assertGreaterEqual(append.count("RA8P1_DISPLAY_TILE_ROW_BYTES"), 2)
        self.assertIn("1U,", append)
        self.assertIn("g_ui_waterfall_columns_generated++;", append)
        self.assertNotIn("g_flow_waterfall", LVGL_APP_C)
        self.assertNotIn("peak", append)
        self.assertNotIn("if (level >", append)

    def test_rf_timebase_is_about_98_ms_not_transport_arrival_time(self) -> None:
        window_samples = integer_define(
            RF_UI_C, "RF_WATERFALL_RF_WINDOW_SAMPLES"
        )
        sample_rate_hz = integer_define(
            RF_UI_C, "RF_WATERFALL_RF_SAMPLE_RATE_HZ"
        )
        rows_per_window = integer_define(
            RF_UI_C, "RF_WATERFALL_RF_ROWS_PER_WINDOW"
        )
        history_columns = integer_define(RF_UI_H, "RF_UI_WATERFALL_COLS")
        retained_columns = integer_define(
            RF_UI_H, "RF_UI_WATERFALL_HISTORY_COLS"
        )

        self.assertEqual(window_samples, 590336)
        self.assertEqual(sample_rate_hz, 60000000)
        self.assertEqual(rows_per_window, 16)
        self.assertEqual(window_samples % rows_per_window, 0)
        samples_per_column = window_samples // rows_per_window
        column_ms = samples_per_column * 1000.0 / sample_rate_hz
        history_ms = history_columns * column_ms
        self.assertAlmostEqual(column_ms, 0.6149333333, places=9)
        retained_ms = retained_columns * column_ms
        self.assertAlmostEqual(history_ms, 98.3893333333, places=9)
        self.assertAlmostEqual(retained_ms, 157.4229333333, places=9)
        self.assertGreaterEqual(history_ms, 98.0)
        self.assertLessEqual(history_ms, 99.0)
        self.assertIn('"-98 ms"', RF_UI_C)
        self.assertIn('"-49 ms"', RF_UI_C)
        self.assertIn('"当前"', RF_UI_C)
        self.assertIn("98.39 ms", RF_UI_C)

    def test_frequency_axis_is_high_to_low_and_time_columns_fill_native_width(self) -> None:
        self.assertIn(
            "RF_UI_WATERFALL_FREQ_BINS - 1U - display_row", RF_UI_C
        )
        display_width = integer_define(RF_UI_C, "RF_WATERFALL_DISPLAY_WIDTH")
        history_columns = integer_define(RF_UI_H, "RF_UI_WATERFALL_COLS")
        self.assertEqual(display_width, 800)
        boundaries = [
            column * display_width // history_columns
            for column in range(history_columns + 1)
        ]
        widths = [right - left for left, right in zip(boundaries, boundaries[1:])]
        self.assertEqual((boundaries[0], boundaries[-1]), (0, display_width))
        self.assertEqual(sum(widths), display_width)
        self.assertEqual(set(widths), {5})

        lookup = RF_UI_C.split("static void prepare_waterfall_lookup_tables", 1)[
            1
        ].split("static void waterfall_image_head_update", 1)[0]
        push = RF_UI_C.split("static void push_waterfall_column", 1)[1].split(
            "static void push_waterfall_gap_column", 1
        )[0]
        head = RF_UI_C.split("static void waterfall_image_head_update", 1)[
            1
        ].split("static void waterfall_render_clear", 1)[0]
        self.assertIn("g_waterfall_render_x[column]", lookup)
        self.assertIn("column * RF_WATERFALL_DISPLAY_WIDTH", lookup)
        self.assertIn("g_waterfall_render_x[g_waterfall_render_write_column]", push)
        self.assertIn("render_width", push)
        self.assertIn("fill_row(&render_row[render_start], pixel, render_width)", push)
        self.assertIn("g_waterfall_render_x", head)
        self.assertIn("g_waterfall_render_write_column", head)
        self.assertNotIn("RF_WATERFALL_PIXELS_PER_COLUMN", RF_UI_C)
        self.assertIn(
            "lv_image_set_scale_x(g_ui.waterfall_image, LV_SCALE_NONE)",
            RF_UI_C,
        )
        self.assertIn("descriptor->header.w = RF_WATERFALL_DISPLAY_WIDTH", RF_UI_C)

        # The source pointer may start at the final expanded column.  The
        # dual-mapped row must still contain a complete 800-pixel read span.
        storage_width = display_width * 2
        maximum_head = boundaries[-2]
        self.assertLess(maximum_head + display_width, storage_width)
        self.assertIn("readable_tail[RF_WATERFALL_DISPLAY_WIDTH]", RF_UI_C)

        fill = RF_UI_C.split("static void fill_row", 1)[1].split(
            "static void spectrum_put_pixel", 1
        )[0]
        self.assertIn("memcpy(row, &paired, sizeof(paired))", fill)
        self.assertIn("(uintptr_t)row & 3U", fill)
        self.assertNotIn("uint32_t * destination", fill)

    def test_waterfall_submission_latency_is_bounded_to_five_ms(self) -> None:
        self.assertEqual(
            integer_define(LVGL_APP_C, "UI_WATERFALL_PRESENT_PERIOD_MS"), 5
        )
        self.assertEqual(
            integer_define(LVGL_APP_C, "UI_DISPLAY_REFRESH_PERIOD_MS"), 5
        )
        step = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        self.assertIn("rf_ui_present_waterfall()", step)
        self.assertIn("UI_WATERFALL_PRESENT_PERIOD_MS", step)
        self.assertNotIn("UI_WATERFALL_UPDATE_PERIOD_MS", LVGL_APP_C)

        push = RF_UI_C.split("static void push_waterfall_column", 1)[1].split(
            "static void format_millihz", 1
        )[0]
        present = RF_UI_C.split("bool rf_ui_present_waterfall", 1)[1].split(
            "bool rf_ui_update_channel_metrics", 1
        )[0]
        self.assertNotIn("waterfall_image_head_update();", push)
        self.assertIn("waterfall_image_head_update();", present)

    def test_spectrum_ingest_is_cached_then_presented_at_bounded_cadence(self) -> None:
        update = RF_UI_C.split("static bool update_spectrum_internal", 1)[1].split(
            "bool rf_ui_present_spectrum", 1
        )[0]
        present = RF_UI_C.split("bool rf_ui_present_spectrum", 1)[1].split(
            "bool rf_ui_update_waterfall", 1
        )[0]
        signal_update = LVGL_APP_C.split("void lvgl_app_signal_update", 1)[1].split(
            "void lvgl_app_signal_reset", 1
        )[0]
        step = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        single_flow = step.split("if (UI_SINGLE_FLOW_ENABLED)", 1)[1].split(
            "ui_poll_glcdc_layer2_underflow();", 1
        )[0]
        present_success = single_flow.split("if (rf_ui_present_spectrum())", 1)[
            1
        ].split("if ((!g_waterfall_present_valid", 1)[0]
        retry = LVGL_APP_C.split(
            "static void ui_visibility_retry_after_flush_failure", 1
        )[1].split("static uint16_t ui_rgb565", 1)[0]

        self.assertIn("bin_count != RF_UI_SPECTRUM_BINS", update)
        self.assertIn("memcpy(g_spectrum_data[channel_index]", update)
        self.assertIn("g_ui.spectrum_dirty[channel_index] = true", update)
        self.assertNotIn("rasterize_spectrum", update)
        self.assertNotIn("lv_obj_invalidate", update)
        self.assertIn("rasterize_spectrum_to", present)
        self.assertIn("lv_obj_invalidate(g_ui.spectrum_image)", present)
        self.assertIn("g_ui.spectrum_dirty[channel] = false", present)
        self.assertIn("bool rf_ui_present_spectrum(void);", RF_UI_H)

        self.assertEqual(
            integer_define(LVGL_APP_C, "UI_SPECTRUM_UPDATE_PERIOD_MS"), 100
        )
        self.assertIn("rf_ui_present_spectrum()", step)
        self.assertIn("UI_SPECTRUM_UPDATE_PERIOD_MS", step)
        self.assertNotIn("spectrum_start_cycles", signal_update)
        self.assertIn(
            "center_index != rf_ui_get_selected_channel()", signal_update
        )
        self.assertIn("ui_visibility_content_prepared();", signal_update)
        self.assertIn("ui_visibility_content_prepared();", present_success)
        self.assertLess(
            present_success.index("ui_frame_center_index(&g_live_signal_frame)"),
            present_success.index("ui_visibility_content_prepared();"),
        )
        self.assertIn("center == selected_center", retry)
        self.assertIn("ui_visibility_content_prepared();", retry)
        self.assertIn("rf_ui_force_channel_result_redraw(center)", retry)
        self.assertIn("g_spectrum_present_valid = false", retry)

    def test_192_bin_waterfall_hot_path_preserves_generic_mapping(self) -> None:
        self.assertEqual(
            integer_define(RF_UI_C, "RF_WATERFALL_FAST_FREQ_BINS"), 192
        )
        self.assertIn("g_waterfall_color_lut[256u]", RF_UI_C)
        self.assertIn(
            "g_waterfall_source_bin_fast[RF_UI_WATERFALL_FREQ_BINS]", RF_UI_C
        )
        table = RF_UI_C.split("static void prepare_waterfall_lookup_tables", 1)[
            1
        ].split("static void waterfall_image_head_update", 1)[0]
        push = RF_UI_C.split("static void push_waterfall_column", 1)[1].split(
            "static void push_waterfall_gap_column", 1
        )[0]

        self.assertIn("intensity < 256U", table)
        self.assertIn("g_waterfall_color_lut[intensity] = rgb565(rgb)", table)
        self.assertIn(
            "RF_UI_WATERFALL_FREQ_BINS - 1U - display_row", table
        )
        self.assertIn("RF_WATERFALL_FAST_FREQ_BINS - 1U", table)
        self.assertIn(
            "frequency_bin_count == RF_WATERFALL_FAST_FREQ_BINS", push
        )
        self.assertIn("g_waterfall_source_bin_fast[display_row]", push)
        self.assertIn("(uint32_t)frequency_bin_count - 1U", push)

        create = RF_UI_C.split("void rf_ui_create(void)", 1)[1].split(
            "void rf_ui_set_page", 1
        )[0]
        self.assertLess(
            create.index("prepare_waterfall_lookup_tables();"),
            create.index("prepare_waterfall_images();"),
        )

        mapped_bins = [191 - row for row in range(192)]
        generic_bins = [
            ((192 - 1 - row) * (192 - 1)) // (192 - 1) for row in range(192)
        ]
        self.assertEqual(mapped_bins, generic_bins)
        self.assertEqual(mapped_bins[0], 191)
        self.assertEqual(mapped_bins[-1], 0)
        self.assertEqual(sorted(mapped_bins), list(range(192)))
        self.assertTrue(
            all(left >= right for left, right in zip(mapped_bins, mapped_bins[1:]))
        )

    def test_missing_tile_sequences_insert_unknown_columns_before_real_data(self) -> None:
        self.assertEqual(
            model_sequence_events([(1, 2), (1, 4), (1, 8), (1, 10)]),
            ["data", "data", "gap", "data", "data"],
        )
        self.assertEqual(
            model_sequence_events([(1, 8)]),
            ["gap", "gap", "gap", "data"],
        )
        self.assertEqual(
            model_sequence_events([(1, 8), (2, 2)]),
            ["gap", "gap", "gap", "data", "data"],
        )
        self.assertEqual(
            model_sequence_events([(1, 2 + 2 * 100)]).count("gap"), 100
        )
        self.assertEqual(
            model_sequence_events([(1, 2 + 2 * 300)]).count("gap"), 256
        )

        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]
        gap_call = tile_update.index("rf_ui_append_waterfall_gap_columns")
        real_call = tile_update.index("ui_flow_append_waterfall_tile(tile)")
        self.assertLess(gap_call, real_call)
        self.assertIn("g_ui_waterfall_tiles_dropped += gap_columns", tile_update)

        gap_api = RF_UI_C.split(
            "bool rf_ui_append_waterfall_gap_columns", 1
        )[1].split("bool rf_ui_present_waterfall", 1)[0]
        gap_writer = RF_UI_C.split(
            "static void push_waterfall_gap_column", 1
        )[1].split("static void format_millihz", 1)[0]
        self.assertIn("column_count > RF_UI_WATERFALL_HISTORY_COLS", gap_api)
        self.assertIn("push_waterfall_gap_column", gap_api)
        self.assertIn("RF_COLOR_GAP_A", gap_writer)
        self.assertIn("RF_COLOR_GAP_B", gap_writer)
        self.assertNotIn("waterfall_pixel(0U)", gap_writer)

    def test_planned_center_rotation_preserves_each_channel_history(self) -> None:
        self.assertEqual(
            model_center_guard_events([(1, 2, 0), (1, 4, 1)]),
            ["data:0", "data:1"],
        )
        self.assertEqual(
            model_center_guard_events([(1, 2, 0), (1, 4, 1), (1, 8, 1)]),
            ["data:0", "data:1", "unknown:0,1", "data:1"],
        )

        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]
        rotation = tile_update.index("A planned four-frequency scan")
        gap = tile_update.index("if ((gap_columns != 0U)", rotation)
        clear_loop = tile_update.index("center < RA8P1_CENTER_COUNT", gap)
        real = tile_update.index("ui_flow_append_waterfall_tile(tile)", clear_loop)
        self.assertLess(rotation, gap)
        self.assertLess(gap, clear_loop)
        self.assertLess(clear_loop, real)
        self.assertIn("g_tile_session_center_mask", tile_update)
        self.assertNotIn("const bool center_conflict", tile_update)

        history_guard = LVGL_APP_C.split(
            "static bool ui_tile_history_reset_required", 1
        )[1].split("static bool ui_frame_identity_matches", 1)[0]
        session_guard = history_guard.split("for (uint32_t slot", 1)[0]
        self.assertIn("fresh display session", session_guard)
        self.assertNotIn("const bool reconnect", session_guard)
        self.assertNotIn("reset_history = true", session_guard)

    def test_retry_and_discontinuity_reset_unknown_history(self) -> None:
        one_window = [
            (0, 9, 3, row, 2 + row * 2, True) for row in range(16)
        ]
        self.assertEqual(model_logical_history_resets(one_window), [(0, 9, 3)])

        # The two overlapping analysis lanes interleave logical windows. A
        # single source discontinuity still clears the history only once.
        self.assertEqual(
            model_logical_history_resets(
                [
                    (0, 9, 3, 0, 2, True),
                    (0, 9, 4, 0, 4, True),
                    (0, 9, 3, 1, 6, True),
                    (0, 9, 4, 1, 8, True),
                ]
            ),
            [(0, 9, 3)],
        )

        # Ordinary RETRY does not set DISCONTINUITY. Repeated logical row zero
        # must still separate the failed and replayed RF attempts.
        self.assertEqual(
            model_logical_history_resets(
                [
                    (0, 9, 0, 0, 2, False),
                    (0, 9, 0, 1, 4, False),
                    (0, 9, 0, 0, 6, False),
                    (0, 9, 0, 1, 8, False),
                ]
            ),
            [(0, 9, 0)],
        )

        # If retry rows zero through two were overwritten, the transport gap
        # makes a still-increasing first visible row conservative unknown.
        self.assertEqual(
            model_logical_history_resets(
                [
                    (0, 9, 3, 0, 2, True),
                    (0, 9, 3, 1, 4, True),
                    (0, 9, 3, 3, 12, True),
                ]
            ),
            [(0, 9, 3), (0, 9, 3)],
        )

        # A whole-session retry is also detectable after the original window
        # state has been evicted by the two-lane tracker.
        self.assertEqual(
            model_logical_history_resets(
                [
                    (0, 9, 0, 15, 2, False),
                    (0, 9, 1, 15, 4, False),
                    (0, 9, 2, 0, 6, False),
                    (0, 9, 0, 3, 8, False),
                ]
            ),
            [(0, 9, 0)],
        )

        history_guard = LVGL_APP_C.split(
            "static bool ui_tile_history_reset_required", 1
        )[1].split("static bool ui_frame_identity_matches", 1)[0]
        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]
        self.assertIn("UI_TILE_LOGICAL_RUN_SLOTS", history_guard)
        self.assertIn("tile->novel_time_start <= run->last_time_start", history_guard)
        self.assertIn("gap_columns != 0U", history_guard)
        self.assertIn("g_tile_logical_max_window", history_guard)
        self.assertIn("RA8P1_DISPLAY_FLAG_DISCONTINUITY", history_guard)
        reset = tile_update.index("ui_tile_history_reset_required")
        unknown = tile_update.index(
            "unknown_history_mask |= current_center_mask", reset
        )
        clear = tile_update.index("rf_ui_append_waterfall_gap_columns(", unknown)
        real = tile_update.index("ui_flow_append_waterfall_tile(tile)", clear)
        self.assertLess(reset, unknown)
        self.assertLess(unknown, clear)
        self.assertLess(clear, real)
        self.assertIn("RF_UI_WATERFALL_HISTORY_COLS", tile_update[clear:real])

    def test_vsync_wait_does_not_ingest_tiles_during_active_scan(self) -> None:
        wait = LVGL_APP_C.split("static void lvgl_flush_wait_callback", 1)[1].split(
            "static void lvgl_touch_read_callback", 1
        )[0]
        self.assertIn(
            "while (g_flush_line_event == g_display_diag.glcdc_line_events)", wait
        )
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_FLUSH_WAIT", wait)
        self.assertNotIn("display_app_drain_tiles_bounded(", wait)

        bounded = DISPLAY_APP_C.split(
            "void display_app_drain_tiles_bounded", 1
        )[1].split("void display_app_drain_tiles(void)", 1)[0]
        drain = DISPLAY_APP_C.split("void display_app_drain_tiles(void)", 1)[1].split(
            "static void display_app_panel_presentation_service", 1
        )[0]
        self.assertIn("tile_index < max_tiles", bounded)
        self.assertIn("ipc_bridge_cpu1_display_tile_poll", bounded)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_TILE_DRAIN", bounded)
        self.assertEqual(
            integer_define(DISPLAY_APP_C, "DISPLAY_APP_TILE_DRAIN_BUDGET"), 4
        )
        self.assertIn(
            "display_app_drain_tiles_bounded(DISPLAY_APP_TILE_DRAIN_BUDGET)",
            drain,
        )

    def test_scan_and_focus_controls_reconfigure_the_capture_scheduler(self) -> None:
        selector = RF_UI_C.split("static void selector_click_event", 1)[1].split(
            "static void acquisition_mode_click_event", 1
        )[0]
        mode = RF_UI_C.split("static void acquisition_mode_click_event", 1)[1].split(
            "static void create_acquisition_modes", 1
        )[0]

        self.assertIn("g_ui.focus_mode", selector)
        self.assertIn("display_app_request_focus(channel)", selector)
        self.assertIn("channel == g_ui.committed_channel", selector)
        self.assertLess(
            selector.index("channel == g_ui.committed_channel"),
            selector.index("rf_ui_set_selected_channel(channel)"),
        )
        self.assertIn("display_app_request_scan()", mode)
        self.assertIn("display_app_request_focus(g_ui.committed_channel)", mode)
        self.assertIn("if(accepted)", mode)
        self.assertIn(
            "rf_ui_set_focus_mode(mode == RF_ACQUISITION_FOCUS)", mode
        )
        self.assertIn("bool display_app_request_focus(uint32_t center_index)", DISPLAY_APP_C)
        self.assertIn("bool display_app_request_scan(void)", DISPLAY_APP_C)

        create = RF_UI_C.split("void rf_ui_create(void)", 1)[1].split(
            "void rf_ui_set_page", 1
        )[0]
        initial = DISPLAY_APP_C.split("void display_app_init(void)", 1)[1].split(
            "void display_app_step(void)", 1
        )[0]
        self.assertIn("g_ui.focus_mode = false", create)
        self.assertIn("RA8P1_CENTER_2420_HZ", initial)
        self.assertIn("RA8P1_CENTER_2464_HZ", initial)
        self.assertNotIn("0ULL", initial)
        self.assertIn("RA8P1_COMMAND_FLAG_SCAN_ALL", DISPLAY_APP_C)
        self.assertIn("RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS", DISPLAY_APP_C)

    def test_focus_view_and_four_frequency_work_state_have_separate_sources(self) -> None:
        center_frame = LVGL_APP_C.split(
            "static const ra8p1_display_frame_t * ui_center_frame", 1
        )[1].split("static uint8_t ui_channel_level", 1)[0]
        presence = LVGL_APP_C.split(
            "static uint32_t ui_presence_q15_fused", 1
        )[1].split("static uint32_t ui_presence_percent", 1)[0]
        detections = LVGL_APP_C.split(
            "static void ui_flow_update_detections", 1
        )[1].split("static bool ui_flow_rf_window_complete", 1)[0]

        self.assertIn("rf_ui_is_focus_mode()", center_frame)
        self.assertIn("rf_ui_get_selected_channel()", center_frame)
        self.assertIn("ui_center_frame(center)", presence)
        self.assertNotIn("g_center_frames[center]", presence)
        self.assertIn("rf_v25_activity_service_snapshot", detections)
        self.assertIn("rf_v25_activity_fusion_get", detections)
        self.assertIn("RF_V25_ACTIVITY_WORKING", detections)
        self.assertIn("RF_V25_ACTIVITY_UNCERTAIN", detections)
        self.assertIn("last_positive_center_slot", detections)
        self.assertNotIn("ui_center_frame(center)", detections)
        self.assertNotIn("presence_q15", detections)
        self.assertIn("bool rf_ui_is_focus_mode(void);", RF_UI_H)

    def test_real_rf_boxes_are_anchored_after_the_matching_waterfall_window(self) -> None:
        prepare = LVGL_APP_C.split(
            "static void ui_flow_prepare_rf_boxes", 1
        )[1].split("static void ui_flow_commit_rf_boxes_for_tile", 1)[0]
        commit = LVGL_APP_C.split(
            "static void ui_flow_commit_rf_boxes_for_tile", 1
        )[1].split("void lvgl_app_activity_update", 1)[0]
        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]
        overlay = RF_UI_C.split("static void refresh_rf_box_overlays", 1)[1].split(
            "static uint16_t rgb565", 1
        )[0]
        box_api = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "void rf_ui_mark_channel_result", 1
        )[0]

        for field in (
            "frequency_start_q8",
            "time_start_q8",
            "frequency_span_q8",
            "time_span_q8",
        ):
            self.assertIn(field, prepare)
            self.assertIn(field, RF_UI_H)
        self.assertIn("RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID", prepare)
        self.assertIn("ui_flow_object_to_detection_index", prepare)
        self.assertIn("RA8P1_DISPLAY_TILE_HEIGHT - 1U", commit)
        self.assertLess(
            tile_update.index("ui_flow_append_waterfall_tile(tile)"),
            tile_update.index("ui_flow_commit_rf_boxes_for_tile(tile)"),
        )
        self.assertIn("rf_box_window_anchor_find", box_api)
        self.assertIn("incoming.anchor_end_column = waterfall_end_column", box_api)
        self.assertIn("anchor_end_column", overlay)
        self.assertIn("RF_WATERFALL_RF_ROWS_PER_WINDOW", overlay)
        self.assertIn("waterfall_rf_boxes", overlay)
        self.assertIn("spectrum_rf_boxes", overlay)

    def test_only_same_round_working_boxes_are_baked_into_history(self) -> None:
        history_raster = RF_UI_C.split(
            "static uint32_t waterfall_history_box_raster", 1
        )[1].split("static void waterfall_overlay_pixel_set", 1)[0]
        clut_map = RF_UI_C.split(
            "static void prepare_waterfall_lookup_tables", 1
        )[1].split("static void waterfall_overlay_history_pixel_write", 1)[0]
        resolver = RF_UI_C.split("static void rf_box_batch_resolve", 1)[1].split(
            "static rf_ui_fusion_decision_cache_t", 1
        )[0]
        box_api = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "void rf_ui_reset_rf_box_fusion", 1
        )[0]
        pause = RF_UI_C.split("static void waterfall_pause_snapshot_capture", 1)[
            1
        ].split("static void waterfall_render_rebuild_paused", 1)[0]

        self.assertIn("waterfall_box_history_bounds", history_raster)
        self.assertIn("waterfall_box_border_cell", history_raster)
        self.assertIn("g_waterfall_rings[channel].rows", history_raster)
        self.assertIn(
            "history_column + RF_UI_WATERFALL_HISTORY_COLS",
            history_raster,
        )
        self.assertIn("g_target_accent_colors[box->detection_index]", history_raster)
        self.assertIn("RF_WATERFALL_CLUT_BOX_FIRST + index", clut_map)
        self.assertIn("activity == RF_UI_FUSION_WORKING", resolver)
        self.assertIn("waterfall_history_box_raster(", resolver)
        self.assertIn("waterfall_overlay_box_raster_matching_sources(", resolver)
        self.assertNotIn("waterfall_history_box_raster(", box_api)
        self.assertIn("g_pending_box_batches", RF_UI_C)
        self.assertIn("one RF box batch exceeds the SDRAM write budget", RF_UI_C)
        self.assertIn("live_build_cancel(true)", RF_UI_C)
        self.assertLess(
            resolver.index("activity == RF_UI_FUSION_WORKING"),
            resolver.index("waterfall_history_box_raster("),
        )
        self.assertIn("g_waterfall_rings[channel].rows", pause)

    def test_spectrum_boxes_use_only_the_current_complete_window(self) -> None:
        overlay = RF_UI_C.split("static void refresh_rf_box_overlays", 1)[1].split(
            "static uint16_t rgb565", 1
        )[0]
        box_api = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "void rf_ui_reset_rf_box_fusion", 1
        )[0]
        resolver = RF_UI_C.split("static void rf_box_batch_resolve", 1)[1].split(
            "static rf_ui_fusion_decision_cache_t", 1
        )[0]
        running = RF_UI_C.split("void rf_ui_set_running", 1)[1].split(
            "int rf_ui_is_running", 1
        )[0]

        self.assertIn("g_spectrum_rf_box_batches[g_ui.committed_channel]", overlay)
        self.assertIn("g_spectrum_rf_box_pause_snapshot", overlay)
        self.assertIn("g_rf_box_batches[g_ui.committed_channel]", overlay)
        self.assertIn("rf_ui_rf_box_batch_t spectrum_next = {0}", resolver)
        self.assertIn(
            "g_spectrum_rf_box_batches[batch->channel] = spectrum_next", resolver
        )
        self.assertIn("memset(&g_spectrum_rf_box_batches[channel_index]", box_api)
        self.assertIn("rf_box_detail_update", resolver)
        self.assertNotIn("waterfall_history_box_raster", box_api)
        self.assertIn("g_spectrum_rf_box_pause_snapshot =", running)

    def test_idle_fused_target_remains_selectable_in_detail(self) -> None:
        detail = RF_UI_C.split("static void refresh_target_detail(void)", 1)[1].split(
            "static void refresh_selected_view", 1
        )[0]
        metrics = RF_UI_C.split("static void refresh_selected_metric", 1)[1].split(
            "static void refresh_target_detail_surface", 1
        )[0]
        cards = RF_UI_C.split("static void refresh_target_cards", 1)[1].split(
            "static void refresh_compare_overlay", 1
        )[0]
        sidebar = RF_UI_C.split("static void create_sidebar(void)", 1)[1].split(
            "static void create_plots", 1
        )[0]
        alert = RF_UI_C.split("static void refresh_alert", 1)[1].split(
            "static void select_target_index", 1
        )[0]
        select = RF_UI_C.split("static void select_target_index", 1)[1].split(
            "static void target_click_event", 1
        )[0]
        click = RF_UI_C.split("static void target_click_event", 1)[1].split(
            "static void compare_target_click_event", 1
        )[0]

        self.assertIn("g_ui.selected_detection_index = -1", RF_UI_C)
        self.assertNotIn("find_best_detection", RF_UI_C)
        self.assertNotIn("selected_detection_index", alert)
        self.assertIn("selection_present", detail)
        self.assertIn("detection_online", detail)
        self.assertNotIn("g_detections[(uint32_t)selected].channel_index", detail)
        self.assertIn("g_detections", detail)
        self.assertIn('"空闲"', detail)
        self.assertIn("detail_selected", metrics)
        self.assertNotIn("detection_online", metrics)
        self.assertIn("lv_obj_set_style_image_recolor_opa", detail)
        self.assertIn("working ? LV_OPA_TRANSP : LV_OPA_COVER", detail)
        self.assertIn("lv_obj_set_style_image_recolor_opa", cards)
        self.assertNotIn("const bool selected = online", cards)
        self.assertNotIn("if(!online) return", select)
        self.assertIn("online && target_channel != g_ui.committed_channel", select)
        self.assertNotIn("detection_online(index)", click)
        self.assertIn("g_ui.selected_detection_index = -1", select)
        confidence_value = sidebar.split(
            "g_ui.alert_idle_label = create_label", 1
        )[1].split(";", 1)[0]
        self.assertIn("&rf_font_zh_14", confidence_value)

    def test_idle_targets_hide_frequency_ranges(self) -> None:
        cards = RF_UI_C.split("static void refresh_target_cards", 1)[1].split(
            "static void refresh_compare_overlay", 1
        )[0]
        detail = RF_UI_C.split("static void refresh_target_detail(void)", 1)[
            1
        ].split("static void refresh_selected_view", 1)[0]
        empty_detail = detail.split("if(!selection_present)", 1)[1].split(
            "else {", 1
        )[0]

        self.assertIn("if(online)", cards)
        self.assertIn(
            'lv_label_set_text(g_ui.target_channel_labels[index], "")',
            cards,
        )
        self.assertIn(
            "working && find_last_detection_box(target, &last_ref)", detail
        )
        self.assertIn("if(working && !range_present)", detail)
        self.assertIn(
            "set_visible(g_ui.detail_range_name_labels[0], false)",
            empty_detail,
        )
        self.assertIn(
            "set_visible(g_ui.detail_range_value_labels[0], false)",
            empty_detail,
        )

    def test_rate_windows_do_not_reset_on_session_changes(self) -> None:
        result_rate = LVGL_APP_C.split("if (!g_result_rate_valid)", 1)[1].split(
            "g_live_signal_frame = *frame", 1
        )[0]
        tile_rate = LVGL_APP_C.split("if (!g_tile_rate_valid)", 1)[1].split(
            "if (g_tile_last_received_session", 1
        )[0]
        self.assertNotIn("session_id", result_rate)
        self.assertNotIn("session_id", tile_rate)
        self.assertNotIn("g_result_rate_session", LVGL_APP_C)
        self.assertNotIn("g_tile_rate_session", LVGL_APP_C)

    def test_history_labels_are_sample_derived_not_tile_rate_derived(self) -> None:
        timing = RF_UI_C.split("static void refresh_waterfall_timing", 1)[1].split(
            "static void refresh_live_state", 1
        )[0]
        self.assertNotIn("g_global_tile_rate_millihz", timing)
        self.assertIn("RF_WATERFALL_RF_WINDOW_SAMPLES /", timing)
        self.assertIn("RF_WATERFALL_RF_SAMPLE_RATE_HZ", timing)
        self.assertIn("RF_UI_WATERFALL_COLS * 100000ULL", timing)
        self.assertIn('"%u RF 行 | %u.%03u ms/列 | %u.%02u ms"', timing)
        for misleading in ("1.28 s", "4.48 s", "10 ms/COL", "448 COL"):
            self.assertNotIn(misleading, RF_UI_C)

    def test_pause_freezes_a_snapshot_while_ingestion_keeps_advancing(self) -> None:
        setter = RF_UI_C.split("void rf_ui_set_running", 1)[1].split(
            "int rf_ui_is_running", 1
        )[0]
        spectrum_present = RF_UI_C.split("bool rf_ui_present_spectrum", 1)[1].split(
            "bool rf_ui_update_waterfall", 1
        )[0]
        waterfall_present = RF_UI_C.split("bool rf_ui_present_waterfall", 1)[1].split(
            "bool rf_ui_update_channel_metrics", 1
        )[0]
        push = RF_UI_C.split("static void push_waterfall_column", 1)[1].split(
            "static void push_waterfall_gap_column", 1
        )[0]

        self.assertIn("waterfall_pause_snapshot_capture", setter)
        self.assertIn("waterfall_render_rebuild_paused", setter)
        self.assertIn("waterfall_render_bootstrap_live", setter)
        self.assertIn("g_ui.waterfall_pan_columns = 0U", setter)
        self.assertIn("!g_ui.running", spectrum_present)
        self.assertIn("!g_ui.running", waterfall_present)
        self.assertIn("g_waterfall_write_head[channel]", push)
        self.assertIn("g_ui.running", push)
        self.assertNotIn("if(!g_ui.running) return", push)

    def test_replay_boxes_use_their_completed_window_history_anchor(self) -> None:
        note = RF_UI_C.split("bool rf_ui_note_complete_window", 1)[1].split(
            "bool rf_ui_present_spectrum", 1
        )[0]
        box_api = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "bool rf_ui_box_fusion_step", 1
        )[0]
        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]

        self.assertIn("rf_box_window_anchor_record", note)
        self.assertIn("g_waterfall_total_columns[channel_index]", note)
        self.assertIn("rf_box_window_anchor_find", box_api)
        self.assertIn("incoming.anchor_end_column = waterfall_end_column", box_api)
        self.assertNotIn(
            "incoming.anchor_end_column = g_waterfall_total_columns", box_api
        )
        self.assertLess(
            tile_update.index("rf_ui_note_complete_window"),
            tile_update.index("ui_flow_commit_rf_boxes_for_tile"),
        )

    def test_paused_touch_drag_is_bounded_to_the_retained_history(self) -> None:
        pan = RF_UI_C.split("static void waterfall_pan_event", 1)[1].split(
            "static void refresh_source_badge", 1
        )[0]
        create = RF_UI_C.split("static void create_waterfall_panel", 1)[1].split(
            "void rf_ui_create", 1
        )[0]
        visible = integer_define(RF_UI_H, "RF_UI_WATERFALL_COLS")
        retained = integer_define(RF_UI_H, "RF_UI_WATERFALL_HISTORY_COLS")

        self.assertEqual(retained - visible, 96)
        self.assertIn("LV_EVENT_PRESSING", pan)
        self.assertIn("g_ui.running", pan)
        self.assertIn(
            "RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS", pan
        )
        self.assertIn("waterfall_paused_view_present();", pan)
        self.assertIn("LV_EVENT_RELEASED", pan)
        self.assertIn("LV_EVENT_PRESS_LOST", pan)
        self.assertEqual(
            integer_define(RF_UI_C, "RF_WATERFALL_PAN_PRESENT_PERIOD_MS"), 33
        )
        self.assertIn("LV_OBJ_FLAG_CLICKABLE", create)
        self.assertIn("waterfall_pan_event", create)

    def test_b2_history_buttons_share_pause_and_review_state(self) -> None:
        pan_by = RF_UI_C.split("static void waterfall_pan_by", 1)[1].split(
            "static void history_button_event", 1
        )[0]
        event = RF_UI_C.split("static void history_button_event", 1)[1].split(
            "static void live_button_event", 1
        )[0]
        timing = RF_UI_C.split("static void refresh_waterfall_timing", 1)[1].split(
            "static void refresh_live_state", 1
        )[0]

        self.assertEqual(
            integer_define(RF_UI_C, "RF_WATERFALL_HISTORY_STEP_COLS"), 24
        )
        self.assertIn("rf_ui_set_running(false)", pan_by)
        self.assertIn("waterfall_paused_view_present();", pan_by)
        self.assertIn("RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS", pan_by)
        self.assertIn("rf_ui_toggle_running();", event)
        self.assertIn("RF_HISTORY_OLDER", event)
        self.assertIn("RF_HISTORY_NEWER", event)
        self.assertIn('g_ui.running ? "实时" :', timing)
        self.assertIn('"暂停" : "回放"', timing)

    def test_channel_switch_keeps_committed_view_until_atomic_commit(self) -> None:
        setter = RF_UI_C.split("bool rf_ui_set_selected_channel", 1)[1].split(
            "uint32_t rf_ui_get_selected_channel", 1
        )[0]
        commit = RF_UI_C.split("static bool channel_switch_commit", 1)[1].split(
            "static bool channel_switch_prepare_catchup", 1
        )[0]
        begin = RF_UI_C.rsplit("static bool render_transaction_begin", 1)[1].split(
            "static void live_render_commit", 1
        )[0]

        self.assertIn("g_ui.pending_channel = (uint8_t)channel_index", setter)
        self.assertNotIn("g_ui.committed_channel =", setter)
        self.assertNotIn("refresh_selected_view", setter)
        self.assertIn("g_ui.committed_channel = (uint8_t)channel", commit)
        self.assertIn("waterfall_image_source_rebind(source)", begin)
        self.assertIn("spectrum_image_source_rebind(source)", begin)
        self.assertIn("waterfall_image_head_update();", commit)
        self.assertIn("refresh_selected_view();", commit)
        self.assertLess(
            commit.index("g_ui.committed_channel ="),
            commit.index("refresh_selected_view();"),
        )

    def test_switch_gate_accepts_a_cached_matching_complete_window(self) -> None:
        ready = RF_UI_C.split("static bool channel_switch_window_ready", 1)[1].split(
            "static bool channel_switch_build_start", 1
        )[0]
        capture = RF_UI_C.split(
            "static void complete_spectrum_snapshot_try_capture", 1
        )[1].split("static bool waterfall_history_snapshot", 1)[0]
        setter = RF_UI_C.split("bool rf_ui_set_selected_channel", 1)[1].split(
            "uint32_t rf_ui_get_selected_channel", 1
        )[0]
        tile_update = LVGL_APP_C.split("void lvgl_app_tile_update", 1)[1].split(
            "void lvgl_app_telemetry_update", 1
        )[0]
        signal_update = LVGL_APP_C.split("void lvgl_app_signal_update", 1)[1].split(
            "void lvgl_app_signal_reset", 1
        )[0]

        self.assertIn("required_spectrum_revision", setter)
        self.assertIn("required_window_revision", setter)
        self.assertIn("complete_spectrum_snapshot_ready(channel)", ready)
        self.assertIn("spectrum->session_id != window->session_id", capture)
        self.assertIn("spectrum->window_sequence != window->window_sequence", capture)
        self.assertIn("g_complete_spectrum_data[channel]", capture)
        self.assertIn("complete_spectrum_snapshot_ready(channel_index)", setter)
        self.assertIn("g_complete_windows[channel_index].spectrum_revision", setter)
        self.assertIn("RA8P1_DISPLAY_TILE_HEIGHT - 1U", tile_update)
        self.assertIn("rf_ui_note_complete_window", tile_update)
        self.assertIn("rf_ui_update_spectrum_window", signal_update)
        switch_code = RF_UI_C.split("static bool channel_switch_window_ready", 1)[1]
        self.assertNotIn("SoftwareDelay", switch_code)
        self.assertNotIn("1000U", switch_code)

    def test_switch_build_is_bounded_and_last_request_wins(self) -> None:
        self.assertEqual(
            integer_define(
                RF_UI_C,
                "RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK",
            ),
            10,
        )
        self.assertEqual(
            integer_define(
                RF_UI_C,
                "RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK",
            ),
            20,
        )
        self.assertEqual(
            integer_define(
                RF_UI_C, "RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK"
            ),
            32,
        )
        self.assertEqual(
            integer_define(
                RF_UI_C,
                "RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK",
            ),
            20,
        )
        self.assertIn("RF_CHANNEL_SWITCH_MAX_WRITE_BYTES", RF_UI_C)
        self.assertIn("RF_WATERFALL_RENDER_STRIDE_BYTES", RF_UI_C)
        self.assertIn("RF_WATERFALL_VISIBLE_ROW_BYTES", RF_UI_C)
        step = RF_UI_C.split("bool rf_ui_channel_switch_step", 1)[1].split(
            "void rf_ui_create", 1
        )[0]
        setter = RF_UI_C.split("bool rf_ui_set_selected_channel", 1)[1].split(
            "uint32_t rf_ui_get_selected_channel", 1
        )[0]
        self.assertIn("rows_written <", step)
        self.assertIn(
            "RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK", step
        )
        self.assertIn("waterfall_catchup_rows_per_step", step)
        self.assertIn("request_generation", step)
        self.assertIn("request_generation", setter)
        self.assertIn("cancellations++", setter)
        self.assertIn("g_ui.pending_channel = (uint8_t)channel_index", setter)
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        self.assertEqual(
            integer_define(LVGL_APP_C, "UI_SDRAM_WORK_BUDGET_US"), 8000
        )
        self.assertEqual(
            integer_define(LVGL_APP_C, "UI_SDRAM_WORK_GUARD_US"), 1500
        )
        self.assertEqual(
            integer_define(
                LVGL_APP_C, "UI_CHANNEL_SWITCH_MAX_STEPS_PER_VSYNC"
            ),
            12,
        )
        self.assertIn("while (switch_steps <", owner)
        self.assertIn("cycle_budget_available", owner)

    def test_channel_switch_reuses_a_coherent_hidden_waterfall_source(self) -> None:
        start = RF_UI_C.split(
            "static bool channel_switch_build_start(bool restart)", 1
        )[1].split("static void channel_switch_restart", 1)[0]
        prepare = RF_UI_C.rsplit(
            "static bool channel_switch_prepare_catchup(void)\n{", 1
        )[1].split("bool rf_ui_channel_switch_step(void)", 1)[0]
        peak = RF_UI_C.split(
            "if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_SPECTRUM_PEAK)",
            1,
        )[1].split("target = &g_waterfall_render_rings", 1)[0]

        self.assertIn("const rf_ui_waterfall_source_state_t cached_state", start)
        self.assertIn("cached_state.valid && cached_state.channel == channel", start)
        self.assertIn("RF_UI_WATERFALL_HISTORY_COLS", start)
        self.assertIn("waterfall_history_head_matches", start)
        self.assertLess(
            start.index("cached_state ="),
            start.index("waterfall_source_state_invalidate"),
        )
        self.assertIn("g_channel_build.waterfall_cache_reused", start)
        self.assertIn("switch_cache_hits++", start)
        self.assertIn("switch_cache_misses++", start)
        self.assertIn("switch_cache_stale_misses++", start)
        self.assertIn("delta > RF_UI_WATERFALL_HISTORY_COLS", prepare)
        self.assertIn("switch_cache_catchup_columns", prepare)
        self.assertIn("g_channel_build.waterfall_cache_reused", peak)
        self.assertIn("return channel_switch_prepare_catchup();", peak)

    def test_active_channel_switch_preempts_box_fusion_work(self) -> None:
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1].split(
            "void lvgl_app_runtime_metrics_get", 1
        )[0]
        fusion_prefix = owner.split("rf_ui_box_fusion_step();", 1)[0]
        switch_suffix = owner.split("bool channel_switch_committed", 1)[1]

        self.assertIn("!rf_ui_channel_switch_busy()", fusion_prefix)
        self.assertIn("if (have_work_slot && !work_slot_used)", switch_suffix)

    def test_channel_switch_catchup_commits_one_frozen_pass(self) -> None:
        prepare = RF_UI_C.rsplit(
            "static bool channel_switch_prepare_catchup(void)\n{", 1
        )[1].split("bool rf_ui_channel_switch_step(void)", 1)[0]
        step = RF_UI_C.split(
            "bool rf_ui_channel_switch_step(void)\n{", 1
        )[1].split("void rf_ui_create(void)", 1)[0]
        catchup = step.split(
            "if(g_channel_build.state == "
            "RF_UI_CHANNEL_SWITCH_WATERFALL_CATCHUP)", 1
        )[1]
        completed = catchup.rsplit(
            "g_channel_build.caught_up_total_columns =", 1
        )[1]

        self.assertIn(
            "g_channel_build.catchup_target_total_columns = current_total",
            prepare,
        )
        self.assertIn("g_channel_build.catchup_target_head = current_head", prepare)
        # The cached path enters catch-up after the small spectrum build;
        # the cold path enters it after the full waterfall base build.
        self.assertEqual(step.count("channel_switch_prepare_catchup()"), 2)
        self.assertNotIn(
            "g_channel_build.catchup_target_total_columns =", catchup
        )
        self.assertNotIn("g_channel_build.catchup_target_head =", catchup)
        self.assertIn("(void)channel_switch_prepare_render();", completed)
        self.assertNotIn("channel_switch_prepare_catchup()", completed)
        self.assertNotIn("return channel_switch_prepare_render()", completed)
        self.assertLess(
            completed.index("(void)channel_switch_prepare_render();"),
            completed.index("return false;"),
        )
        self.assertIn("switch_catchup_overwrite_restarts++", catchup)
        self.assertEqual(
            integer_define(
                RF_UI_C, "RF_WATERFALL_OVERLAY_CATCHUP_MAX_ROWS_PER_TICK"
            ),
            64,
        )

    def test_live_build_catchup_finishes_one_frozen_pass(self) -> None:
        step = RF_UI_C.rsplit(
            "static bool live_build_step(void)\n{", 1
        )[1].split("static bool channel_switch_prepare_render", 1)[0]
        catchup = step.split(
            "if(g_live_build.state == RF_UI_LIVE_BUILD_CATCHUP)", 1
        )[1]
        completed = catchup.rsplit(
            "g_live_build.caught_up_total_columns =", 1
        )[1]

        self.assertEqual(step.count("live_build_prepare_catchup()"), 1)
        self.assertNotIn("g_live_build.catchup_target_total_columns =", catchup)
        self.assertNotIn("g_live_build.catchup_target_head =", catchup)
        self.assertIn("g_live_build.state = RF_UI_LIVE_BUILD_READY;", completed)
        self.assertNotIn("live_build_prepare_catchup()", completed)
        self.assertIn("live_catchup_overwrite_cancellations", catchup)

    def test_pending_channel_feedback_updates_only_changed_cards(self) -> None:
        card = RF_UI_C.split(
            "static void refresh_selector_style(uint32_t index)\n{", 1
        )[1].split("static void refresh_selector_styles(void)", 1)[0]
        setter = RF_UI_C.split(
            "bool rf_ui_set_selected_channel", 1
        )[1].split("static void channel_switch_soak_step", 1)[0]

        self.assertNotIn("for(", re.sub(r"\s+", "", card))
        self.assertIn("index == g_ui.committed_channel", card)
        self.assertIn("index == g_ui.pending_channel", card)
        self.assertIn("RF_COLOR_AMBER_SOFT", card)
        self.assertIn("previous_pending_channel", setter)
        self.assertIn("refresh_selector_style(previous_pending_channel);", setter)
        self.assertIn("refresh_selector_style(channel_index);", setter)
        self.assertNotIn("refresh_selector_styles();", setter)
        self.assertNotIn("refresh_selected_view", setter)

    def test_same_round_fusion_gates_spectrum_and_history_fail_closed(self) -> None:
        online = RF_UI_C.split("static bool detection_online", 1)[1].split(
            "static const char * detection_state_text", 1
        )[0]
        state_text = RF_UI_C.split(
            "static const char * detection_state_text", 1
        )[1].split("static uint32_t detection_state_color", 1)[0]
        raster = RF_UI_C.split(
            "static void waterfall_overlay_box_raster", 1
        )[1].split("static void waterfall_overlay_boxes_refresh", 1)[0]
        refresh = RF_UI_C.split(
            "static void waterfall_overlay_boxes_refresh", 1
        )[1].split("static void waterfall_image_head_set", 1)[0]
        update = RF_UI_C.split("bool rf_ui_update_detection", 1)[1].split(
            "static uint32_t rf_box_observation_next", 1
        )[0]
        box_api = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "void rf_ui_reset_rf_box_fusion", 1
        )[0]
        resolver = RF_UI_C.split("static void rf_box_batch_resolve", 1)[1].split(
            "static rf_ui_fusion_decision_cache_t", 1
        )[0]
        round_apply = RF_UI_C.split("void rf_ui_apply_fusion_round", 1)[1].split(
            "void rf_ui_mark_channel_result", 1
        )[0]
        signal_update = LVGL_APP_C.split(
            "void lvgl_app_signal_update", 1
        )[1].split("void lvgl_app_telemetry_update", 1)[0]

        self.assertIn("state == RF_UI_DETECTION_ACTIVE", online)
        self.assertNotIn("!= RF_UI_DETECTION_INACTIVE", online)
        self.assertNotIn('"跟踪中"', state_text)
        self.assertIn('return "空闲"', state_text)
        self.assertNotIn("rf_box_is_working", raster)
        self.assertNotIn("restore", raster)
        self.assertIn("RF boxes live in the RGB565 history itself", refresh)
        self.assertNotIn("stamped_boxes", refresh)
        self.assertNotIn("g_ui.rf_boxes_dirty", update)
        self.assertIn("session_id", box_api)
        self.assertIn("window_sequence", box_api)
        self.assertNotIn("detection_online", box_api)
        self.assertNotIn("waterfall_history_box_raster", box_api)
        self.assertIn("activity == RF_UI_FUSION_WORKING", resolver)
        self.assertIn("history_boxes_dropped_idle", resolver)
        self.assertIn("history_boxes_dropped_uncertain", resolver)
        self.assertIn("waterfall_history_box_raster", resolver)
        self.assertIn("waterfall_overlay_box_raster_matching_sources", resolver)
        self.assertIn("RF_UI_FUSION_ROUND_OUTPUT_VALID", round_apply)
        self.assertIn("rf_box_pending_drop_stale", round_apply)
        self.assertLess(
            signal_update.index("ui_flow_update_detections();"),
            signal_update.index("ui_flow_prepare_rf_boxes(frame);")
        )

    def test_fusion_box_commits_use_one_existing_vsync_work_slot(self) -> None:
        step = RF_UI_C.split("bool rf_ui_box_fusion_step", 1)[1].split(
            "void rf_ui_reset_rf_box_fusion", 1
        )[0]
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1].split(
            "void lvgl_app_runtime_metrics_get", 1
        )[0]

        self.assertIn("rf_box_batch_resolve(decision, batch);", step)
        self.assertIn("return true;", step)
        self.assertIn("have_work_slot && !work_slot_used", owner)
        self.assertIn("work_slot_used = rf_ui_box_fusion_step();", owner)
        self.assertLess(
            owner.index("lvgl_touch_poll_step();"),
            owner.index("rf_ui_box_fusion_step();"),
        )

    def test_rf_boxes_keep_the_last_real_range_across_empty_windows(self) -> None:
        detail_update = RF_UI_C.split("static void rf_box_detail_update", 1)[1].split(
            "static void rf_box_batch_resolve", 1
        )[0]
        resolver = RF_UI_C.split("static void rf_box_batch_resolve", 1)[1].split(
            "static rf_ui_fusion_decision_cache_t", 1
        )[0]
        finder = RF_UI_C.split("static bool find_last_detection_box", 1)[
            1
        ].split("static void format_box_frequency_range", 1)[0]
        cards = RF_UI_C.split("static void refresh_target_cards", 1)[1].split(
            "static void refresh_compare_overlay", 1
        )[0]
        detail = RF_UI_C.split("static void refresh_target_detail(void)", 1)[
            1
        ].split("static void refresh_selected_view", 1)[0]

        self.assertIn("rf_ui_rf_box_batch_t * const detail", detail_update)
        self.assertIn("detail->boxes[destination] = *box", detail_update)
        self.assertIn("detail->anchor_end_columns[destination]", detail_update)
        self.assertIn("g_last_detail_round_index", detail_update)
        self.assertIn("rf_box_detail_update", resolver)
        self.assertIn("activity == RF_UI_FUSION_WORKING", resolver)
        self.assertIn("observation_generation", finder)
        self.assertIn("rf_box_generation_newer", finder)
        self.assertIn("find_last_detection_box(index, &ref)", cards)
        self.assertIn("find_last_detection_box(target, &last_ref)", detail)
        self.assertIn("index == 0U && range_present", detail)
        self.assertNotIn("refs[2]", detail)
        self.assertNotIn("range_count", detail)

    def test_target_detail_surface_keeps_fixed_dark_colors(self) -> None:
        surface = RF_UI_C.split(
            "static void refresh_target_detail_surface", 1
        )[1].split("static void refresh_target_detail(void)", 1)[0]
        create = RF_UI_C.split("static void create_sidebar(void)", 1)[1].split(
            "static void create_spectrum_panel", 1
        )[0]

        self.assertNotIn("blend_rgb", surface)
        self.assertNotIn("g_target_accent_colors", surface)
        for expression in (
            "color(RF_COLOR_PANEL)",
            "color(RF_COLOR_HEADER)",
            "color(RF_COLOR_PANEL_ALT)",
            "color(RF_COLOR_DIVIDER)",
            "color(RF_COLOR_BORDER)",
        ):
            self.assertIn(expression, surface)
        for panel in (
            "detail_title_panel",
            "detail_ranges_panel",
            "detail_metric_panels",
            "detail_confidence_panel",
            "detail_preview_panel",
            "detail_image_frame",
        ):
            self.assertIn(f"g_ui.{panel}", surface)
            self.assertIn(f"g_ui.{panel}", create)

    def test_rf_box_colors_are_distinct_for_all_four_classes(self) -> None:
        spectrum_style = RF_UI_C.split("static void style_rf_box_overlay", 1)[
            1
        ].split("static void hide_rf_box_overlays", 1)[0]
        waterfall_raster = RF_UI_C.split(
            "static void waterfall_overlay_box_raster", 1
        )[1].split("static void waterfall_overlay_boxes_refresh", 1)[0]

        colors = ("42A5F5", "B8BEC4", "FF7A59", "5DD39E")
        for index, value in enumerate(colors, 1):
            self.assertIn(f"RF_COLOR_TARGET_{index} 0x{value}u", RF_UI_C)
        self.assertIn("g_target_accent_colors[box->detection_index]", spectrum_style)
        self.assertIn(
            "RF_WATERFALL_CLUT_BOX_FIRST + box->detection_index",
            waterfall_raster,
        )

    def test_staged_image_sources_rebind_without_full_invalidation(self) -> None:
        rebind = LV_IMAGE_C.split("lv_result_t lv_image_rebind_src", 1)[1].split(
            "void lv_image_set_offset_x", 1
        )[0]
        transaction = RF_UI_C.rsplit("static bool render_transaction_begin", 1)[
            1
        ].split("static void live_render_commit", 1)[0]

        self.assertIn("lv_image_dsc_t g_waterfall_image_dsc[", RF_UI_C)
        self.assertIn("lv_image_dsc_t g_spectrum_image_dsc[", RF_UI_C)
        self.assertIn("lv_image_rebind_src", LV_IMAGE_H)
        self.assertIn("header.w != (uint32_t)img->w", rebind)
        self.assertIn("header.h != (uint32_t)img->h", rebind)
        self.assertNotIn("lv_obj_invalidate", rebind)
        self.assertIn("waterfall_image_source_rebind(source)", transaction)
        self.assertIn("spectrum_image_source_rebind(source)", transaction)
        self.assertIn("waterfall_invalidations++", RF_UI_C)
        self.assertIn("waterfall_invalidated_rows += row_count", RF_UI_C)

    def test_switch_spectrum_is_drawn_in_deferred_row_chunks(self) -> None:
        render = RF_UI_C.rsplit("static bool render_transaction_step(void)\n{", 1)[
            1
        ].split("static void live_build_cancel", 1)[0]
        commit = RF_UI_C.split("static bool channel_switch_commit(void)", 1)[
            1
        ].split("static void render_transaction_reset", 1)[0]

        self.assertIn("g_render_txn.spectrum_render_row", render)
        self.assertIn(
            "RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK", render
        )
        self.assertIn("invalidate_image_area_rows(g_ui.spectrum_image", render)
        self.assertLess(
            render.index("lv_display_deferred_commit(display)"),
            render.index("invalidate_image_area_rows(g_ui.spectrum_image"),
        )
        self.assertNotIn("lv_obj_invalidate(g_ui.spectrum_image)", commit)

    def test_spectrum_build_chunks_fit_sdram_budget(self) -> None:
        budget = 32 * 1024
        texture_width = integer_define(RF_UI_C, "RF_SPECTRUM_TEXTURE_WIDTH")
        texture_height = integer_define(RF_UI_C, "RF_SPECTRUM_TEXTURE_HEIGHT")
        stride_pixels = integer_define(
            RF_UI_C, "RF_SPECTRUM_TEXTURE_STRIDE_PIXELS"
        )
        base_rows = integer_define(
            RF_UI_C, "RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK"
        )
        trace_segments = integer_define(
            RF_UI_C, "RF_CHANNEL_SWITCH_SPECTRUM_SEGMENTS_PER_TICK"
        )

        self.assertEqual(base_rows, 32)
        self.assertEqual(trace_segments, 112)
        divider_rows = {
            ((texture_height - 1) * index) // 4 for index in range(5)
        }
        base_chunks = []
        for first_row in range(0, texture_height, base_rows):
            rows = min(base_rows, texture_height - first_row)
            horizontal_dividers = sum(
                first_row <= row < first_row + rows for row in divider_rows
            )
            base_chunks.append(
                rows * (stride_pixels * 2 + 9 * 2)
                + horizontal_dividers * texture_width * 2
            )
        self.assertEqual(max(base_chunks), 30_400)
        self.assertLessEqual(max(base_chunks), budget)

        def trunc_div(numerator: int, denominator: int) -> int:
            quotient = abs(numerator) // denominator
            return -quotient if numerator < 0 else quotient

        max_segment_bytes = 0
        for span in (1, 2):
            for y0 in range(texture_height):
                for y1 in range(texture_height):
                    stores = 0
                    for offset in range(span + 1):
                        y = y0 + trunc_div((y1 - y0) * offset, span)
                        stores += texture_height - y

                    x, y = 0, y0
                    dx = span
                    dy = -abs(y1 - y0)
                    sy = 1 if y0 < y1 else -1
                    error = dx + dy
                    while True:
                        stores += int(0 <= y < texture_height)
                        stores += int(0 <= y + 1 < texture_height)
                        if x == span and y == y1:
                            break
                        twice_error = error * 2
                        if twice_error >= dy:
                            error += dy
                            x += 1
                        if twice_error <= dx:
                            error += dx
                            y += sy
                    max_segment_bytes = max(max_segment_bytes, stores * 2)

        self.assertEqual(max_segment_bytes, 282)
        self.assertLessEqual(trace_segments * max_segment_bytes, budget)
        self.assertIn("&trace_bytes", RF_UI_C)
        self.assertIn("channel_switch_record_chunk(trace_bytes, 0U)", RF_UI_C)
        self.assertNotIn("channel_switch_record_chunk(16U * 1024U", RF_UI_C)
        self.assertNotIn("channel_switch_record_chunk(64U, 0U)", RF_UI_C)

    def test_waterfall_build_and_render_chunks_fit_sdram_budget(self) -> None:
        budget = 32 * 1024
        display_width = integer_define(RF_UI_C, "RF_WATERFALL_DISPLAY_WIDTH")
        display_height = integer_define(RF_UI_C, "RF_WATERFALL_DISPLAY_HEIGHT")
        visible_columns = integer_define(RF_UI_H, "RF_UI_WATERFALL_COLS")
        history_columns = integer_define(
            RF_UI_H, "RF_UI_WATERFALL_HISTORY_COLS"
        )
        pixels_per_column = display_width // visible_columns
        rgb_base_rows = integer_define(
            RF_UI_C, "RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK"
        )
        clut_base_rows = integer_define(
            RF_UI_C, "RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK"
        )
        render_rows = integer_define(
            RF_UI_C, "RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK"
        )
        catchup_cap = integer_define(
            RF_UI_C, "RF_WATERFALL_OVERLAY_CATCHUP_MAX_ROWS_PER_TICK"
        )

        rgb_base_row_bytes = display_width * 2 * 2
        clut_column_bytes = ((pixels_per_column + 1) // 2) * 2 * 2
        clut_base_row_bytes = (
            history_columns * ((pixels_per_column + 1) // 2)
            + history_columns * pixels_per_column // 2
        ) * 2
        self.assertEqual((rgb_base_rows, clut_base_rows, render_rows), (10, 11, 20))
        self.assertEqual(rgb_base_rows * rgb_base_row_bytes, 32_000)
        self.assertEqual(clut_base_rows * clut_base_row_bytes, 30_976)
        self.assertEqual(render_rows * display_width * 2, 32_000)

        self.assertEqual(catchup_cap, 64)
        for columns in range(1, history_columns + 1):
            row_bytes = columns * clut_column_bytes
            rows = min(catchup_cap, display_height, budget // row_bytes)
            self.assertGreater(rows, 0)
            self.assertLessEqual(rows * row_bytes, budget)
        for columns in range(1, visible_columns + 1):
            row_bytes = columns * pixels_per_column * 2 * 2
            rows = min(display_height, budget // row_bytes)
            self.assertGreater(rows, 0)
            self.assertLessEqual(rows * row_bytes, budget)

        self.assertIn("one CLUT4 catch-up row exceeds", RF_UI_C)
        self.assertIn("one RGB565 catch-up row exceeds", RF_UI_C)

    def test_switch_metadata_is_staged_before_the_commit_tick(self) -> None:
        render = RF_UI_C.rsplit("static bool render_transaction_step(void)\n{", 1)[
            1
        ].split("static void live_build_cancel", 1)[0]
        stage = RF_UI_C.rsplit(
            "static bool channel_switch_stage_metadata_step(void)", 1
        )[1].split("static bool channel_switch_commit(void)", 1)[0]
        commit = RF_UI_C.rsplit("static bool channel_switch_commit(void)", 1)[
            1
        ].split("static void render_transaction_reset", 1)[0]
        defer = RF_UI_C.rsplit(
            "static bool channel_switch_defer_metadata_refresh(void)", 1
        )[1].split("static bool channel_switch_stage_metadata_step", 1)[0]
        poll = RF_UI_C.rsplit(
            "static void render_transaction_poll_complete(void)", 1
        )[1].split("static bool render_transaction_begin", 1)[0]

        self.assertEqual(
            integer_define(RF_UI_C, "RF_CHANNEL_SWITCH_METADATA_STAGE_COUNT"),
            4,
        )
        self.assertIn("channel_switch_stage_metadata_step()", render)
        self.assertLess(
            render.index("channel_switch_stage_metadata_step()"),
            render.rindex("lv_display_deferred_commit(display)"),
        )
        self.assertIn("switch(g_render_txn.metadata_stage)", stage)
        self.assertIn("saved_committed_channel", stage)
        self.assertIn("saved_pending_channel", stage)
        self.assertNotIn("refresh_selected_view();", commit)
        self.assertNotIn("refresh_compare_overlay();", commit)
        self.assertIn("switch_metadata_stage_steps++", stage)
        self.assertIn("channel_switch_defer_metadata_refresh()", RF_UI_C)
        self.assertIn("g_render_txn.metadata_refresh_pending = true", defer)
        self.assertNotIn("g_render_txn.metadata_stage = 0U", defer)
        self.assertNotIn("switch_metadata_stage_restarts++", stage)
        self.assertNotIn(
            "channel_switch_target_detection_index(g_render_txn.channel)",
            render,
        )
        self.assertIn("refresh_selected_view();", poll)
        self.assertIn("refresh_compare_overlay();", poll)
        self.assertIn("switch_metadata_post_commit_refreshes++", poll)

    def test_new_ui_optional_channel_label_is_guarded(self) -> None:
        stage = RF_UI_C.rsplit(
            "static bool channel_switch_stage_metadata_step(void)", 1
        )[1].split("static bool channel_switch_commit(void)", 1)[0]
        selected_update = stage.split("case 0U:", 1)[1].split(
            "lv_label_set_text_fmt(g_ui.waterfall_channel_label", 1
        )[0]

        self.assertIn("if(g_ui.selected_channel_label != NULL)", selected_update)
        self.assertIn(
            "lv_label_set_text_fmt(g_ui.selected_channel_label",
            selected_update,
        )

    def test_visible_chinese_labels_use_the_simhei_mixed_font(self) -> None:
        compact = re.sub(r"\s+", " ", RF_UI_C)
        expected = (
            'timeline, 52, 3, 274, 16, "CH1 | 2420 MHz | 0 目标", '
            '&rf_font_zh_14',
            'panel, RF_PLOT_X + 8, 1, 100, 16, "功率频谱", '
            '&rf_font_zh_14',
            'panel, 520, 7, 336, 18, '
            '"160 RF ROW | 0.615 ms/COL | 98.39 ms", &rf_font_zh_14',
        )
        for declaration in expected:
            self.assertIn(declaration, compact)

    def test_switch_stream_updates_are_coalesced_after_atomic_commit(self) -> None:
        metrics = RF_UI_C.split("bool rf_ui_update_channel_metrics", 1)[1].split(
            "bool rf_ui_update_detection", 1
        )[0]
        boxes = RF_UI_C.split("bool rf_ui_update_rf_boxes", 1)[1].split(
            "void rf_ui_mark_channel_result", 1
        )[0]
        result = RF_UI_C.split("void rf_ui_mark_channel_result", 1)[1].split(
            "void rf_ui_force_channel_result_redraw", 1
        )[0]
        setter = RF_UI_C.split("bool rf_ui_set_selected_channel", 1)[1].split(
            "static void channel_switch_soak_step", 1
        )[0]
        target = RF_UI_C.split("static void select_target_index", 1)[1].split(
            "static void target_click_event", 1
        )[0]

        self.assertLess(
            metrics.index("previous.peak_dbfs == metrics->peak_dbfs"),
            metrics.index("channel_switch_defer_metadata_refresh()"),
        )
        self.assertIn("channel_switch_defer_metadata_refresh()", boxes)
        self.assertLess(
            result.index("channel_switch_defer_metadata_refresh()"),
            result.index("lv_obj_set_style_bg_color"),
        )
        self.assertLess(
            setter.index("render_transaction_poll_complete();"),
            setter.index("render_transaction_abort();"),
        )
        self.assertIn("channel_switch_defer_metadata_refresh();", target)

    def test_live_waterfall_reuses_the_complete_inactive_source(self) -> None:
        start = RF_UI_C.rsplit(
            "static bool live_build_start(uint32_t channel)\n{", 1
        )[1].split("static bool live_build_prepare_catchup", 1)[0]
        step = RF_UI_C.rsplit("static bool live_build_step(void)\n{", 1)[1].split(
            "static bool channel_switch_prepare_render", 1
        )[0]
        present = RF_UI_C.split("bool rf_ui_present_spectrum(void)", 1)[1].split(
            "bool rf_ui_update_waterfall", 1
        )[0]

        self.assertIn("source_state.valid", start)
        self.assertIn("source_state.channel == channel", start)
        self.assertIn("source_state.total_columns", start)
        self.assertIn("live_incremental_builds++", start)
        self.assertIn("live_base_rebuilds++", start)
        self.assertIn("waterfall_source_state_invalidate(source)", start)
        self.assertIn("waterfall_catchup_rows_per_step", step)
        self.assertIn("RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES", RF_UI_C)
        self.assertNotIn(
            "g_live_build.state != RF_UI_LIVE_BUILD_IDLE", present
        )

    def test_switch_commit_stays_on_existing_vsync_buffer_change_path(self) -> None:
        step = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        flush = LVGL_APP_C.split("static void lvgl_flush_callback", 1)[1].split(
            "static void lvgl_flush_wait_callback", 1
        )[0]
        callback = DISPLAY_BRINGUP_C.split("void glcdc_callback", 1)[1]

        self.assertIn("rf_ui_channel_switch_step()", step)
        self.assertIn("R_GLCDC_BufferChange", flush)
        self.assertIn("g_flush_pending = true", flush)
        self.assertIn("g_display_diag.glcdc_line_events++", callback)
        self.assertNotIn("lv_", callback)

    def test_underflow_diagnostics_attribute_owner_thread_work(self) -> None:
        callback = DISPLAY_BRINGUP_C.split("void glcdc_callback", 1)[1]
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1]

        self.assertIn("g_display_diag.underflow_last_context", callback)
        self.assertIn("g_display_diag.underflow_unattributed++", callback)
        self.assertIn("underflow_deferred_resync++", callback)
        self.assertIn("underflow_channel_switch++", callback)
        self.assertIn("underflow_spectrum_present++", callback)
        self.assertIn("underflow_waterfall_present++", callback)
        self.assertIn("underflow_lvgl_refresh++", callback)
        self.assertIn("underflow_flush_wait++", callback)
        self.assertIn("underflow_tile_drain++", callback)
        self.assertIn("underflow_deferred_draw++", callback)
        self.assertIn("underflow_deferred_commit++", callback)
        self.assertIn("underflow_normal_refresh++", callback)
        self.assertNotIn("lv_", callback)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_RESYNC", owner)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_CHANNEL_SWITCH", owner)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_SPECTRUM_PRESENT", owner)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT", owner)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_LVGL_REFRESH", LVGL_APP_C)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_DRAW", LVGL_APP_C)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_COMMIT", LVGL_APP_C)
        self.assertIn("DISPLAY_UNDERFLOW_CONTEXT_NORMAL_REFRESH", LVGL_APP_C)

    def test_detached_switch_soak_uses_the_normal_owner_thread_path(self) -> None:
        step = RF_UI_C.split("static void channel_switch_soak_step(void)\n{", 1)[1].split(
            "uint32_t rf_ui_get_selected_channel", 1
        )[0]
        owner = RF_UI_C.split("bool rf_ui_channel_switch_step", 1)[1].split(
            "void rf_ui_create", 1
        )[0]

        self.assertIn("channel_switch_soak_step();", owner)
        self.assertIn("rf_ui_set_selected_channel(channel)", step)
        self.assertIn("g_render_txn.active", step)
        self.assertIn("g_ui.pending_channel != g_ui.committed_channel", step)
        self.assertIn("g_channel_build.state != RF_UI_CHANNEL_SWITCH_IDLE", step)
        self.assertIn("g_rf_ui_channel_soak.completed_switches++", step)
        self.assertNotIn("SoftwareDelay", step)
        self.assertNotIn("lv_", step)

    def test_detached_live_monitor_owns_its_measurement_window(self) -> None:
        monitor = RF_UI_C.split("void rf_ui_runtime_monitor_step(void)", 1)[1].split(
            "uint32_t rf_ui_get_selected_channel", 1
        )[0]
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        spectrum = RF_UI_C.split("bool rf_ui_present_spectrum(void)", 1)[1].split(
            "bool rf_ui_update_waterfall", 1
        )[0]

        self.assertIn("RF_UI_RUNTIME_MONITOR_MAGIC", RF_UI_H)
        self.assertIn("start_underflows", RF_UI_H)
        self.assertIn("end_underflows", RF_UI_H)
        self.assertIn("g_display_diag.glcdc_line_events", monitor)
        self.assertIn("g_display_diag.glcdc_underflows", monitor)
        self.assertIn("live_atomic_commits", monitor)
        self.assertIn("spectrum_presents", monitor)
        self.assertNotIn("lv_", monitor)
        self.assertIn("rf_ui_runtime_monitor_step();", owner)
        self.assertIn("spectrum_presents++", spectrum)
        self.assertIn("Start-Sleep -Seconds ($Seconds + 4)", LIVE_MONITOR_PS1)
        self.assertIn("end_underflows", LIVE_MONITOR_PS1)
        self.assertIn("LiveCommitDelta", LIVE_MONITOR_PS1)
        self.assertIn("SpectrumPresentDelta", LIVE_MONITOR_PS1)

    def test_deferred_commit_resyncs_old_buffer_without_a_bandwidth_spike(self) -> None:
        refresh = LV_REFR_C.split("void lv_display_refr_timer", 1)[1].split(
            "static void lv_refr_join_area", 1
        )[0]
        sync = LV_REFR_C.rsplit("static uint32_t refr_sync_areas", 1)[1].split(
            "static void refr_invalid_areas", 1
        )[0]
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1]

        self.assertIn("disp_refr->deferred_resync", refresh)
        self.assertIn("Keep the accumulated transaction areas", refresh)
        self.assertNotIn("lv_ll_clear(&disp_refr->sync_areas)", refresh)
        self.assertIn("max_bytes", sync)
        self.assertIn("sync_area->y1 += (int32_t)rows", sync)
        self.assertIn("disp->deferred_resync = 1", LV_REFR_C)
        self.assertIn("if(disp->deferred_resync) return false", LV_DISPLAY_C)
        self.assertIn("UI_DEFERRED_RESYNC_MAX_BYTES (32U * 1024U)", LVGL_APP_C)
        self.assertIn("lvgl_refresh_skips_after_resync", LVGL_APP_C)
        self.assertIn("ui_deferred_resync_step()", owner)
        self.assertIn("lv_display_deferred_resync_step", LVGL_APP_C)
        self.assertIn("line_event != g_sdram_work_line_event", owner)

    def test_pre_draw_deferred_abort_does_not_force_full_frame_resync(self) -> None:
        abort = LV_DISPLAY_C.split("void lv_display_deferred_abort", 1)[1].split(
            "bool lv_display_deferred_is_active", 1
        )[0]

        self.assertIn("buffer_was_touched", abort)
        self.assertIn("if(!buffer_was_touched) return", abort)
        self.assertLess(
            abort.index("if(!buffer_was_touched) return"),
            abort.index("disp->deferred_resync = 1"),
        )

    def test_clut4_overlay_reuses_the_rgb565_source_allocation(self) -> None:
        self.assertEqual(integer_define(RF_UI_C, "RF_PLOT_X"), 64)
        self.assertIn(
            "RF_UI_WATERFALL_HISTORY_COLS * RF_WATERFALL_CLUT_PIXELS_PER_COLUMN",
            RF_UI_C,
        )
        self.assertIn("rf_ui_waterfall_render_storage_t", RF_UI_C)
        self.assertIn("rf_ui_waterfall_rgb565_ring_t rgb565", RF_UI_C)
        self.assertIn("rf_ui_waterfall_clut4_ring_t clut4", RF_UI_C)
        self.assertIn(
            "sizeof(rf_ui_waterfall_clut4_ring_t) <=\n"
            "               sizeof(rf_ui_waterfall_rgb565_ring_t)",
            RF_UI_C,
        )
        self.assertIn("RF_WATERFALL_CLUT_STRIDE_BYTES & 63u", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_ALIGNMENT_PIXELS == 128u", RF_UI_C)
        self.assertIn("[aligned_head >> 1]", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_PHASE_COUNT 2u", RF_UI_C)
        self.assertIn("RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK 11u", RF_UI_C)
        self.assertIn("RF_CHANNEL_SWITCH_MAX_WRITE_BYTES", RF_UI_C)
        self.assertIn(
            "&g_waterfall_render_rings[source].rgb565.rows[0][0]", RF_UI_C
        )

    def test_clut4_overlay_is_vsync_paced_outside_the_isr(self) -> None:
        owner = LVGL_APP_C.split("void lvgl_app_step", 1)[1]
        callback = DISPLAY_BRINGUP_C.split("void glcdc_callback", 1)[1]
        self.assertIn("GLCDC_INPUT_INTERFACE_FORMAT_CLUT4", LVGL_APP_C)
        self.assertNotIn("GLCDC_INPUT_INTERFACE_FORMAT_CLUT8", LVGL_APP_C)
        self.assertIn("R_GLCDC_ClutUpdate", LVGL_APP_C)
        self.assertIn("R_GLCDC_LayerChange", LVGL_APP_C)
        self.assertIn("UI_RF_OVERLAY_STABLE_VSYNCS", LVGL_APP_C)
        self.assertIn("UI_RF_OVERLAY_ENABLE_CLEAN_VSYNCS", LVGL_APP_C)
        self.assertIn("overlay_startup_underflows_tolerated", LVGL_APP_C)
        self.assertIn("overlay_enable_clean_vsyncs", DISPLAY_BRINGUP_H)
        self.assertIn("UI_RF_OVERLAY_MONITOR_VSYNCS   (2812U)", LVGL_APP_C)
        self.assertIn("overlay_monitor_windows", DISPLAY_BRINGUP_H)
        self.assertIn("ui_rf_waterfall_overlay_monitor_step", LVGL_APP_C)
        self.assertIn("ui_rf_waterfall_overlay_step", owner)
        self.assertIn("line_advanced", LVGL_APP_C)
        self.assertEqual(
            integer_define(
                RF_UI_C, "RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC"
            ),
            16,
        )
        self.assertIn("waterfall_overlay_paced_pixels", RF_UI_C)
        self.assertIn("g_scan_rate_x10", RF_UI_C)
        self.assertIn("g_ui.focus_mode ? 1U", RF_UI_C)
        self.assertNotIn("R_GLCDC_LayerChange", callback)
        self.assertNotIn("lv_", callback)

    def test_clut4_pacer_spreads_a_scan_window_across_vsyncs(self) -> None:
        rate_x10 = integer_define(
            RF_UI_C, "RF_WATERFALL_OVERLAY_DEFAULT_RATE_X10"
        )
        max_pixels = integer_define(
            RF_UI_C, "RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC"
        )
        pixels_per_window = 16 * 5
        denominator = 10000 * 4
        accumulator = 0
        backlog = pixels_per_window
        steps: list[int] = []
        for _ in range(31):
            accumulator += rate_x10 * pixels_per_window * 20
            paced_pixels = (accumulator // denominator) & ~1
            pixels = min(max_pixels, paced_pixels, backlog) & ~1
            accumulator -= min(paced_pixels, pixels) * denominator
            backlog -= pixels
            steps.append(pixels)

        self.assertEqual(rate_x10, 65)
        self.assertEqual(backlog, 0)
        self.assertLessEqual(max(steps), 4)
        self.assertGreaterEqual(sum(pixel != 0 for pixel in steps), 25)
        self.assertTrue(all((pixel & 1) == 0 for pixel in steps))

    def test_clut4_pixels_are_packed_without_neighbor_damage(self) -> None:
        helper = RF_UI_C.split("static void waterfall_clut4_pixel_set", 1)[
            1
        ].split("static void prepare_waterfall_lookup_tables", 1)[0]
        prepare = RF_UI_C.split("bool rf_ui_waterfall_overlay_prepare_frame", 1)[
            1
        ].split("void rf_ui_waterfall_overlay_frame_submitted", 1)[0]

        self.assertIn("pixel_x >> 1", helper)
        self.assertIn("old_value & 0xF0U", helper)
        self.assertIn("old_value & 0x0FU", helper)
        self.assertIn("target_end_pixels &= ~UINT64_C(1)", prepare)
        self.assertIn("frame->hsize & 1U", LVGL_APP_C)

    def test_clut4_alignment_prefix_uses_hardware_clip_without_source_writes(
        self,
    ) -> None:
        prepare = RF_UI_C.split("bool rf_ui_waterfall_overlay_prepare_frame", 1)[
            1
        ].split("void rf_ui_waterfall_overlay_frame_submitted", 1)[0]
        submit = LVGL_APP_C.split(
            "static fsp_err_t ui_rf_waterfall_overlay_layer_submit", 1
        )[1].split("static void ui_rf_waterfall_overlay_monitor_start", 1)[0]

        self.assertIn(".transparent_prefix = prefix_pixels", prepare)
        self.assertNotIn("waterfall_overlay_guard_", RF_UI_C)
        self.assertNotIn("memset(", prepare)
        self.assertNotIn("memcpy(", prepare)
        for register in ("AB4", "AB5", "AB6", "AB7"):
            self.assertIn(register, submit)
        self.assertIn("UI_GLCDC_RECT_ALPHA_ENABLE", submit)
        self.assertIn("overlay_guard_clip_submits++", submit)
        self.assertNotIn("R_GLCDC_LayerChange", submit)

        plot_x = integer_define(RF_UI_C, "RF_PLOT_X")
        for prefix in range(0, 64, 2):
            graphics_x = plot_x - prefix
            self.assertGreaterEqual(graphics_x, 0)
            self.assertEqual(graphics_x + prefix, plot_x)

    def test_clut4_layer2_submit_latches_clip_and_source_atomically(self) -> None:
        submit = LVGL_APP_C.split(
            "static fsp_err_t ui_rf_waterfall_overlay_layer_submit", 1
        )[1].split("static void ui_rf_waterfall_overlay_monitor_start", 1)[0]
        pven_write = "VEN_b.PVEN = 1U"

        self.assertIn("VEN_b.PVEN != 0U", submit)
        self.assertIn("BG.EN_b.VEN != 0U", submit)
        self.assertLess(submit.index("FLM2 ="), submit.index("AB5 ="))
        self.assertLess(submit.index("AB5 ="), submit.index("AB1 ="))
        self.assertLess(submit.index("AB1 ="), submit.rindex(pven_write))
        self.assertLess(submit.index("__DSB()"), submit.rindex(pven_write))

    def test_clut4_latch_confirmation_waits_for_layer2_pven(self) -> None:
        present = LVGL_APP_C.split(
            "static void ui_rf_waterfall_overlay_step", 1
        )[1].split("static bool ui_waterfall_overlay_present", 1)[0]
        pven = "R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].VEN_b.PVEN"
        self.assertIn(pven, present)
        self.assertIn("overlay_line_ready && !overlay_update_pending", present)
        self.assertIn("overlay_latch_pven_deferrals++", present)
        self.assertLess(present.index(pven), present.index(
            "rf_ui_waterfall_overlay_frame_latched"))

    def test_clut4_ingestion_is_published_after_bounded_background_sync(self) -> None:
        push = RF_UI_C.split("static void push_waterfall_column", 1)[1].split(
            "static void push_waterfall_gap_column", 1
        )[0]
        sync = RF_UI_C.split("static bool waterfall_overlay_sync_step(void)\n{", 1)[
            1
        ].split("static uint32_t waterfall_render_catchup_row", 1)[0]
        present = RF_UI_C.split("bool rf_ui_present_waterfall(void)", 1)[1].split(
            "bool rf_ui_update_channel_metrics", 1
        )[0]

        self.assertNotIn("waterfall_overlay_history_pixel_write", push)
        self.assertIn("waterfall_overlay_catchup_rows_per_step", sync)
        self.assertIn("overlay_sync_last_chunk_bytes", sync)
        self.assertLess(
            sync.index("waterfall_overlay_catchup_row"),
            sync.index("source_state->total_columns ="),
        )
        self.assertIn("waterfall_overlay_sync_start(channel)", present)
        self.assertIn("g_waterfall_overlay_sync.active", RF_UI_C)

    def test_overlay_palette_reserves_transparency_and_real_box_colors(self) -> None:
        self.assertIn("RF_WATERFALL_CLUT_HEAT_FIRST 1u", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_HEAT_LAST 9u", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_BOX_FIRST 10u", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_GAP_A 14u", RF_UI_C)
        self.assertIn("RF_WATERFALL_CLUT_GAP_B 15u", RF_UI_C)
        self.assertIn("RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS 16u", RF_UI_H)
        self.assertIn("memset(g_waterfall_clut_palette, 0", RF_UI_C)
        self.assertIn("g_target_accent_colors[index]", RF_UI_C)
        self.assertIn("256u + RF_UI_DETECTION_COUNT + 2u", RF_UI_C)
        self.assertIn("waterfall_overlay_box_raster", RF_UI_C)
        self.assertIn("waterfall_history_box_raster", RF_UI_C)
        self.assertNotIn("waterfall_overlay_pixel_restore", RF_UI_C)

    def test_overlay_failure_returns_to_the_bounded_v26_builder(self) -> None:
        self.assertIn("rf_ui_waterfall_overlay_fail", RF_UI_C)
        self.assertIn("g_waterfall_overlay.fallback_rebuilding = true", RF_UI_C)
        self.assertIn("ui_rf_waterfall_overlay_disable", LVGL_APP_C)
        failure = LVGL_APP_C.split("if (g_rf_overlay_failed)", 1)[1].split(
            "switch (g_rf_overlay_state)", 1
        )[0]
        self.assertIn("rf_ui_waterfall_overlay_disable_ready()", failure)
        self.assertIn("fallback_disable_ready = true", RF_UI_C)
        self.assertIn("R_GLCDC_BufferChange", LVGL_APP_C)
        self.assertIn("DISPLAY_FRAME_LAYER_2", LVGL_APP_C)
        self.assertIn("overlay_fallbacks", RF_UI_H)
        self.assertIn("overlay_underflows", RF_UI_H)
        self.assertIn("RF_UI_RUNTIME_MONITOR_VERSION     2u", RF_UI_H)


if __name__ == "__main__":
    unittest.main()
