import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("ra8p1_board_campaign.py")
SPEC = importlib.util.spec_from_file_location("ra8p1_board_campaign", MODULE_PATH)
campaign = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(campaign)


CPU0_HASH = "A" * 64
CPU1_HASH = "B" * 64
SDR_AGENT_HASH = "C" * 64
SDR_ADAPTER_HASH = "D" * 64


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def net_document():
    return {
        "Elf": {"Sha256": CPU0_HASH},
        "Snapshots": [{
            "Phy": {
                "LinkUp": True,
                "GigabitControl": 0x0300,
                "GigabitStatus": 0x0C00,
            },
            "Rmac": {},
            "Iq": {
                "MbpsX1000": 800_000,
                "CrcBackend": 2,
                "CrcHardwareSelfTest": 1,
                "CrcTimingFlags": 3,
            },
            "Ring": {"FullDrops": 0, "OversizeDrops": 0},
        }],
    }


def trace_record(index: int, *, center_index=None, sequence=None):
    start = 100_000_000 * index
    iqsc_start = start + (20_000_000 if index == 0 else 152_000_000)
    prefetched = index > 0
    return {
        "Sequence": 2 * (index + 1) if sequence is None else sequence,
        "RequestId": 100 + index,
        "SessionId": 200 + index,
        "CenterIndex": index if center_index is None else center_index,
        "WindowIndex": 0,
        "SampleCount": campaign.WINDOW_SAMPLES,
        "Flags": "0x0000F7FF" if prefetched else "0x0000E7FF",
        "Status": 0,
        "RequestTxCycles": start,
        "AckTxCycles": start + 251_000_000,
        "NpuEndCycles": start + 250_000_000,
        "Cpu1VisibleCycles": start + 251_000_000,
        "CaptureReadyCycles": start + 10_000_000 if prefetched else 0,
        "CaptureCompleteCycles": iqsc_start + 24_000_000,
        "CreditAcceptedCycles": start + 251_500_000,
        "IqscStartCycles": iqsc_start,
        "RequestToCaptureReadyMs": 10.0 if prefetched else None,
        "CaptureReadyToIqscStartMs": 142.0 if prefetched else None,
        "IqscStartToCaptureCompleteMs": 24.0,
        "CaptureCompleteToCreditAcceptedMs": (
            207.5 if index == 0 else 75.5
        ),
        "AckToCreditAcceptedMs": 0.5,
        "RequestToIqscStartMs": 20.0 if index == 0 else 152.0,
        "StftMs": 180.0,
        "NpuMs": 3.0,
        "RequestToNpuResultMs": 250.0,
        "FirstPacketToNpuResultMs": 220.0,
        "CaptureStartToNpuUpperMs": 240.0,
        "RemoteTuneMs": 0.5,
        "RemoteCaptureMs": 9.839,
        "FirstToLastPacketMs": 23.613,
        "LastPacketToCrcCompleteMs": 0.1,
        "NpuToCpu1VisibleUpperMs": 1.0,
        "RequestToAckMs": 251.0,
        "PayloadMbpsX1000": 800_000,
        "SequenceGaps": 0,
        "Reordered": 0,
        "InvalidPackets": 0,
        "RingFullDrops": 0,
        "RingOversizeDrops": 0,
        "RingHighWatermark": 1000,
        "RingFree": 3000,
        "Cpu0LoadPermille": 600,
    }


def trace_document(
    records, *, records_started=None, records_overwritten=0, capacity=128,
    latest_sequence=None,
):
    return {
        "Elf": {"Sha256": CPU0_HASH},
        "Trace": {
            "BootCount": 1,
            "LatestSequence": (
                latest_sequence if latest_sequence is not None
                else (records[-1]["Sequence"] if records else 0)
            ),
            "RecordsStarted": len(records) if records_started is None else records_started,
            "RecordsOverwritten": records_overwritten,
            "Capacity": capacity,
            "Records": records,
        },
    }


def runtime_document(*, campaign_fields=True):
    runtime = {
        "Valid": True,
        "Headless": True,
        "ModelPlaceholder": True,
    }
    if campaign_fields:
        runtime.update({
            "Cpu0ReadySeen": True,
            "IpcDataSeen": True,
            "IpcFramesReceived": 4,
            "IpcTilesReceived": 4,
            "LastSessionId": 203,
        })
    return {
        "Elf": {
            "Cpu0": {"Sha256": CPU0_HASH},
            "Cpu1": {"Sha256": CPU1_HASH},
        },
        "Snapshots": [{"Runtime": runtime}],
    }


