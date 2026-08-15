import ctypes
import re
import unittest
from pathlib import Path

from project_layout import resolve_cpu0, resolve_cpu1


ROOT = Path(__file__).resolve().parents[1]
CPU0 = resolve_cpu0(ROOT)
CPU1 = resolve_cpu1(ROOT)
HEADER = CPU1 / "src" / "framework" / "campaign_control.h"
SOURCE = HEADER.with_suffix(".c")
DISPLAY = HEADER.with_name("display_app.c")
DISPLAY_HEADER = HEADER.with_name("display_app.h")
PROBE_TOOL = ROOT / "tools/ra8p1-cpu1-campaign.ps1"
SDR_CONTROL = CPU0 / "src" / "framework" / "sdr_control_client.c"
SDR_CONTROL_TEST = ROOT / "tools" / "test_sdr_control_client.c"


def c_function_body(text, signature):
    """Return a C function body while skipping an earlier prototype."""
    cursor = 0
    while True:
        start = text.index(signature, cursor)
        opening = text.find("{", start)
        semicolon = text.find(";", start)
        if opening != -1 and (semicolon == -1 or opening < semicolon):
            break
        cursor = semicolon + 1

    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise AssertionError(f"unterminated function body: {signature}")


class CampaignRequest(ctypes.LittleEndianStructure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("request_id", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("center_index", ctypes.c_uint32),
        ("iterations", ctypes.c_uint32),
        ("target_payload_mbps_x1000", ctypes.c_uint32),
        ("test_fault_flags", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 5),
        ("end_sequence", ctypes.c_uint32),
    ]


class CampaignProof(ctypes.LittleEndianStructure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("request_id", ctypes.c_uint32),
        ("request_begin_sequence", ctypes.c_uint32),
        ("state", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("configured_center_index", ctypes.c_uint32),
        ("iterations_requested", ctypes.c_uint32),
        ("iterations_completed", ctypes.c_uint32),
        ("windows_expected", ctypes.c_uint32),
        ("windows_visible", ctypes.c_uint32),
        ("next_center_index", ctypes.c_uint32),
        ("active_center_index", ctypes.c_uint32),
        ("target_payload_mbps_x1000", ctypes.c_uint32),
        ("test_fault_flags", ctypes.c_uint32),
        ("campaign_flags", ctypes.c_uint32),
        ("last_session_id", ctypes.c_uint32),
        ("last_window_sequence", ctypes.c_uint32),
        ("last_result_center_index", ctypes.c_uint32),
        ("last_command_sequence", ctypes.c_uint32),
        ("last_command_status", ctypes.c_uint32),
        ("last_command_reason", ctypes.c_uint32),
        ("last_applied_session_id", ctypes.c_uint32),
        ("command_send_retries", ctypes.c_uint32),
        ("busy_retries", ctypes.c_uint32),
        ("rejected_requests", ctypes.c_uint32),
        ("duplicate_requests", ctypes.c_uint32),
        ("unexpected_results", ctypes.c_uint32),
        ("last_error", ctypes.c_uint32),
        ("terminal_magic", ctypes.c_uint32),
        ("end_sequence", ctypes.c_uint32),
    ]


class Cpu1CampaignControlTests(unittest.TestCase):
    def test_fixed_abi_sizes_and_offsets(self):
        self.assertEqual(64, ctypes.sizeof(CampaignRequest))
        self.assertEqual(60, CampaignRequest.end_sequence.offset)
        self.assertEqual(128, ctypes.sizeof(CampaignProof))
        self.assertEqual(120, CampaignProof.terminal_magic.offset)
        self.assertEqual(124, CampaignProof.end_sequence.offset)

    def test_header_locks_magic_version_size_and_modes(self):
        text = HEADER.read_text(encoding="utf-8")
        for fragment in (
            "RA8P1_CPU1_CAMPAIGN_REQUEST_MAGIC       (0x51525043UL)",
            "RA8P1_CPU1_CAMPAIGN_PROOF_MAGIC         (0x46525043UL)",
            "RA8P1_CPU1_CAMPAIGN_VERSION             (1U)",
            "RA8P1_CPU1_CAMPAIGN_REQUEST_BYTES       (64U)",
            "RA8P1_CPU1_CAMPAIGN_PROOF_BYTES         (128U)",
            "RA8P1_CPU1_CAMPAIGN_MODE_STOP = 1U",
            "RA8P1_CPU1_CAMPAIGN_MODE_SINGLE = 2U",
            "RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP = 3U",
            "RA8P1_CPU1_CAMPAIGN_MODE_FOUR_SERIAL = 4U",
        ):
            self.assertIn(fragment, text)

    def test_result_ack_follows_owned_copy_but_precedes_lvgl_rendering(self):
        text = DISPLAY.read_text(encoding="utf-8")
        step = c_function_body(text, "void display_app_step(void)")
        poll = step.index("ipc_bridge_cpu1_display_poll(&display_frame)")
        semantic = step.index(
            "display_app_frame_semantically_valid(&display_frame)", poll
        )
        owned_copy = step.index("ui_model_update_frame(&display_frame)", semantic)
        visible = step.index(
            "ipc_bridge_cpu1_display_visible(&display_frame)", owned_copy
        )
        advance = step.index(
            "cpu1_campaign_result_visible(&display_frame)", visible
        )
        lvgl = step.index("lvgl_app_signal_update(&display_frame)", advance)

        self.assertLess(poll, semantic)
        self.assertLess(semantic, owned_copy)
        self.assertLess(owned_copy, visible)
        self.assertLess(visible, advance)
        self.assertLess(advance, lvgl)
        self.assertNotIn("lvgl_app_frame_presented", step)

    def test_panel_presentation_remains_independently_vsync_qualified(self):
        display = DISPLAY.read_text(encoding="utf-8")
        lvgl = (CPU1 / "src" / "lvgl_app.c").read_text(encoding="utf-8")
        service = c_function_body(
            display, "static void display_app_panel_presentation_service("
        )
        self.assertIn("lvgl_app_frame_presented(frame_probe)", service)
        self.assertNotIn("ipc_bridge_cpu1_display_visible", service)
        self.assertIn("bool lvgl_app_frame_presented", lvgl)
        self.assertLess(lvgl.index("R_GLCDC_BufferChange"),
                        lvgl.index("g_visibility_vsync_pending = true"))
        self.assertLess(lvgl.index("g_visibility_vsync_pending = true"),
                        lvgl.index("g_visibility_frame_presented = true"))
        self.assertIn("ui_visibility_render_required()", lvgl)

    def test_new_capture_blocks_the_pre_stop_baseline_session(self):
        display = DISPLAY.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("display_app_last_visible_session_id", display)
        self.assertIn(
            "g_campaign.baseline_session_id =\n"
            "        display_app_last_visible_session_id();",
            source,
        )
        self.assertIn("g_campaign.awaiting_new_session", source)
        self.assertIn(
            "frame->session_id == g_campaign.baseline_session_id", source
        )

    def test_cpu1_uses_normal_ipc_command_api(self):
        display = DISPLAY.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("ipc_bridge_cpu1_command_send(&command)", display)
        self.assertIn("display_app_campaign_command_start(", source)
        self.assertIn("display_app_campaign_command_stop()", source)
        self.assertNotIn("RA8P1_COMMAND_MAILBOX", source)

    def test_overlap_and_serial_ownership_are_separate(self):
        source = SOURCE.read_text(encoding="utf-8")
        issue = c_function_body(source, "static void campaign_issue_capture(")
        visible = c_function_body(
            source, "void cpu1_campaign_result_visible("
        )
        self.assertRegex(
            source,
            re.compile(
                r"mode == RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP.*?"
                r"display_app_campaign_command_start",
                re.DOTALL,
            ),
        )
        self.assertIn("CPU1 only advances proof state", source)
        self.assertIn("g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT", source)
        self.assertIn("scan_all,\n            scan_all,", issue)
        self.assertIn("center 3 -> 0 boundary", visible)
        self.assertIn(
            "g_campaign.expected_center_index = (center_index + 1U) %",
            visible,
        )
        overlap = visible.index(
            "RA8P1_CPU1_CAMPAIGN_MODE_FOUR_OVERLAP"
        )
        keep_running = visible.index("return;", overlap)
        reissue = visible.index("g_campaign.command_issued = false;", overlap)
        self.assertLess(keep_running, reissue)

    def test_live_recovery_stops_then_waits_then_backs_off(self):
        display = DISPLAY.read_text(encoding="utf-8")
        service = c_function_body(
            display, "static void display_app_live_retry_service(void)"
        )
        for state in (
            "DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED",
            "DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED",
            "DISPLAY_APP_LIVE_RECOVERY_BACKOFF",
        ):
            self.assertIn(state, display)

        stop_required = service.index(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;"
        )
        send_stop = service.index(
            "if (display_app_send_stop_command())", stop_required
        )
        wait_stopped = service.index(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED;",
            send_stop,
        )
        sequence_gate = service.index(
            "g_last_command_sequence != g_live_recovery_sequence",
            wait_stopped,
        )
        applied = service.index(
            "g_last_command_status == RA8P1_COMMAND_APPLIED", sequence_gate
        )
        stopped = service.index(
            "g_last_command_reason == RA8P1_COMMAND_REASON_STOPPED", applied
        )
        delay = service.index(
            "g_live_retry_delay_steps = DISPLAY_APP_LIVE_RETRY_DELAY_STEPS;",
            stopped,
        )
        backoff = service.index(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_BACKOFF;",
            delay,
        )
        restart = service.index("display_app_resend_live_start", backoff)

        self.assertLess(stop_required, send_stop)
        self.assertLess(send_stop, wait_stopped)
        self.assertLess(wait_stopped, sequence_gate)
        self.assertLess(sequence_gate, applied)
        self.assertLess(applied, stopped)
        self.assertLess(stopped, delay)
        self.assertLess(delay, backoff)
        self.assertLess(backoff, restart)
        self.assertNotIn("display_app_resend_live_start", service[stop_required:backoff])

    def test_live_progress_watchdog_uses_ordered_recovery(self):
        display = DISPLAY.read_text(encoding="utf-8")
        step = c_function_body(display, "void display_app_step(void)")
        watchdog = c_function_body(
            display, "static void display_app_live_progress_service("
        )

        self.assertIn(
            "#define DISPLAY_APP_LIVE_STALL_LINE_EVENTS     (90U)", display
        )
        valid = step.index("display_frame_valid = display_frame_ready")
        consume = step.index("if (display_frame_valid)", valid)
        service = step.index(
            "display_app_live_progress_service(display_frame_valid);", consume
        )
        self.assertLess(valid, consume)
        self.assertLess(consume, service)
        self.assertIn("g_display_diag.glcdc_line_events", watchdog)
        self.assertIn("g_last_command_status != RA8P1_COMMAND_APPLIED", watchdog)
        self.assertIn("DISPLAY_APP_LIVE_STALL_LINE_EVENTS", watchdog)
        self.assertIn("g_display_app_stall_recoveries++;", watchdog)
        self.assertIn(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;",
            watchdog,
        )
        self.assertNotIn("display_app_resend_live_start", watchdog)

    def test_focus_request_queues_stop_before_continuous_single_start(self):
        display = DISPLAY.read_text(encoding="utf-8")
        header = DISPLAY_HEADER.read_text(encoding="utf-8")
        focus = c_function_body(display, "bool display_app_request_focus(")
        prepare = c_function_body(
            display, "static bool display_app_prepare_capture_command("
        )

        self.assertIn("bool display_app_request_focus(uint32_t center_index);", header)
        self.assertIn("cpu1_campaign_owns_scheduler()", focus)
        self.assertIn("g_live_start_command = command;", focus)
        self.assertIn("g_live_start_command_valid = true;", focus)
        self.assertIn(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;",
            focus,
        )
        self.assertNotIn("ipc_bridge_cpu1_command_send", focus)
        self.assertNotIn("display_app_submit_capture", focus)

        scan_all = prepare.index("if (scan_all)")
        scan_all_flag = prepare.index(
            "command->flags |= RA8P1_COMMAND_FLAG_SCAN_ALL;", scan_all
        )
        continuous = prepare.index("if (continuous_scan)", scan_all_flag)
        continuous_flag = prepare.index(
            "command->flags |= RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS;",
            continuous,
        )
        self.assertLess(scan_all_flag, continuous)
        self.assertLess(continuous, continuous_flag)

    def test_cpu0_dispatches_continuous_single_without_busy_reentry(self):
        pipeline = (CPU0 / "src/framework/rf_pipeline.c").read_text(
            encoding="utf-8"
        )
        client = SDR_CONTROL.read_text(encoding="utf-8")
        poll = c_function_body(pipeline, "static void rf_pipeline_poll_command(void)")
        validate = c_function_body(
            pipeline, "static bool rf_pipeline_command_valid("
        )
        start = c_function_body(client, "static bool sdr_control_start(")
        repeat = c_function_body(
            client, "static void sdr_control_credit_accepted("
        )
        next_prefetch = c_function_body(
            client, "static bool sdr_control_next_prefetch_center("
        )

        self.assertIn("sdr_control_client_start_continuous_single(", poll)
        self.assertNotIn(
            "RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS) != 0U) &&\n"
            "             ((command->flags & RA8P1_COMMAND_FLAG_SCAN_ALL) == 0U)",
            validate,
        )
        self.assertIn("sdr_control_state_active(client->stats.state)", start)
        self.assertIn("client->repeat_scan = repeat_scan ? 1U : 0U;", start)
        self.assertIn("client->current_center_index", repeat)
        self.assertIn("sdr_control_make_capture_request(client, next_center);", repeat)
        self.assertIn("if (client->repeat_scan == 0U)", next_prefetch)
        self.assertIn("*center_index = 0U;", next_prefetch)

    def test_rejected_recovery_stop_cannot_fall_through_to_start(self):
        display = DISPLAY.read_text(encoding="utf-8")
        service = c_function_body(
            display, "static void display_app_live_retry_service(void)"
        )
        wait_stopped = service.index(
            "g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED"
        )
        rejected_stop = service.index(
            "else if (g_last_command_status == RA8P1_COMMAND_REJECTED)",
            wait_stopped,
        )
        stop_required = service.index(
            "DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED", rejected_stop
        )
        backoff_branch = service.index(
            "g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_BACKOFF",
            stop_required,
        )
        self.assertLess(rejected_stop, stop_required)
        self.assertLess(stop_required, backoff_branch)
        self.assertNotIn(
            "display_app_resend_live_start", service[rejected_stop:backoff_branch]
        )

    def test_live_restart_preserves_the_complete_start_command(self):
        display = DISPLAY.read_text(encoding="utf-8")
        submit = c_function_body(
            display, "static bool display_app_submit_capture("
        )
        resend = c_function_body(
            display, "static bool display_app_resend_live_start(void)"
        )
        takeover = c_function_body(
            display, "void display_app_campaign_takeover(void)"
        )

        saved = submit.index("g_live_start_command = command;")
        valid = submit.index("g_live_start_command_valid = true;", saved)
        self.assertLess(saved, valid)

        copied = resend.index("command = g_live_start_command;")
        sent = resend.index("ipc_bridge_cpu1_command_send(&command)", copied)
        mutations = re.findall(r"command\.(\w+)\s*=", resend[copied:sent])
        self.assertEqual(["sequence"], mutations)
        self.assertLess(
            resend.index("command.sequence = g_ui_command_sequence;", copied),
            sent,
        )
        self.assertGreater(
            resend.index("g_live_command_sequence = command.sequence;", sent),
            sent,
        )
        self.assertIn("g_live_start_command_valid = false;", takeover)

    def test_campaign_transient_retry_stops_before_backoff_and_start(self):
        source = SOURCE.read_text(encoding="utf-8")
        schedule = c_function_body(
            source, "static void campaign_schedule_retry(void)"
        )
        begin_stop = c_function_body(
            source, "static void campaign_begin_stop(uint32_t terminal_state)"
        )
        service = c_function_body(source, "void cpu1_campaign_service(")

        self.assertIn(
            "campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT);",
            schedule,
        )
        self.assertNotIn("campaign_issue_capture", schedule)
        self.assertNotIn(
            "g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT", schedule
        )
        self.assertIn(
            "g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_STOPPING;",
            begin_stop,
        )

        schedule_retry = service.index("campaign_schedule_retry();")
        stopping = service.index(
            "g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPING",
            schedule_retry,
        )
        sequence_gate = service.index(
            "command_sequence == g_campaign.current_command_sequence", stopping
        )
        applied = service.index(
            "command_status == RA8P1_COMMAND_APPLIED", sequence_gate
        )
        stopped = service.index(
            "command_reason == RA8P1_COMMAND_REASON_STOPPED", applied
        )
        terminal_transition = service.index(
            "g_campaign.state = g_campaign.terminal_state_after_stop;", stopped
        )
        retry_wait = service.index(
            "g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT",
            terminal_transition,
        )
        decrement = service.index("g_campaign.retry_delay_steps--;", retry_wait)
        arming = service.index(
            "g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_ARMING;", decrement
        )
        start = service.index("campaign_issue_capture(command_pending);", arming)

        self.assertLess(stopping, sequence_gate)
        self.assertLess(sequence_gate, applied)
        self.assertLess(applied, stopped)
        self.assertLess(stopped, terminal_transition)
        self.assertLess(terminal_transition, retry_wait)
        self.assertLess(retry_wait, decrement)
        self.assertLess(decrement, arming)
        self.assertLess(arming, start)

    def test_campaign_stop_rejection_has_a_bounded_error_exit(self):
        source = SOURCE.read_text(encoding="utf-8")
        service = c_function_body(source, "void cpu1_campaign_service(")
        rejected = service.index(
            "command_status == RA8P1_COMMAND_REJECTED"
        )
        stopping = service.index(
            "if (g_campaign.state == RA8P1_CPU1_CAMPAIGN_STATE_STOPPING)",
            rejected,
        )
        non_stop = service.index(
            "else if (campaign_transient_rejection(command_reason)",
            stopping,
        )
        stop_branch = service[stopping:non_stop]

        self.assertIn("CPU1_CAMPAIGN_MAX_COMMAND_RETRIES", stop_branch)
        self.assertIn(
            "g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_ERROR;",
            stop_branch,
        )
        self.assertIn(
            "RA8P1_CPU1_CAMPAIGN_ERROR_COMMAND_REJECTED;",
            stop_branch,
        )
        self.assertNotIn("campaign_fail(", stop_branch)

    def test_completed_campaign_stops_before_terminal_complete(self):
        source = SOURCE.read_text(encoding="utf-8")
        result_visible = c_function_body(
            source, "void cpu1_campaign_result_visible("
        )
        complete_gate = result_visible.index(
            "g_campaign.windows_visible >= g_campaign.windows_expected"
        )
        begin_stop = result_visible.index(
            "campaign_begin_stop(RA8P1_CPU1_CAMPAIGN_STATE_COMPLETE);",
            complete_gate,
        )
        publish = result_visible.index("campaign_publish();", begin_stop)

        self.assertLess(complete_gate, begin_stop)
        self.assertLess(begin_stop, publish)
        self.assertNotIn(
            "g_campaign.state = RA8P1_CPU1_CAMPAIGN_STATE_COMPLETE;",
            result_visible[complete_gate:publish],
        )

    def test_explicit_stop_takeover_cancels_pending_live_restart(self):
        source = SOURCE.read_text(encoding="utf-8")
        display = DISPLAY.read_text(encoding="utf-8")
        accept = c_function_body(source, "static void campaign_accept_request(")
        takeover = c_function_body(
            display, "void display_app_campaign_takeover(void)"
        )

        self.assertLess(
            accept.index("display_app_campaign_takeover();"),
            accept.index("campaign_begin_stop("),
        )
        self.assertRegex(
            accept,
            re.compile(
                r"request->mode == RA8P1_CPU1_CAMPAIGN_MODE_STOP\).*?"
                r"RA8P1_CPU1_CAMPAIGN_STATE_STOPPED",
                re.DOTALL,
            ),
        )
        for cancellation in (
            "g_live_command_sequence = 0U;",
            "g_live_recovery_sequence = 0U;",
            "g_live_retry_delay_steps = 0U;",
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;",
            "g_live_start_command_valid = false;",
        ):
            self.assertIn(cancellation, takeover)

    def test_error_state_stays_quarantined_until_cancel(self):
        control = SDR_CONTROL.read_text(encoding="utf-8")
        control_test = SDR_CONTROL_TEST.read_text(encoding="utf-8")
        display = DISPLAY.read_text(encoding="utf-8")
        start = c_function_body(control, "static bool sdr_control_start(")
        quarantine_test = c_function_body(
            control_test, "static void test_error_requires_cancel_before_restart(void)"
        )
        transient = c_function_body(
            display, "static bool display_app_live_rejection_is_transient("
        )
        live_service = c_function_body(
            display, "static void display_app_live_retry_service(void)"
        )

        self.assertIn(
            "client->stats.state == SDR_CONTROL_CLIENT_ERROR", start
        )
        self.assertNotIn("RA8P1_COMMAND_REASON_SDR_CONTROL_ERROR", transient)
        non_transient = live_service.index(
            "if (!display_app_live_rejection_is_transient(g_last_command_reason))"
        )
        invalidate = live_service.index(
            "g_live_start_command_valid = false;", non_transient
        )
        non_transient_return = live_service.index("return;", invalidate)
        stop_recovery = live_service.index(
            "g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;",
            non_transient_return,
        )
        self.assertLess(non_transient, invalidate)
        self.assertLess(invalidate, non_transient_return)
        self.assertLess(non_transient_return, stop_recovery)
        rejected_restart = quarantine_test.index(
            "assert(!sdr_control_client_start_single"
        )
        cancel = quarantine_test.index(
            "assert(sdr_control_client_cancel", rejected_restart
        )
        accepted_restart = quarantine_test.index(
            "assert(sdr_control_client_start_single", cancel
        )
        self.assertLess(rejected_restart, cancel)
        self.assertLess(cancel, accepted_restart)

    def test_active_campaign_rejects_foreign_non_stop_request(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("static bool campaign_owns_active_scheduler(void)", source)
        for state in (
            "RA8P1_CPU1_CAMPAIGN_STATE_STOPPING",
            "RA8P1_CPU1_CAMPAIGN_STATE_ARMING",
            "RA8P1_CPU1_CAMPAIGN_STATE_RUNNING",
            "RA8P1_CPU1_CAMPAIGN_STATE_RETRY_WAIT",
        ):
            self.assertIn(state, source)
        guard = re.compile(
            r"error = campaign_request_error\(&request, &windows_expected\);.*?"
            r"if \(campaign_owns_active_scheduler\(\) &&.*?"
            r"request\.mode != RA8P1_CPU1_CAMPAIGN_MODE_STOP.*?"
            r"g_campaign\.rejected_requests\+\+;.*?"
            r"campaign_publish\(\);.*?return;",
            re.DOTALL,
        )
        self.assertRegex(source, guard)
        self.assertRegex(
            source,
            re.compile(
                r"if \(!campaign_owns_active_scheduler\(\)\).*?"
                r"RA8P1_CPU1_CAMPAIGN_ERROR_REQUEST_ID_REUSED",
                re.DOTALL,
            ),
        )

    def test_probe_tool_never_attaches_cpu1_or_writes_cpu0_mailbox(self):
        text = PROBE_TOOL.read_text(encoding="utf-8")
        self.assertIn("$script:ExpectedProbe = '1082495494'", text)
        self.assertIn("$script:Cpu0Target = 'R7KA8P1KF_CPU0'", text)
        self.assertNotIn("'-Device', 'R7KA8P1KF_CPU1'", text)
        self.assertIn("g_cpu1_campaign_control", text)
        self.assertIn("g_cpu1_campaign_proof", text)
        self.assertNotIn("RA8P1_COMMAND_MAILBOX", text)
        self.assertIn("Automatic latest-ELF selection is intentionally disabled", text)
        self.assertIn("Address -band 31", text)
        self.assertIn("Type -notmatch '^[BbDd]$'", text)

    def test_probe_tool_commits_seqlock_end_then_begin(self):
        text = PROBE_TOOL.read_text(encoding="utf-8")
        end_write = text.index("(Add-Address $base 60)")
        begin_write = text.index(
            "(Format-Hex32 $base),\n                                                "
            "(Format-Hex32 $even)))",
            end_write,
        )
        self.assertLess(end_write, begin_write)


if __name__ == "__main__":
    unittest.main()
