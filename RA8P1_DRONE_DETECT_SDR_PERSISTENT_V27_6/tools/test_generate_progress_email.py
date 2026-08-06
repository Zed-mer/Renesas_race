#!/usr/bin/env python3

import email
import email.policy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_progress_email as generator


def sample_summary() -> dict:
    metric = lambda value, status, basis: {
        "status": status,
        "value_ms": value,
        "basis": basis,
        "source": ["fixture.json"],
    }
    return {
        "tool": "ra8p1-performance-summary",
        "tool_version": "1.0",
        "generated_utc": "2026-07-22T00:00:00Z",
        "contract": {"sample_rate_hz": 60000000, "window_samples": 590336, "rf_window_span_ms": 9.838933},
        "single_frequency": [{
            "session_id": 1,
            "center_hz": 2420000000,
            "stages": {
                "capture": metric(None, "missing", "capture absent"),
                "tune": metric(120.0, "estimated", "operator fallback"),
                "send": metric(64.0, "estimated", "payload/rate"),
                "stft": metric(275.0, "measured", "runtime field"),
                "npu": metric(0.75, "measured", "runtime field"),
                "e2e": metric(336.0, "measured", "latency field"),
                "cpu1_visible": metric(6.1, "measured", "latency field"),
            },
            "steady_state_fps": {
                "status": "measured", "value_hz": 2.92,
                "basis": "cycle timing", "source": ["runtime.json"],
            },
        }],
        "steady_state_fps": {
            "status": "measured", "value_hz": 2.92,
            "basis": "median", "source": ["runtime.json"],
        },
        "four_frequency_total": {
            "status": "missing", "value_ms": None,
            "basis": "capture/tune evidence absent", "source": [],
            "formula": "cycle_end_ms - cycle_start_ms",
            "missing": ["capture timing", "tune timing"],
        },
        "model_accuracy": {"status": "not_proven", "conclusion": "placeholder model"},
        "evidence_boundary": "Timing only; no accuracy claim.",
        "warnings": [],
    }


class GenerateProgressEmailTests(unittest.TestCase):
    def test_json_draft_has_explicit_statuses_and_no_send_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            summary = directory / "summary.json"
            eml = directory / "draft.eml"
            summary.write_text(json.dumps(sample_summary()), encoding="utf-8")
            code = generator.main([
                "--summary-json", str(summary),
                "--to", "operator@example.com",
                "--output-eml", str(eml),
            ])
            self.assertEqual(code, 0)
            message = email.message_from_bytes(eml.read_bytes(), policy=email.policy.default)
            body = message.get_content()
            self.assertEqual(message["To"], "operator@example.com")
            self.assertEqual(message["X-Delivery-Status"], "DRAFT-NOT-SENT")
            self.assertIn("275 ms [MEASURED]", body)
            self.assertIn("120 ms [ESTIMATED]", body)
            self.assertIn("MISSING [MISSING]", body)
            self.assertIn("2.92 Hz [MEASURED]", body)
            self.assertIn("placeholder model", body)

    def test_to_is_required_and_header_injection_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            summary = Path(temporary) / "summary.json"
            summary.write_text(json.dumps(sample_summary()), encoding="utf-8")
            code = generator.main([
                "--summary-json", str(summary),
                "--to", "bad\nBcc: x@example.com",
                "--output-eml", str(Path(temporary) / "draft.eml"),
            ])
        self.assertEqual(code, 2)

    def test_markdown_requires_status_marker_and_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            markdown = directory / "summary.md"
            eml = directory / "draft.eml"
            markdown.write_text("| STFT | 10 ms [MEASURED] |\n", encoding="utf-8")
            code = generator.main([
                "--summary-md", str(markdown),
                "--to", "operator@example.com",
                "--output-eml", str(eml),
            ])
            self.assertEqual(code, 0)
            body = email.message_from_bytes(eml.read_bytes(), policy=email.policy.default).get_content()
            self.assertIn("source Markdown", body)
            self.assertIn("10 ms [MEASURED]", body)

            invalid = directory / "invalid.md"
            invalid.write_text("STFT 10 ms\n", encoding="utf-8")
            self.assertEqual(generator.main([
                "--summary-md", str(invalid),
                "--to", "operator@example.com",
                "--output-eml", str(directory / "invalid.eml"),
            ]), 2)

    def test_malformed_metric_cannot_hide_a_missing_value(self) -> None:
        summary = sample_summary()
        summary["four_frequency_total"]["value_ms"] = 123.0
        with self.assertRaises(generator.InputError):
            generator.render_json_summary(summary)


if __name__ == "__main__":
    unittest.main()
