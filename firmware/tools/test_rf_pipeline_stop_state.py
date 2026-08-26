#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RF_PIPELINE = (ROOT / "cpu0/src/framework/rf_pipeline.c").read_text(
    encoding="utf-8"
)
SDR_CLIENT = (ROOT / "cpu0/src/framework/sdr_control_client.c").read_text(
    encoding="utf-8"
)


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class RfPipelineStopStateTests(unittest.TestCase):
    def test_stop_withdraws_expected_session_before_quiescing_ingress(self) -> None:
        stop = between(
            RF_PIPELINE,
            "if (command.action == RA8P1_COMMAND_ACTION_STOP)",
            "g_requested_sample_rate_hz = command.requested_sample_rate_hz;",
        )
        cancel = stop.index("sdr_control_client_cancel")
        withdraw = stop.index("rf_pipeline_sdr_expected_sync();", cancel)
        quiesce = stop.index("rf_pipeline_stop_stream_local();", withdraw)
        self.assertLess(cancel, withdraw)
        self.assertLess(withdraw, quiesce)

        expected = between(
            SDR_CLIENT,
            "const ra8p1_sdr_control_message_t *sdr_control_client_expected_request",
            "uint32_t sdr_control_client_expected_session",
        )
        self.assertIn("SDR_CONTROL_CLIENT_WAIT_TERMINAL_CANCELLED", expected)
        self.assertIn("return NULL;", expected)

    def test_local_stop_invalidates_every_stream_owner(self) -> None:
        quiesce = between(
            RF_PIPELINE,
            "static void rf_pipeline_stop_stream_local(void)",
            "static bool rf_pipeline_command_valid",
        )
        for statement in (
            "g_stream_rx_active = 0U;",
            "g_stream_config_pending = 0U;",
            "g_stream_end_pending = 0U;",
            "g_stream_rx_session_id = 0U;",
            "g_stream_active_session_id = 0U;",
            "g_stream_valid = 0U;",
            "g_applied_session_id = 0U;",
            "memset(&g_pending_stream_config, 0, sizeof(g_pending_stream_config));",
            "analysis_pipeline_abort_stream();",
        ):
            self.assertIn(statement, quiesce)

    def test_stop_is_pending_until_terminal_cancel_completes(self) -> None:
        service = between(
            RF_PIPELINE,
            "static void rf_pipeline_sdr_control_service(void)",
            "static void rf_pipeline_stop_stream_local(void)",
        )
        owned = between(
            service,
            "if ((g_stop_command_sequence != 0U)",
            "if ((g_stop_command_sequence != 0U)",
        )
        self.assertIn("SDR_CONTROL_CLIENT_CANCELLED", owned)
        self.assertIn("RA8P1_COMMAND_APPLIED", owned)
        self.assertIn("SDR_CONTROL_CLIENT_ERROR", owned)
        self.assertIn("RA8P1_COMMAND_REJECTED", owned)
        self.assertIn("RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY", owned)
        self.assertIn("RA8P1_COMMAND_REASON_STOPPED", owned)

        stop = between(
            RF_PIPELINE,
            "if (command.action == RA8P1_COMMAND_ACTION_STOP)",
            "g_requested_sample_rate_hz = command.requested_sample_rate_hz;",
        )
        self.assertIn("if (cancel_required)", stop)
        self.assertIn("g_stop_command_sequence = command.sequence;", stop)
        self.assertIn("RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY", stop)

    def test_late_control_result_cannot_overwrite_a_newer_command(self) -> None:
        self.assertIn("static volatile uint32_t g_start_command_sequence;", RF_PIPELINE)
        self.assertIn("static volatile uint32_t g_stop_command_sequence;", RF_PIPELINE)
        self.assertIn(
            "(g_command_sequence == g_start_command_sequence)", RF_PIPELINE
        )
        service = between(
            RF_PIPELINE,
            "static void rf_pipeline_sdr_control_service(void)",
            "static void rf_pipeline_stop_stream_local(void)",
        )
        self.assertIn("g_command_sequence == g_stop_command_sequence", service)
        self.assertIn("A newer mailbox command owns telemetry", service)


if __name__ == "__main__":
    unittest.main()
