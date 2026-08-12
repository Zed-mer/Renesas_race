#!/usr/bin/env python3
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOLUTION = ROOT / "solution.xml"
CPU0_CONFIGURATION = ROOT / "cpu0/configuration.xml"
CPU1_CONFIGURATION = ROOT / "cpu1/configuration.xml"
CPU0_PIN_HEADER = ROOT / "cpu0/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h"
CPU1_PIN_HEADER = ROOT / "cpu1/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h"
CPU0_PIN_DATA = ROOT / "cpu0/ra_gen/pin_data.c"
CPU1_PIN_DATA = ROOT / "cpu1/ra_gen/pin_data.c"
EASE_SCRIPT = ROOT / "tools/configure_p411_panel_reset_fsp.py"


def pin_configuration(path: Path, root_tag: str):
    root = ET.parse(path).getroot()
    if root.tag != root_tag:
        raise AssertionError(f"unexpected root in {path}: {root.tag}")
    nodes = root.findall("./raPinConfiguration")
    if len(nodes) != 1:
        raise AssertionError(f"expected one raPinConfiguration in {path}")
    profiles = nodes[0].findall("./pincfg")
    names = [profile.attrib["name"] for profile in profiles]
    if len(names) != len(set(names)):
        raise AssertionError(f"duplicate pin profiles in {path}")
    return root, nodes[0], {profile.attrib["name"]: profile for profile in profiles}


def option(root: ET.Element, key: str) -> str:
    node = root.find(f"./generalSettings/option[@key='{key}']")
    if node is None:
        raise AssertionError(f"missing general setting {key}")
    return node.attrib["value"]


def settings(profile: ET.Element) -> dict[str, str]:
    return {
        node.attrib["configurationId"]: node.attrib["altId"]
        for node in profile.findall("./configSetting")
    }


def p411_symbols(pin_node: ET.Element) -> list[str]:
    return [
        node.attrib.get("value", "")
        for node in pin_node.findall("./symbolicName")
        if node.attrib.get("propertyId") == "p411.symbolic_name"
    ]


def assert_profile_selection(
    test: unittest.TestCase,
    profiles: dict[str, ET.Element],
    selected_name: str,
) -> None:
    for name, profile in profiles.items():
        selected = name == selected_name
        test.assertEqual(profile.attrib.get("active") == "true", selected, name)
        test.assertEqual(profile.attrib.get("selected") == "true", selected, name)
        test.assertEqual(
            profile.attrib.get("symbol", ""),
            "g_bsp_pin_cfg" if selected else "",
            name,
        )


def generated_pin_entry(source: str, pin: str) -> str:
    match = re.search(
        rf"\{{\s*\.pin\s*=\s*{re.escape(pin)},\s*"
        rf"\.pin_cfg\s*=\s*\((.*?)\)\s*\}},",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"generated entry not found for {pin}")
    return match.group(1)


class P411PanelResetContractTest(unittest.TestCase):
    def test_solution_has_one_ra8p1_pin_owner(self) -> None:
        root, pin_node, profiles = pin_configuration(SOLUTION, "raSolution")
        self.assertEqual(option(root, "#TargetName#"), "R7KA8P1KFLCAC")
        self.assertEqual(option(root, "#FSPVersion#"), "6.4.0")
        self.assertEqual(len(profiles), 4)
        self.assertEqual(p411_symbols(pin_node), ["PANEL_RESET"])
        assert_profile_selection(self, profiles, "RA8P1_CPKHMI.pincfg")
        active = settings(profiles["RA8P1_CPKHMI.pincfg"])
        self.assertEqual(active["p411"], "p411.output.low")
        self.assertEqual(
            active["p411.gpio_mode"],
            "p411.gpio_mode.gpio_mode_out.low",
        )
        self.assertFalse(any("dsi_te" in value.lower() for value in active.values()))

    def test_cpu0_releases_p411(self) -> None:
        root, pin_node, profiles = pin_configuration(
            CPU0_CONFIGURATION, "raConfiguration"
        )
        self.assertEqual(option(root, "#TargetName#"), "R7KA8P1KFLCAC")
        self.assertEqual(option(root, "#FSPVersion#"), "6.4.0")
        self.assertEqual(p411_symbols(pin_node), [])
        assert_profile_selection(self, profiles, "RA8P1_Competition_Board")
        active = settings(profiles["RA8P1_Competition_Board"])
        self.assertFalse(
            any(key == "p411" or key.startswith("p411.") for key in active)
        )
        self.assertFalse(any("dsi_te" in value.lower() for value in active.values()))

    def test_cpu1_owns_p411_as_output_low(self) -> None:
        root, pin_node, profiles = pin_configuration(
            CPU1_CONFIGURATION, "raConfiguration"
        )
        self.assertEqual(option(root, "#TargetName#"), "R7KA8P1KFLCAC")
        self.assertEqual(option(root, "#FSPVersion#"), "6.4.0")
        self.assertEqual(p411_symbols(pin_node), ["PANEL_RESET"])
        assert_profile_selection(self, profiles, "RA8P1_CPKHMI.pincfg")
        active = settings(profiles["RA8P1_CPKHMI.pincfg"])
        self.assertEqual(active["p411"], "p411.output.low")
        self.assertEqual(
            active["p411.gpio_mode"],
            "p411.gpio_mode.gpio_mode_out.low",
        )
        self.assertFalse(any("dsi_te" in value.lower() for value in active.values()))

    def test_generated_cpu1_table_contains_output_low_panel_reset(self) -> None:
        header = CPU1_PIN_HEADER.read_text(encoding="utf-8")
        pin_data = CPU1_PIN_DATA.read_text(encoding="utf-8")
        self.assertRegex(
            header,
            r"#define\s+PANEL_RESET\s+\(BSP_IO_PORT_04_PIN_11\)",
        )
        self.assertIn("/* RA8P1_CPKHMI.pincfg */", header)
        entry = generated_pin_entry(pin_data, "BSP_IO_PORT_04_PIN_11")
        self.assertIn("IOPORT_CFG_PORT_DIRECTION_OUTPUT", entry)
        self.assertIn("IOPORT_CFG_PORT_OUTPUT_LOW", entry)
        self.assertNotIn("IOPORT_CFG_PERIPHERAL_PIN", entry)
        self.assertNotIn("IOPORT_PERIPHERAL_MIPI", entry)

    def test_generated_cpu0_table_does_not_claim_p411(self) -> None:
        header = CPU0_PIN_HEADER.read_text(encoding="utf-8")
        pin_data = CPU0_PIN_DATA.read_text(encoding="utf-8")
        self.assertNotIn("MIPI_TE", header)
        self.assertNotIn("PANEL_RESET", header)
        self.assertNotIn("BSP_IO_PORT_04_PIN_11", header)
        self.assertNotIn("BSP_IO_PORT_04_PIN_11", pin_data)

    def test_fsp_mutation_is_semantic_and_idempotence_guarded(self) -> None:
        script = EASE_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("openDDSCConfigurationWithVersion", script)
        self.assertIn("PinConfiguratorFacade", script)
        self.assertIn("configuration.getProblems()", script)
        self.assertIn("configuration.save()", script)
        self.assertIn("SOLUTION_ALREADY_CONFIGURED", script)
        self.assertNotIn("ET.ElementTree", script)
        self.assertNotIn("ET.SubElement", script)
        self.assertNotIn("ET.tostring", script)


if __name__ == "__main__":
    unittest.main()
