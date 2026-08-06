#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

import ra8p1_acceptance_report as acceptance


HERE = Path(__file__).resolve().parent
PASS = HERE / "acceptance_fixtures" / "pass"
FAIL = HERE / "acceptance_fixtures" / "fail"


class AcceptanceReportTests(unittest.TestCase):
    def base_args(self, output: Path, *, net: Path | None = None,
                  latency: Path | None = None, npu: Path | None = None,
                  sender: Path | None = None) -> list[str]:
        return [
            "--sender-log", str(sender or (PASS / "sender.log")),
            "--net-stats", str(net or (PASS / "net-stats.json")),
            "--runtime", str(PASS / "runtime.json"),
            "--latency", str(latency or (PASS / "latency.json")),
            "--npu-proof", str(npu or (PASS / "npu-proof.json")),
            "--output-json", str(output),
            "--quiet",
        ]

    def test_pass_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "report.json"
            self.assertEqual(acceptance.main(self.base_args(output)), 0)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["verdict"], "PASS")
        self.assertEqual(report["summary"]["highest_zero_loss_rate_mbps"], 800)
        self.assertEqual(report["summary"]["stable_rate_target_mbps"], 720)
        self.assertTrue(report["summary"]["stable_rate_verified"])
        self.assertAlmostEqual(report["summary"]["window_rf_span_ms"], 9.838933)
        self.assertTrue(all(item["state"] == "pass" for item in report["sessions"]))

    def test_fault_register_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "report.json"
            code = acceptance.main(self.base_args(output, npu=FAIL / "npu-proof-fault.json"))
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("CFSR=0x00000001" in item for item in report["failures"]))

    def test_gap_is_not_zero_loss(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            net = json.loads((PASS / "net-stats.json").read_text(encoding="utf-8"))
            net[-1]["Snapshots"][0]["Iq"]["SequenceGaps"] = 1
            net_path = temporary_path / "net-gap.json"
            net_path.write_text(json.dumps(net), encoding="utf-8")
            output = temporary_path / "report.json"
            code = acceptance.main(self.base_args(output, net=net_path))
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        failed = next(item for item in report["sessions"] if item["session_id"] == 1007)
        self.assertEqual(failed["state"], "fail")
        self.assertTrue(any("SequenceGaps=1" in item for item in failed["net"]["failures"]))

    def test_iiod_diagnostic_is_rejected_for_formal_session(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            net = json.loads((PASS / "net-stats.json").read_text(encoding="utf-8"))
            net[0]["Snapshots"][0]["IiodPerf"] = {
                "Initialized": True,
                "State": 7,
                "BytesReceived": 11806720,
                "TargetBytes": 11806720,
            }
            # Keep a later payload-complete snapshot clean; the report must
            # still reject the earlier diagnostic observation.
            net[0]["Snapshots"].append(
                json.loads(json.dumps(net[0]["Snapshots"][0]))
            )
            net[0]["Snapshots"][1].pop("IiodPerf")
            net_path = temporary_path / "net-iiod-enabled.json"
            net_path.write_text(json.dumps(net), encoding="utf-8")
            output = temporary_path / "report.json"
            code = acceptance.main(self.base_args(output, net=net_path))
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        failed = next(item for item in report["sessions"] if item["session_id"] == 1000)
        self.assertEqual(failed["state"], "fail")
        self.assertTrue(any("iiod diagnostic is initialized" in item
                            for item in failed["net"]["failures"]))
        self.assertTrue(any("IiodPerf.BytesReceived" in item
                            for item in failed["net"]["failures"]))

    def test_backpressure_fails_latency_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            latency = json.loads((PASS / "latency.json").read_text(encoding="utf-8"))
            latency["Sessions"][0]["BackpressureEvents"] = 2
            latency_path = temporary_path / "latency-backpressure.json"
            latency_path.write_text(json.dumps(latency), encoding="utf-8")
            output = temporary_path / "report.json"
            code = acceptance.main(self.base_args(output, latency=latency_path))
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        failed = next(item for item in report["sessions"] if item["session_id"] == 1000)
        self.assertTrue(any("BackpressureEvents=2" in item for item in failed["latency"]["failures"]))

    def test_elf_hash_mismatch_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            runtime = json.loads((PASS / "runtime.json").read_text(encoding="utf-8"))
            runtime[-1]["Elf"]["Cpu0"]["Sha256"] = "C" * 64
            runtime_path = temporary_path / "runtime-mismatch.json"
            runtime_path.write_text(json.dumps(runtime), encoding="utf-8")
            output = temporary_path / "report.json"
            args = self.base_args(output)
            args[args.index(str(PASS / "runtime.json"))] = str(runtime_path)
            code = acceptance.main(args)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        self.assertTrue(any("CPU0 ELF hash mismatch" in item for item in report["failures"]))

    def test_sender_requires_exactly_19_tiles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            text = (PASS / "sender.log").read_text(encoding="utf-8")
            sender_path = temporary_path / "sender-invalid.log"
            sender_path.write_text(text.replace("tiles=19", "tiles=18", 1), encoding="utf-8")
            output = temporary_path / "report.json"
            code = acceptance.main(self.base_args(output, sender=sender_path))
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(code, 1)
        self.assertTrue(any("tiles=18" in item for item in report["failures"]))

    def test_utf16_powershell_output_is_readable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "powershell.json"
            path.write_text('{"ok": true}', encoding="utf-16")
            self.assertEqual(json.loads(acceptance.read_text_file(path)), {"ok": True})


if __name__ == "__main__":
    unittest.main()