def agent_log(*, receipt="request", crc_backend="slice8", centers=None) -> str:
    centers = list(centers) if centers is not None else [0, 1, 2, 3]
    lines = []

    def append_receipt(index):
        request = 100 + index
        session = 200 + index
        if receipt == "accepted":
            lines.append(
                f"SDRC control_trace direction=tx event=CAPTURE_ACCEPTED command=0x8001 "
                f"request={request} session={session} status=0 attempt=0 credit=0"
            )
        else:
            lines.append(
                f"SDRC control_trace direction=rx event=CAPTURE_REQ command=0x0001 "
                f"request={request} session={session} status=0 attempt=0 credit=0"
            )

    for index, center in enumerate(centers):
        request = 100 + index
        session = 200 + index
        if index == 0:
            append_receipt(index)
        lines.append(
            f"SDRC window_trace event=complete request={request} session={session} "
            f"center_index={center} attempt=0 retransmit=0 samples=590336 "
            "target_mbps_x1000=800000 actual_mbps_x1000=800000 "
            "capture_elapsed_us=9839 tune_elapsed_us=500 send_elapsed_us=23613 "
            "transport=udp_gso gso_batches=52 gso_fallbacks=0 "
            f"crc_backend={crc_backend} "
            "adapter=Pluto-local-IIO-block+mmap "
            "adapter_block_setup_us=120 adapter_dma_wait_us=9400 "
            "adapter_disable_us=110 adapter_copy_us=209"
        )
        next_index = index + 1
        if next_index < len(centers):
            append_receipt(index + 1)
        lines.append(
            f"SDRC control_trace direction=rx event=WINDOW_ACK command=0x0002 "
            f"request={request} session={session} status=0 attempt=0 credit=1 "
            "gaps=0 reordered=0 invalid=0 ring_full_drops=0 "
            "ring_oversize_drops=0 crc_errors=0"
        )
        lines.append(
            f"SDRC control_trace direction=tx event=CREDIT_ACCEPTED command=0x8004 "
            f"request={request} session={session} status=0 attempt=0 credit=1"
        )
    return "\n".join(lines) + "\n"


def campaign_proof_document(
    *, windows=4, last_session=203, last_center=3, mode=3, elf_hash=CPU1_HASH,
):
    return {
        "Elf": {"Sha256": elf_hash},
        "Proof": {
            "WindowsExpected": windows,
            "WindowsVisible": windows,
            "State": 6,
            "StateName": "COMPLETE",
            "Mode": mode,
            "ModeName": "FOUR_OVERLAP" if mode == 3 else "FOUR_SERIAL",
            "LastSessionId": last_session,
            "LastResultCenterIndex": last_center,
            "LastError": 0,
            "Complete": True,
            "Failed": False,
        },
    }


def report_document():
    return {
        "failures": [],
        "warnings": [],
        "performance": {},
        "control": {},
        "sdr": {},
    }


