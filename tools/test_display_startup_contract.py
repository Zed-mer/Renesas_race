#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRINGUP_C = (ROOT / "cpu1/src/display_bringup.c").read_text(
    encoding="utf-8"
)
BRINGUP_H = (ROOT / "cpu1/src/display_bringup.h").read_text(
    encoding="utf-8"
)
HAL_ENTRY = (ROOT / "cpu1/src/hal_entry.c").read_text(
    encoding="utf-8"
)
WARMSTART = (ROOT / "cpu1/src/hal_warmstart.c").read_text(
    encoding="utf-8"
)
CPU0_WARMSTART = (ROOT / "cpu0/libraries/HAL_Drivers/drv_common.c").read_text(
    encoding="utf-8"
)
PANEL_C = (ROOT / "cpu1/src/jd9165_panel.c").read_text(
    encoding="utf-8"
)
CPU0_CONFIGURATION = (ROOT / "cpu0/configuration.xml").read_text(
    encoding="utf-8"
)
CPU1_CONFIGURATION = (ROOT / "cpu1/configuration.xml").read_text(
    encoding="utf-8"
)
SOLUTION_CONFIGURATION = (ROOT / "solution.xml").read_text(
    encoding="utf-8"
)
CPU1_BSP_CFG = (ROOT / "cpu1/ra_cfg/fsp_cfg/bsp/bsp_cfg.h").read_text(
    encoding="utf-8"
)
CPU0_BSP_CFG = (ROOT / "cpu0/ra_cfg/fsp_cfg/bsp/bsp_cfg.h").read_text(
    encoding="utf-8"
)
STARTUP_SOAK = (ROOT / "tools/run_display_startup_reset_soak.ps1").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


