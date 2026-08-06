"""Regression checks for the production/diagnostic iiod boot switch."""

from __future__ import annotations

import pathlib
import re
import unittest

from project_layout import resolve_cpu0


ROOT = pathlib.Path(__file__).resolve().parents[1]
CPU0 = resolve_cpu0(ROOT)


class SdrIiodModeTests(unittest.TestCase):
    def test_production_default_is_disabled(self) -> None:
        header = (CPU0 / "src" / "sdr_iiod_perf.h").read_text(encoding="utf-8")
        match = re.search(
            r"^\s*#define\s+SDR_IIOD_PERF_BOOT_ENABLE\s+([01])\s*$",
            header,
            re.MULTILINE,
        )
        self.assertIsNotNone(match)
        self.assertEqual("0", match.group(1))

    def test_hal_entry_call_is_compile_guarded(self) -> None:
        entry = (CPU0 / "src" / "hal_entry.c").read_text(encoding="utf-8")
        keepalive = re.search(
            r"#if\s+!SDR_IIOD_PERF_BOOT_ENABLE(?P<body>.*?)#endif",
            entry,
            re.DOTALL,
        )
        self.assertIsNotNone(keepalive)
        self.assertIn("g_sdr_iiod_perf_result.magic", keepalive.group("body"))
        self.assertIn("g_sdr_iiod_perf_report[0]", keepalive.group("body"))
        guarded = re.search(
            r"#if\s+SDR_IIOD_PERF_BOOT_ENABLE(?P<body>.*?)#endif",
            entry,
            re.DOTALL,
        )
        self.assertIsNotNone(guarded)
        self.assertIn("sdr_iiod_perf_start();", guarded.group("body"))

    def test_documented_override_is_explicit_diagnostic_only(self) -> None:
        readme = (ROOT / "tools" / "README_SDR_IIOD_PERF.md").read_text(
            encoding="utf-8"
        )
        self.assertIn("SDR_IIOD_PERF_BOOT_ENABLE=0", readme)
        self.assertIn("SDR_IIOD_PERF_BOOT_ENABLE=1", readme)
        self.assertRegex(readme, r"(?s)Do not use.*diagnostic build.*formal IQSC")


if __name__ == "__main__":
    unittest.main()
