#!/usr/bin/env python3
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIGURATION = ROOT / "cpu1/configuration.xml"
DISPLAY_C = (ROOT / "cpu1/src/display_bringup.c").read_text(encoding="utf-8")
DISPLAY_H = (ROOT / "cpu1/src/display_bringup.h").read_text(encoding="utf-8")
GENERATED_C = (ROOT / "cpu1/ra_gen/common_data.c").read_text(encoding="utf-8")
EASE_SCRIPT = (ROOT / "tools/configure-display-v27-stable.py").read_text(
    encoding="utf-8"
)


def clock_option(root: ET.Element, node_id: str) -> str:
    node = root.find(f".//node[@id='{node_id}']")
    if node is None:
        raise AssertionError(f"missing clock node {node_id}")
    return node.attrib["option"]


def module_property(root: ET.Element, property_id: str) -> str:
    prop = root.find(f".//property[@id='{property_id}']")
    if prop is None:
        raise AssertionError(f"missing module property {property_id}")
    return prop.attrib["value"]


class DisplayTimingContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = ET.parse(CONFIGURATION).getroot()

    def test_clock_tree_produces_exact_40_mhz_pixel_clock(self) -> None:
        self.assertEqual(
            clock_option(self.root, "board.clock.pll2.source"),
            "board.clock.pll2.source.xtal",
        )
        self.assertEqual(
            clock_option(self.root, "board.clock.pll2.div"),
            "board.clock.pll2.div.3",
        )
        self.assertEqual(
            clock_option(self.root, "board.clock.pll2.mul"),
            "board.clock.pll2.mul.300_00",
        )
        self.assertEqual(
            clock_option(self.root, "board.clock.pll2r.div"),
            "board.clock.pll2r.div.5",
        )
        self.assertEqual(
            clock_option(self.root, "board.clock.lcdclk.div"),
            "board.clock.lcdclk.div.2",
        )
        self.assertEqual(
            module_property(
                self.root, "module.driver.display.clock_div_ratio"
            ),
            "module.driver.display.clock_div_ratio.panel_clk_divisor_6",
        )
        self.assertEqual(24_000_000 // 3 * 300 // 5 // 2 // 6, 40_000_000)

    def test_totals_and_porches_match_v27_stable_contract(self) -> None:
        htotal = int(module_property(
            self.root, "module.driver.display.output.htiming.total_cyc"
        ))
        hactive = int(module_property(
            self.root, "module.driver.display.output.htiming.display_cyc"
        ))
        hback_start = int(module_property(
            self.root, "module.driver.display.output.htiming.back_porch"
        ))
        hsync = int(module_property(
            self.root, "module.driver.display.output.htiming.sync_width"
        ))
        vtotal = int(module_property(
            self.root, "module.driver.display.output.vtiming.total_cyc"
        ))
        vactive = int(module_property(
            self.root, "module.driver.display.output.vtiming.display_cyc"
        ))
        vback_start = int(module_property(
            self.root, "module.driver.display.output.vtiming.back_porch"
        ))
        vsync = int(module_property(
            self.root, "module.driver.display.output.vtiming.sync_width"
        ))

        self.assertEqual((htotal, hactive, hback_start, hsync),
                         (1344, 1024, 136, 24))
        self.assertEqual((vtotal, vactive, vback_start, vsync),
                         (635, 600, 21, 2))
        self.assertEqual((hback_start - hsync,
                          htotal - hactive - hback_start), (112, 184))
        self.assertEqual((vback_start - vsync,
                          vtotal - vactive - vback_start), (19, 14))
        self.assertEqual((hback_start - hsync,
                          htotal - hactive - hback_start - hsync),
                         (112, 160))
        self.assertEqual((vback_start - vsync,
                          vtotal - vactive - vback_start - vsync),
                         (19, 12))
        self.assertAlmostEqual(40_000_000 / (htotal * vtotal),
                               46.869141, places=6)

    def test_generated_glcdc_values_follow_configuration(self) -> None:
        self.assertRegex(GENERATED_C, r"\.total_cyc\s*=\s*1344,")
        self.assertRegex(GENERATED_C, r"\.back_porch\s*=\s*136,")

    def test_generated_dsi_tuple_matches_generated_delay(self) -> None:
        self.assertRegex(GENERATED_C,
                         r"\.horizontal_back_porch\s*=\s*\(136 - 24\),")
        self.assertRegex(
            GENERATED_C,
            r"\.horizontal_front_porch\s*=\s*\(1344 - 1024 - 136 - 24\),",
        )
        self.assertRegex(GENERATED_C,
                         r"\.vertical_back_porch\s*=\s*\(21 - 2\),")
        self.assertRegex(
            GENERATED_C,
            r"\.vertical_front_porch\s*=\s*\(635 - 600 - 21 - 2\),",
        )
        self.assertRegex(GENERATED_C,
                         r"\.video_mode_delay\s*=\s*184\b")

    def test_runtime_validates_dsi_tuple_without_mutating_it(self) -> None:
        self.assertIn("generated DSI porches and delay", DISPLAY_C)
        self.assertIn("DISPLAY_HORIZONTAL_TOTAL_CYC          (1344U)",
                      DISPLAY_C)
        self.assertIn("DISPLAY_DSI_HORIZONTAL_FRONT_PORCH_CYC (160U)",
                      DISPLAY_C)
        self.assertIn("DISPLAY_DSI_VERTICAL_FRONT_PORCH_CYC   (12U)",
                      DISPLAY_C)
        self.assertIn("DISPLAY_DSI_VIDEO_MODE_DELAY           (184U)",
                      DISPLAY_C)
        self.assertIn("g_display_diag.dsi_timing_verified = 1U;", DISPLAY_C)
        for member in (
            "horizontal_active_lines",
            "horizontal_sync_lines",
            "horizontal_back_porch",
            "horizontal_front_porch",
            "vertical_active_lines",
            "vertical_sync_lines",
            "vertical_back_porch",
            "vertical_front_porch",
            "video_mode_delay",
        ):
            self.assertNotRegex(
                DISPLAY_C,
                rf"g_external_dsi_cfg\.{member}\s*=",
            )
        for field in (
            "dsi_timing_verified",
            "horizontal_back_porch_cyc",
            "horizontal_front_porch_cyc",
            "vertical_back_porch_cyc",
            "vertical_front_porch_cyc",
            "dsi_video_mode_delay",
        ):
            self.assertRegex(DISPLAY_H, rf"uint32_t\s+{field};")

    def test_ease_script_uses_semantic_stack_setters(self) -> None:
        self.assertIn("openDDSCConfigurationWithVersion", EASE_SCRIPT)
        self.assertIn("prop.getOptions()", EASE_SCRIPT)
        self.assertIn("prop.setValue(value)", EASE_SCRIPT)
        self.assertIn("configuration.getProblems()", EASE_SCRIPT)
        self.assertIn("configuration.save()", EASE_SCRIPT)
        self.assertIn("CONTENT_GENERATION_REQUIRED=solution-build", EASE_SCRIPT)
        self.assertNotIn("ElementTree", EASE_SCRIPT)


if __name__ == "__main__":
    unittest.main()