class DisplayStartupContractTest(unittest.TestCase):
    def test_early_init_is_enabled_in_every_configuration_source(self) -> None:
        enabled = (
            'id="config.bsp.common.early_init" '
            'value="config.bsp.common.early_init.enabled"'
        )
        self.assertIn(enabled, CPU0_CONFIGURATION)
        self.assertIn(enabled, CPU1_CONFIGURATION)
        self.assertIn(enabled, SOLUTION_CONFIGURATION)
        self.assertIn("#define BSP_CFG_EARLY_INIT     ((1))", CPU0_BSP_CFG)
        self.assertIn("#define BSP_CFG_EARLY_INIT     ((1))", CPU1_BSP_CFG)

    def test_cpu0_clamps_backlight_and_panel_reset_before_cpu1_start(self) -> None:
        body = function_body(
            CPU0_WARMSTART, "void R_BSP_WarmStart (bsp_warm_start_event_t event)"
        )
        reset_phase, post_c = body.split(
            "if (BSP_WARM_START_POST_C == event)", 1
        )
        self.assertIn("DISPLAY_STARTUP_BACKLIGHT_PIN", reset_phase)
        self.assertIn("DISPLAY_STARTUP_PANEL_RESET_PIN", reset_phase)
        self.assertGreaterEqual(reset_phase.count("R_BSP_PinWrite"), 2)
        self.assertIn("BSP_IO_LEVEL_LOW", reset_phase)
        self.assertGreaterEqual(post_c.count("R_IOPORT_PinCfg"), 2)
        self.assertGreaterEqual(post_c.count("R_IOPORT_PinWrite"), 4)
        self.assertIn(
            "#define DISPLAY_STARTUP_PANEL_RESET_PIN (BSP_IO_PORT_04_PIN_11)",
            CPU0_WARMSTART,
        )

    def test_lvgl_first_frame_uses_the_pre_video_ready_contract(self) -> None:
        before_loop = HAL_ENTRY.split("while (1)", 1)[0]
        self.assertIn("display_bringup_ready_for_first_frame()", before_loop)
        self.assertNotIn("if (0U != g_display_diag.running)", before_loop)

    def test_warmstart_configures_backlight_and_resx_low(self) -> None:
        post_c = WARMSTART.index("R_IOPORT_Open")
        safe = WARMSTART[post_c:]
        self.assertIn("DISPLAY_STARTUP_BACKLIGHT_PIN", safe)
        self.assertIn("DISPLAY_STARTUP_PANEL_RESET_PIN", safe)
        self.assertGreaterEqual(safe.count("R_IOPORT_PinCfg"), 2)
        self.assertGreaterEqual(safe.count("R_IOPORT_PinWrite"), 2)
        self.assertIn("R_IOPORT_PinRead", safe)
        self.assertIn("display_startup_diag_note_reset_asserted", safe)
        self.assertIn("IOPORT_CFG_PORT_OUTPUT_LOW", WARMSTART)
        self.assertIn(
            "#define DISPLAY_STARTUP_PANEL_RESET_PIN (PANEL_RESET)",
            WARMSTART,
        )
        self.assertNotIn("BSP_IO_PORT_04_PIN_11", WARMSTART)

    def test_panel_reset_stays_low_then_releases_once(self) -> None:
        self.assertIn("#define DISPLAY_PANEL_RESET   (PANEL_RESET)", BRINGUP_C)
        self.assertNotIn("BSP_IO_PORT_04_PIN_11", BRINGUP_C)
        reset_cfg = BRINGUP_C.split("#define DISPLAY_RESET_PIN_CFG", 1)[1].split(
            "#define DISPLAY_BACKLIGHT_PIN_CFG", 1
        )[0]
        self.assertIn("IOPORT_CFG_PORT_OUTPUT_LOW", reset_cfg)
        self.assertNotIn("IOPORT_CFG_PORT_OUTPUT_HIGH", reset_cfg)
        body = function_body(
            BRINGUP_C, "static fsp_err_t display_panel_hardware_reset(void)"
        )
        asserted = body.index("DISPLAY_RESET_ACTIVE")
        hold = body.index(
            "R_BSP_SoftwareDelay(DISPLAY_RESET_LOW_HOLD_MS"
        )
        released = body.index("DISPLAY_RESET_IDLE")
        settle = body.index(
            "R_BSP_SoftwareDelay(DISPLAY_RESET_RELEASE_WAIT_MS"
        )
        self.assertLess(asserted, hold)
        self.assertLess(hold, released)
        self.assertLess(released, settle)
        self.assertEqual(body.count("DISPLAY_RESET_IDLE"), 1)
        self.assertNotIn("SoftwareDelay(5U", body)
        self.assertIn("DISPLAY_RESET_READY_MIN_MS         (17U)", BRINGUP_C)
        self.assertIn("DISPLAY_RESET_RELEASE_WAIT_MS      (120U)", BRINGUP_C)

    def test_vendor_panel_delays_remain_unchanged(self) -> None:
        self.assertIn("{0x11, {0x00}, 0, 120}", PANEL_C)
        self.assertIn("{0x29, {0x00}, 0, 20}", PANEL_C)

    def test_startup_skips_optional_bta_reads(self) -> None:
        body = function_body(BRINGUP_C, "void display_bringup_run(void)")
        self.assertNotIn("jd9165_panel_read_lane_config(", body)
        self.assertNotIn("jd9165_panel_read_power_mode(", body)
        self.assertIn(
            "#define DISPLAY_PANEL_READ_SKIPPED (UINT32_MAX)", BRINGUP_C
        )
        self.assertIn(
            "panel_lane_read_error = DISPLAY_PANEL_READ_SKIPPED", body
        )
        self.assertIn("panel_read_error = DISPLAY_PANEL_READ_SKIPPED", body)

    def test_backlight_gate_runs_only_after_owner_step(self) -> None:
        before_loop, loop = HAL_ENTRY.split("while (1)", 1)
        self.assertNotIn("display_backlight_startup_step", before_loop)
        self.assertLess(
            loop.index("lvgl_app_step"),
            loop.index("display_backlight_startup_step"),
        )
        self.assertNotIn("display_backlight_enable_after_first_frame", HAL_ENTRY)
        callback = function_body(
            BRINGUP_C, "void glcdc_callback(display_callback_args_t * p_args)"
        )
        self.assertNotIn("lv_", callback)
        self.assertNotIn("display_backlight_startup_step", callback)

    def test_gate_requires_both_layers_and_sixteen_clean_vsyncs(self) -> None:
        step = function_body(
            BRINGUP_C, "fsp_err_t display_backlight_startup_step(void)"
        )
        self.assertIn("DISPLAY_STARTUP_CLEAN_VSYNCS        (16U)", BRINGUP_C)
        for prerequisite in (
            "startup_black_framebuffer_ready",
            "startup_panel_configured",
            "startup_video_started",
            "animation_buffer_changes",
            "overlay_enabled",
            "DISPLAY_FRAME_LAYER_1].VEN_b.PVEN",
            "DISPLAY_FRAME_LAYER_2].VEN_b.PVEN",
        ):
            self.assertIn(prerequisite, step)
        for health_field in (
            "glcdc_underflows",
            "overlay_underflows",
            "animation_buffer_errors",
            "overlay_errors",
            "video_status",
            "fatal_status",
            "phy_status",
        ):
            self.assertIn(health_field, BRINGUP_C)
        self.assertIn("display_startup_health_equal", step)
        self.assertIn("startup_clean_vsync_restarts++", step)
        self.assertNotIn("SoftwareDelay", step)

    def test_startup_soak_matches_reset_and_clean_vsync_contract(self) -> None:
        self.assertIn("startup_reset_low_hold_ms -eq 50U", STARTUP_SOAK)
        self.assertIn("startup_clean_vsync_required -eq 16U", STARTUP_SOAK)
        self.assertIn("startup_clean_vsync_count -ge 16U", STARTUP_SOAK)

    def test_backlight_is_enabled_once_after_the_clean_gate(self) -> None:
        step = function_body(
            BRINGUP_C, "fsp_err_t display_backlight_startup_step(void)"
        )
        clean_gate = step.index("DISPLAY_STARTUP_CLEAN_VSYNCS")
        enable = step.rindex("BSP_IO_LEVEL_HIGH")
        self.assertLess(clean_gate, enable)
        self.assertIn("startup_backlight_transitions++", step)
        self.assertIn("DISPLAY_STAGE_BACKLIGHT_ON", step)
        self.assertIn("startup_backlight_transitions", BRINGUP_H)

    def test_startup_diagnostics_record_strict_visibility_order(self) -> None:
        send = function_body(
            PANEL_C,
            "static fsp_err_t jd9165_send_with_flags(uint8_t command,",
        )
        self.assertLess(
            send.index("display_startup_diag_note_first_dsi_command"),
            send.index("R_MIPI_DSI_Command"),
        )
        for field in (
            "startup_reset_assert_sequence",
            "startup_reset_release_sequence",
            "startup_first_dsi_command_sequence",
            "startup_backlight_enable_sequence",
            "startup_sequence_valid",
            "startup_reset_low_hold_ms",
            "startup_reset_release_wait_ms",
        ):
            self.assertIn(field, BRINGUP_H)
        step = function_body(
            BRINGUP_C, "fsp_err_t display_backlight_startup_step(void)"
        )
        self.assertIn("startup_sequence_valid", step)
        self.assertIn("startup_first_dsi_command_sequence", step)

    def test_display_timing_contract_is_unchanged(self) -> None:
        for value in (
            "DISPLAY_PANEL_CLOCK_HZ                (40000000U)",
            "DISPLAY_HORIZONTAL_TOTAL_CYC          (1344U)",
            "DISPLAY_VERTICAL_TOTAL_CYC            (635U)",
            "DISPLAY_DSI_HORIZONTAL_BACK_PORCH_CYC  (112U)",
            "DISPLAY_DSI_HORIZONTAL_FRONT_PORCH_CYC (160U)",
            "DISPLAY_DSI_VERTICAL_BACK_PORCH_CYC    (19U)",
            "DISPLAY_DSI_VERTICAL_FRONT_PORCH_CYC   (12U)",
            "DISPLAY_DSI_VIDEO_MODE_DELAY           (184U)",
        ):
            self.assertIn(value, BRINGUP_C)


if __name__ == "__main__":
    unittest.main()