class BoardCampaignTests(unittest.TestCase):
    def test_default_plan_covers_all_required_campaigns(self):
        plan = campaign.default_plan(800)
        campaign.validate_plan(plan)
        self.assertEqual(14, len(plan["scenarios"]))

    def test_missing_sdr_artifact_identity_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            management = Path(temporary) / "management.log"
            management.write_text("no deployed hashes\n", encoding="utf-8")
            report = {"failures": []}
            manifest = {"links": {"management": {"sdr_agent_location": "/tmp"}}}
            campaign.validate_sdr_artifacts(report, manifest, management)
            self.assertEqual(2, len(report["failures"]))

    def test_complete_overlap_scenario_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            records = [trace_record(index) for index in range(4)]
            scenario = campaign.scenario("rate-800", "rate-step", 800, [0, 1, 2, 3])
            manifest = {
                "scenario": scenario,
                "firmware": {
                    "cpu0": {"sha256": CPU0_HASH},
                    "cpu1": {"sha256": CPU1_HASH},
                },
                "sdr_artifacts": {
                    "agent": {"sha256": SDR_AGENT_HASH},
                    "adapter": {"sha256": SDR_ADAPTER_HASH},
                },
                "links": {
                    "programming": {"evidence": {"path": "flash.log"}},
                    "management": {
                        "evidence": {"path": "management.log"},
                        "issue": None,
                        "sdr_agent_location": "/tmp",
                    },
                },
                "phases": {
                    "before": {"captured_utc": "2026-07-23T00:00:00Z"},
                    "after": {"captured_utc": "2026-07-23T00:01:00Z"},
                },
            }
            write_json(root / "manifest.json", manifest)
            for phase in ("before", "after"):
                write_json(root / phase / "net.json", net_document())
                write_json(
                    root / phase / "runtime.json",
                    runtime_document(campaign_fields=False),
                )
            write_json(root / "before" / "trace.json", trace_document([]))
            write_json(root / "after" / "trace.json", trace_document(records))
            write_json(root / "after" / "cpu1_campaign.json", campaign_proof_document())
            (root / "agent.log").write_text(agent_log(), encoding="utf-8")
            (root / "flash.log").write_text("Program & Verify OK\n", encoding="utf-8")
            (root / "management.log").write_text(
                f"{SDR_AGENT_HASH}  /tmp/sdr_capture_agent\n"
                f"{SDR_ADAPTER_HASH}  /tmp/sdr_adapter_iio_mmap.so\n",
                encoding="utf-8",
            )
            report = campaign.verify_scenario(root / "manifest.json")
            self.assertEqual("PASS", report["status"], report["failures"])
            self.assertEqual("overlap", report["control"]["observed_pipeline_mode"])
            self.assertEqual("campaign-proof", report["cpu1"]["source"])
            self.assertEqual("measured", report["performance"]["steady_inference_fps"]["status"])
            self.assertEqual("measured", report["performance"]["four_frequency_coverage_ms"]["status"])
            self.assertEqual(
                1.0,
                report["performance"]["previous_ack_to_next_iqsc_start_ms"]["p50"],
            )
            self.assertEqual(
                -151.0,
                report["performance"]["next_request_relative_to_previous_ack_ms"]["p50"],
            )
            self.assertEqual(9.4, report["performance"]["sdr_mmap_dma_wait_ms"]["p50"])
            self.assertEqual(0.209, report["performance"]["sdr_mmap_copy_ms"]["p50"])

    def test_full_trace_ring_keeps_complete_selected_interval(self):
        baseline = 11_010
        records = [
            trace_record(index, center_index=index % 4, sequence=11_030 + index * 17)
            for index in range(40)
        ]
        before = trace_document(
            [], records_started=512, records_overwritten=384, capacity=128,
            latest_sequence=baseline,
        )
        after = trace_document(
            records, records_started=552, records_overwritten=424, capacity=128,
            latest_sequence=11_870,
        )
        report = report_document()
        selected = campaign.trace_records(after, baseline)
        campaign.validate_trace_interval(report, before, after, selected, 40)

        self.assertEqual([], report["failures"])
        self.assertTrue(report["trace_interval"]["recoverable"])
        self.assertEqual(40, report["trace_interval"]["records_started_delta"])
        self.assertEqual(40, report["trace_interval"]["overwrite_delta"])
        self.assertTrue(any("historical" in warning for warning in report["warnings"]))

    def test_trace_interval_larger_than_ring_capacity_is_rejected(self):
        baseline = 4_000
        records = [trace_record(index, sequence=4_100 + index) for index in range(128)]
        before = trace_document([], records_started=512, capacity=128, latest_sequence=baseline)
        after = trace_document(
            records, records_started=641, capacity=128, latest_sequence=4_300,
        )
        report = report_document()
        campaign.validate_trace_interval(
            report, before, after, campaign.trace_records(after, baseline), 129,
        )

        self.assertTrue(any("exceeding ring capacity" in item for item in report["failures"]))
        self.assertFalse(report["trace_interval"]["recoverable"])

    def test_pacing_target_is_informational_without_explicit_minimum(self):
        record = trace_record(0)
        record["FirstToLastPacketMs"] = 60.0
        report = report_document()
        spec = campaign.scenario("pacing-only", "rate-step", 800, [0], mode="serial")
        campaign.validate_trace(report, [record], spec)

        self.assertEqual([], report["failures"])
        self.assertEqual(800.0, report["performance"]["iq_payload_mbps"]["pacing_target_mbps"])

    def test_iqsc_start_may_prove_credit_before_control_response(self):
        records = [trace_record(index) for index in range(2)]
        records[0]["CreditAcceptedCycles"] = records[1]["IqscStartCycles"] + 5_000_000
        report = report_document()
        spec = campaign.scenario("credit-proof", "pipeline", 800, [0, 1])

        campaign.validate_trace(report, records, spec)

        self.assertEqual([], report["failures"])
        self.assertEqual(
            -5.0,
            report["performance"]["previous_credit_to_next_iqsc_start_ms"]["p50"],
        )

    def test_explicit_minimum_payload_rate_is_enforced(self):
        record = trace_record(0)
        record["FirstToLastPacketMs"] = 60.0
        report = report_document()
        spec = campaign.scenario(
            "minimum-rate", "rate-step", 800, [0], mode="serial",
            minimum_payload_mbps=500.0,
        )
        campaign.validate_trace(report, [record], spec)

        self.assertTrue(any("below explicit minimum" in item for item in report["failures"]))

    def test_capture_accepted_and_nibble_sender_crc_are_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "agent.log"
            log.write_text(agent_log(receipt="accepted", crc_backend="nibble"), encoding="utf-8")
            report = report_document()
            spec = campaign.scenario("legacy-agent", "rate-step", 800, [0, 1, 2, 3])
            campaign.validate_agent(
                report,
                [trace_record(index) for index in range(4)],
                campaign.parse_agent_log(log),
                spec,
            )

        self.assertEqual([], report["failures"])
        self.assertEqual(["nibble"], report["sdr"]["sender_crc_backends"])
        self.assertEqual("overlap", report["control"]["observed_pipeline_mode"])

    def test_unsupported_sender_crc_backend_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "agent.log"
            log.write_text(agent_log(crc_backend="unknown"), encoding="utf-8")
            report = report_document()
            spec = campaign.scenario("invalid-crc", "rate-step", 800, [0, 1, 2, 3])
            campaign.validate_agent(
                report,
                [trace_record(index) for index in range(4)],
                campaign.parse_agent_log(log),
                spec,
            )

        self.assertTrue(any("unsupported sender CRC backend" in item for item in report["failures"]))

    def test_four_center_round_boundaries_are_prefetched(self):
        centers = [0, 1, 2, 3] * 10
        records = [
            trace_record(index, center_index=center)
            for index, center in enumerate(centers)
        ]
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "agent.log"
            log.write_text(agent_log(centers=centers), encoding="utf-8")
            report = report_document()
            spec = campaign.scenario("four-rounds", "pipeline", 800, centers)
            campaign.validate_agent(report, records, campaign.parse_agent_log(log), spec)

        self.assertEqual([], report["failures"])
        self.assertEqual("overlap", report["control"]["observed_pipeline_mode"])
        self.assertEqual(39, report["control"]["overlap_pairs"])
        self.assertEqual(0, report["control"]["serial_pairs"])

    def test_mismatched_cpu1_campaign_proof_fails(self):
        report = report_document()
        records = [trace_record(index) for index in range(4)]
        spec = campaign.scenario("proof-mismatch", "rate-step", 800, [0, 1, 2, 3])
        campaign.validate_runtime(
            report,
            runtime_document(campaign_fields=False),
            records,
            spec,
            campaign_proof_document(last_session=999, last_center=999),
        )

        self.assertTrue(any("last session" in item for item in report["failures"]))
        self.assertTrue(any("last center" in item for item in report["failures"]))

    def test_verify_requires_after_cpu1_campaign_proof(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_json(
                root / "manifest.json",
                {"scenario": campaign.scenario("missing-proof", "rate-step", 800, [0])},
            )
            for phase in ("before", "after"):
                write_json(root / phase / "net.json", {})
                write_json(root / phase / "trace.json", {})
                write_json(root / phase / "runtime.json", {})
            for name in ("agent.log", "flash.log", "management.log"):
                (root / name).write_text("", encoding="utf-8")

            with self.assertRaisesRegex(campaign.CampaignError, "cpu1_campaign"):
                campaign.verify_scenario(root / "manifest.json")

    def test_cpu1_campaign_hash_is_bound_separately_from_cpu0(self):
        report = {"failures": []}
        manifest = {
            "firmware": {
                "cpu0": {"sha256": CPU0_HASH},
                "cpu1": {"sha256": CPU1_HASH},
            },
        }
        campaign.validate_hashes(
            report,
            manifest,
            [("cpu1_campaign", campaign_proof_document(elf_hash="E" * 64))],
        )

        self.assertEqual(
            ["cpu1_campaign CPU1 ELF hash does not match manifest"], report["failures"],
        )

    def test_after_phase_collects_cpu1_campaign_proof_only(self):
        args = SimpleNamespace(
            probe_serial="123456789",
            jlink_exe=None,
            cpu0_elf="cpu0.elf",
            cpu1_elf="cpu1.elf",
            phase="before",
        )
        before = campaign.collector_commands(args, Path("before"))
        args.phase = "after"
        after = campaign.collector_commands(args, Path("after"))

        self.assertNotIn("cpu1_campaign", before)
        self.assertIn("cpu1_campaign", after)
        command = after["cpu1_campaign"]
        self.assertEqual("ReadStatus", command[command.index("-Action") + 1])
        self.assertIn("-Json", command)


if __name__ == "__main__":
    unittest.main()
