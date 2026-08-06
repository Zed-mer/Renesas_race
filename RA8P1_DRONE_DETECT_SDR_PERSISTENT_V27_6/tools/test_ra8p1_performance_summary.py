#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ra8p1_performance_summary as summary


CENTERS = (2_420_000_000, 2_464_000_000, 5_760_000_000, 5_816_000_000)


def sender_text(*, capture: bool = True, timestamps: bool = False) -> str:
    lines: list[str] = []
    for index, center in enumerate(CENTERS):
        session = 7001 + index
        if capture:
            lines.append(
                f"captured center_hz={center} samples=590336 bytes=2361344 "
                f"chunks=1 capture_ms={11 + index}"
            )
        interval = f" start_ms={index * 100} end_ms={(index + 1) * 100}" if timestamps else ""
        lines.append(
            f"sent session={session} center_index={index} center_hz={center} "
            "samples=590336 data_packets=1640 udp_packets=1642 target_mbps=320 "
            f"payload_mbps_x1000={295000 + index} tiles=1 stride=295168{interval}"
        )
    return "\n".join(lines) + "\n"


def runtime_payload(*, with_rate: bool = True) -> list[dict]:
    documents: list[dict] = []
    for index, _center in enumerate(CENTERS):
        session = 7001 + index
        runtime = {
            "InferenceRateHz": 2.5 + index * 0.1,
        } if with_rate else {
            "InferenceRateHz": 0.0,
        }
        documents.append({
            "Cpu0DwtClockHz": 1_000_000_000,
            "Snapshots": [{
                "SessionId": session,
                "Latest": {
                    "Valid": True,
                    "SessionId": session,
                    "CenterIndex": index,
                    "StftMs": 275.0 + index,
                    "NpuMs": 0.75,
                    "EndToEndMs": 336.0 + index,
                },
                "Latency": {
                    "ValidRecords": [{
                        "Valid": True,
                        "SessionId": session,
                        "WindowCompleteToCpu1VisibleUpperMs": 6.0 + index * 0.1,
                    }],
                },
                "Runtime": runtime,
            }],
        })
    return documents


class PerformanceSummaryTests(unittest.TestCase):
    def write_evidence(self, directory: Path, sender: str, runtime: list[dict]) -> tuple[Path, Path]:
        sender_path = directory / "sender.log"
        runtime_path = directory / "runtime.json"
        sender_path.write_text(sender, encoding="utf-8")
        runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
        return sender_path, runtime_path

    def test_stage_provenance_and_placeholder_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sender_path, runtime_path = self.write_evidence(
                Path(temporary), sender_text(), runtime_payload()
            )
            report = summary.build_summary(
                [sender_path], [runtime_path], tune_ms=2.5
            )
        first = report["single_frequency"][0]
        self.assertEqual(first["stages"]["capture"]["status"], "measured")
        self.assertEqual(first["stages"]["tune"]["status"], "estimated")
        self.assertEqual(first["stages"]["send"]["status"], "estimated")
        self.assertEqual(first["stages"]["stft"]["status"], "measured")
        self.assertEqual(first["stages"]["npu"]["status"], "measured")
        self.assertEqual(first["stages"]["e2e"]["status"], "measured")
        self.assertEqual(first["stages"]["cpu1_visible"]["status"], "measured")
        self.assertEqual(report["steady_state_fps"]["status"], "measured")
        self.assertEqual(report["four_frequency_total"]["status"], "estimated")
        self.assertIsNotNone(report["four_frequency_total"]["value_ms"])
        self.assertEqual(report["model_accuracy"]["status"], "not_proven")
        self.assertIn("placeholder", report["model_accuracy"]["conclusion"])

    def test_missing_capture_and_tune_block_serial_total(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sender_path, runtime_path = self.write_evidence(
                Path(temporary), sender_text(capture=False), runtime_payload(with_rate=False)
            )
            report = summary.build_summary([sender_path], [runtime_path])
        total = report["four_frequency_total"]
        self.assertEqual(total["status"], "missing")
        self.assertIsNone(total["value_ms"])
        self.assertTrue(any("capture timing" in item for item in total["missing"]))
        self.assertTrue(any("tune timing" in item for item in total["missing"]))
        self.assertEqual(report["steady_state_fps"]["status"], "estimated")

    def test_explicit_cycle_interval_is_measured(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sender_path, runtime_path = self.write_evidence(
                Path(temporary),
                sender_text(capture=False, timestamps=True),
                runtime_payload(with_rate=False),
            )
            report = summary.build_summary([sender_path], [runtime_path])
        total = report["four_frequency_total"]
        self.assertEqual(total["status"], "measured")
        self.assertEqual(total["value_ms"], 400.0)
        self.assertEqual(total["formula"], "cycle_end_ms - cycle_start_ms")

    def test_runtime_from_other_session_is_not_reused_by_center(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            sender_path, runtime_path = self.write_evidence(
                directory, sender_text(), runtime_payload()
            )
            payload = json.loads(runtime_path.read_text(encoding="utf-8"))
            payload[0]["Snapshots"][0]["SessionId"] = 999999
            runtime_path.write_text(json.dumps(payload), encoding="utf-8")
            report = summary.build_summary([sender_path], [runtime_path], tune_ms=1.0)
        self.assertEqual(report["single_frequency"][0]["stages"]["stft"]["status"], "missing")

    def test_invalid_runtime_timing_flags_are_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            sender_path, runtime_path = self.write_evidence(
                directory, sender_text(), runtime_payload()
            )
            payload = json.loads(runtime_path.read_text(encoding="utf-8"))
            latest = payload[0]["Snapshots"][0]["Latest"]
            latest["TimingFlags"] = 0
            runtime_path.write_text(json.dumps(payload), encoding="utf-8")
            report = summary.build_summary([sender_path], [runtime_path], tune_ms=1.0)
        self.assertEqual(report["single_frequency"][0]["stages"]["stft"]["status"], "missing")
        self.assertIn("TimingFlags", report["single_frequency"][0]["stages"]["stft"]["basis"])

    def test_capture_event_is_not_taken_from_a_future_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            sender = (
                "sent session=1 center_index=0 center_hz=2420000000 samples=590336 "
                "payload_mbps_x1000=300000\n"
                "captured center_hz=2420000000 samples=590336 capture_ms=17\n"
                "sent session=2 center_index=0 center_hz=2420000000 samples=590336 "
                "payload_mbps_x1000=300000\n"
            )
            sender_path, runtime_path = self.write_evidence(
                directory, sender, [{"Snapshots": [{"SessionId": 1}, {"SessionId": 2}]}]
            )
            report = summary.build_summary([sender_path], [runtime_path])
        self.assertEqual(report["single_frequency"][0]["stages"]["capture"]["status"], "missing")
        self.assertEqual(report["single_frequency"][1]["stages"]["capture"]["value_ms"], 17.0)

    def test_cli_writes_json_and_marks_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            sender_path, runtime_path = self.write_evidence(
                directory, sender_text(capture=False), runtime_payload(with_rate=False)
            )
            output = directory / "summary.json"
            code = summary.main([
                "--sender-log", str(sender_path),
                "--runtime", str(runtime_path),
                "--output-json", str(output),
                "--quiet",
            ])
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 0)
        self.assertEqual(report["four_frequency_total"]["status"], "missing")


if __name__ == "__main__":
    unittest.main()
