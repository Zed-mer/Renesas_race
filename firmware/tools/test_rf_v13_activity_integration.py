#!/usr/bin/env python3
"""Static checks for the V25 core behind the preserved V27 service ABI."""

from __future__ import annotations

import ctypes
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RESOURCE_HEADER = ROOT / "shared" / "resource_layout.h"
MAILBOX_HEADER = ROOT / "shared" / "activity_mailbox.h"
FUSION_HEADER = ROOT / "shared" / "rf_v13_activity_fusion.h"
V25_SOURCE = ROOT / "cpu1" / "src" / "framework" / "rf_v25_activity_fusion.c"
V25_CONFIG = ROOT / "cpu1" / "src" / "framework" / "rf_v25_activity_calibration.c"
V25_HEADER = ROOT / "cpu1" / "src" / "framework" / "rf_v25_activity_fusion.h"
SERVICE_HEADER = ROOT / "cpu1" / "src" / "framework" / "activity_service.h"
SERVICE_SOURCE = ROOT / "cpu1" / "src" / "framework" / "activity_service.c"
DISPLAY_SOURCE = ROOT / "cpu1" / "src" / "framework" / "display_app.c"
ROUND_BUILDER_SOURCE = (
    ROOT / "cpu0" / "src" / "framework" / "rf_v13_round_builder.c"
)
ANALYSIS_SOURCE = ROOT / "cpu0" / "src" / "framework" / "analysis_pipeline.c"
PREPROCESS_SOURCE = ROOT / "cpu0" / "src" / "framework" / "rf_v12_preprocess.c"
NPU_MODEL_SOURCE = ROOT / "cpu0" / "src" / "framework" / "npu_model" / "iq_npu_model.c"
V31_SCHEDULE_SOURCE = ROOT / "cpu0" / "src" / "framework" / "rf_v31_model_schedule.c"
LVGL_SOURCE = ROOT / "cpu1" / "src" / "lvgl_app.c"
RF_UI_SOURCE = ROOT / "cpu1" / "src" / "ui" / "rf_ui.c"
CPU0_IPC_SOURCE = ROOT / "cpu0" / "src" / "framework" / "ipc_bridge.c"
CPU1_IPC_SOURCE = ROOT / "cpu1" / "src" / "framework" / "ipc_bridge.c"


def numeric_macro(path: pathlib.Path, name: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+\(?\s*"
        r"(0x[0-9A-Fa-f]+|[0-9]+)(?:ULL|UL|U)?\s*\)?",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"numeric macro {name} not found in {path}")
    return int(match.group(1), 0)


class ActivityCpu0State(ctypes.LittleEndianStructure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("boot_epoch", ctypes.c_uint32),
        ("begin_sequence", ctypes.c_uint32),
        ("end_sequence", ctypes.c_uint32),
        ("message_sequence", ctypes.c_uint32),
        ("publish_drops", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
    ]


class ActivityCpu1State(ctypes.LittleEndianStructure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("boot_epoch", ctypes.c_uint32),
        ("observed_cpu0_epoch", ctypes.c_uint32),
        ("acknowledged_message_sequence", ctypes.c_uint32),
        ("protocol_errors", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class ActivityControl(ctypes.LittleEndianStructure):
    _fields_ = [("cpu0", ActivityCpu0State), ("cpu1", ActivityCpu1State)]


class V13Evidence(ctypes.LittleEndianStructure):
    _fields_ = [
        ("detection_time_us", ctypes.c_uint64),
        ("confidence_q15", ctypes.c_uint16),
        ("period_bonus_q12", ctypes.c_int16),
        ("class_id", ctypes.c_uint8),
        ("center_slot", ctypes.c_uint8),
        ("roi_decision", ctypes.c_uint8),
        ("evidence_flags", ctypes.c_uint8),
    ]


class V13RoundMessage(ctypes.LittleEndianStructure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("abi_major", ctypes.c_uint16),
        ("abi_minor", ctypes.c_uint16),
        ("message_bytes", ctypes.c_uint16),
        ("evidence_count", ctypes.c_uint16),
        ("message_sequence", ctypes.c_uint32),
        ("round_index", ctypes.c_uint32),
        ("first_v12_tile_sequence", ctypes.c_uint32),
        ("last_v12_tile_sequence", ctypes.c_uint32),
        ("source_v12_abi_major", ctypes.c_uint16),
        ("source_v12_tile_bytes", ctypes.c_uint16),
        ("round_start_time_us", ctypes.c_uint64),
        ("round_end_time_us", ctypes.c_uint64),
        ("invalid_reason_flags", ctypes.c_uint32),
        ("expected_slot_mask", ctypes.c_uint8),
        ("observed_slot_mask", ctypes.c_uint8),
        ("valid_slot_mask", ctypes.c_uint8),
        ("round_flags", ctypes.c_uint8),
        ("evidence", V13Evidence * 16),
        ("display_session_id", ctypes.c_uint32 * 4),
        ("display_window_sequence", ctypes.c_uint32 * 4),
        ("display_identity_mask", ctypes.c_uint8),
        ("display_identity_conflict_mask", ctypes.c_uint8),
        ("reserved_identity", ctypes.c_uint8 * 2),
        ("reserved", ctypes.c_uint8 * 164),
    ]


class V25ActivityIntegrationTests(unittest.TestCase):
    def test_round_message_carries_named_display_identity_without_growing(self) -> None:
        source = FUSION_HEADER.read_text(encoding="utf-8")
        self.assertIn("RF_V13_ACTIVITY_ABI_MINOR UINT16_C(1)", source)
        self.assertEqual(512, ctypes.sizeof(V13RoundMessage))
        self.assertEqual(56, V13RoundMessage.evidence.offset)
        self.assertEqual(312, V13RoundMessage.display_session_id.offset)
        self.assertEqual(328, V13RoundMessage.display_window_sequence.offset)
        self.assertEqual(344, V13RoundMessage.display_identity_mask.offset)
        self.assertIn("display_identity_conflict_mask", source)

    def test_cpu0_binds_round_identity_to_the_same_display_window(self) -> None:
        builder = ROUND_BUILDER_SOURCE.read_text(encoding="utf-8")
        analysis = ANALYSIS_SOURCE.read_text(encoding="utf-8")
        self.assertIn("rf_v18_round_record_display_identity", builder)
        self.assertIn("display_identity_conflict_mask", builder)
        submit = analysis.split(
            "rf_v13_round_builder_submit_processed(", 1
        )[1].split(");", 1)[0]
        self.assertIn("detector_result.state_confidence_q15", submit)
        self.assertIn("detector_result.state_roi_decision", submit)
        self.assertIn("detector_result.state_quality_tier", submit)
        self.assertIn("g_analysis.session_id", submit)
        self.assertIn("lane->tile_index", submit)

    def test_cpu0_uses_v31_q10_without_absolute_auxiliary_inference(self) -> None:
        preprocess = PREPROCESS_SOURCE.read_text(encoding="utf-8")
        analysis = ANALYSIS_SOURCE.read_text(encoding="utf-8")
        model = NPU_MODEL_SOURCE.read_text(encoding="utf-8")
        schedule = V31_SCHEDULE_SOURCE.read_text(encoding="utf-8")

        finalize = preprocess.split("rf_v12_preprocess_finalize(", 1)[1].split(
            "bool rf_v12_preprocess_finalize_synthetic", 1
        )[0]
        self.assertIn("rf_v26_build_input", finalize)
        self.assertIn("info.background_generation = 0U", finalize)
        self.assertNotIn("RF_V12_PREPROCESS_BACKGROUND_NOT_READY", finalize)
        self.assertIn("rf_v31_run_selected_models", model)
        self.assertEqual(4, schedule.count("status = run_one("))
        self.assertNotIn("g_rf_v27_absolute_model", model)
        self.assertNotIn("submit_processed_with_v27_aux", analysis)

    def test_round_builder_consumes_only_final_detector_evidence(self) -> None:
        analysis = ANALYSIS_SOURCE.read_text(encoding="utf-8")
        submit = analysis.split("rf_v13_round_builder_submit_processed(", 1)[1].split(
            ");", 1
        )[0]
        self.assertIn("detector_result.state_confidence_q15", submit)
        self.assertIn("detector_result.state_roi_decision", submit)
        self.assertIn("detector_result.state_quality_tier", submit)
        self.assertNotIn("v27_aux", submit)

    def test_cpu1_publishes_one_shot_round_decisions_to_the_ui_owner(self) -> None:
        service = SERVICE_SOURCE.read_text(encoding="utf-8")
        display = DISPLAY_SOURCE.read_text(encoding="utf-8")
        lvgl = LVGL_SOURCE.read_text(encoding="utf-8")
        rf_ui = RF_UI_SOURCE.read_text(encoding="utf-8")
        self.assertIn("rf_v27_activity_service_take_round_decision", service)
        self.assertIn("RF_V27_ROUND_DECISION_OUTPUT_VALID", service)
        self.assertIn("message.display_session_id", service)
        self.assertIn("lvgl_app_activity_round_update(&activity_decision)", display)
        self.assertIn("rf_ui_apply_fusion_round(&ui_round)", lvgl)
        self.assertIn("rf_box_window_identity_matches", rf_ui)
        self.assertIn("RF_UI_FUSION_ROUND_OUTPUT_VALID", rf_ui)

    def test_v25_configuration_and_independent_on_off_evidence_are_used(self) -> None:
        config = V25_CONFIG.read_text(encoding="utf-8")
        fusion = V25_SOURCE.read_text(encoding="utf-8")
        service = SERVICE_SOURCE.read_text(encoding="utf-8")
        self.assertIn("const rf_v25_activity_config_t g_rf_v25_activity_config", config)
        self.assertIn("INT32_C(3973)", config)
        self.assertIn("INT32_C(14336)", config)
        self.assertIn('"rf_v25_activity_calibration.h"', service)
        self.assertRegex(
            service,
            re.compile(
                r"rf_v25_activity_fusion_apply_round\s*\([^;]*"
                r"&g_rf_v25_activity_config\s*\)",
                re.DOTALL,
            ),
        )
        self.assertIn("RF_V25_EVIDENCE_STRONG_TEXTURE", fusion)
        self.assertIn("RF_V25_REASON_INVALID_ROUND_FROZEN", fusion)
        self.assertIn("RF_V25_REASON_DUPLICATES_CAPPED", fusion)
        self.assertNotIn("rf_v18_activity_fusion_apply_round", service)
        self.assertNotIn("rf_v24_t12_activity_fusion_apply_round", service)
        self.assertNotIn("rf_v27_activity_fusion_apply_round", service)
        self.assertIn(
            "sizeof(rf_v25_activity_fusion_t) == 536u",
            V25_HEADER.read_text(encoding="utf-8"),
        )
        service_header = SERVICE_HEADER.read_text(encoding="utf-8")
        self.assertIn("sizeof(rf_v27_activity_service_proof_t) == 592U", service_header)
        self.assertIn("sizeof(rf_v27_activity_round_decision_t) == 56U", service_header)
        self.assertIn("RF_V27_ACTIVITY_SERVICE_PROOF_VERSION (2U)", service_header)
        self.assertIn("RF_V27_ACTIVITY_SERVICE_FLAG_V25_FUSION", service_header)
        self.assertIn("g_rf_v27_activity_output_generation", service_header)

    def test_cpu0_epoch_resets_before_any_new_message_is_applied(self) -> None:
        source = SERVICE_SOURCE.read_text(encoding="utf-8")
        epoch_reset = source.index("if (cpu0_epoch_changed)")
        no_message = source.index("if (!message_ready)", epoch_reset)
        apply_round = source.index("rf_v25_activity_fusion_apply_round", no_message)
        self.assertLess(epoch_reset, no_message)
        self.assertLess(no_message, apply_round)
        reset_block = source[epoch_reset:no_message]
        self.assertIn("rf_v25_activity_fusion_init", reset_block)
        self.assertIn("fusion_reset_count++", reset_block)

    def test_display_loop_initializes_and_polls_activity_service(self) -> None:
        source = DISPLAY_SOURCE.read_text(encoding="utf-8")
        self.assertIn('"activity_service.h"', source)
        self.assertIn("rf_v27_activity_service_init();", source)
        self.assertIn("rf_v27_activity_service_poll()", source)
        self.assertIn("activity_output_ready = rf_v27_activity_service_poll();", source)
        self.assertIn("if (activity_output_ready)", source)
        self.assertIn("lvgl_app_activity_update();", source)
        self.assertIn("rf_v27_activity_service_take_round_decision", source)

    def test_control_owners_are_on_separate_cache_lines(self) -> None:
        self.assertEqual(32, ctypes.sizeof(ActivityCpu0State))
        self.assertEqual(32, ctypes.sizeof(ActivityCpu1State))
        self.assertEqual(32, ActivityControl.cpu1.offset)
        self.assertEqual(64, ctypes.sizeof(ActivityControl))
        self.assertEqual(
            32,
            numeric_macro(MAILBOX_HEADER, "RA8P1_ACTIVITY_CACHE_LINE_BYTES"),
        )

    def test_activity_carve_is_aligned_and_non_overlapping(self) -> None:
        cache_line = numeric_macro(
            MAILBOX_HEADER, "RA8P1_ACTIVITY_CACHE_LINE_BYTES"
        )
        control_offset = numeric_macro(
            RESOURCE_HEADER, "RA8P1_ACTIVITY_CONTROL_OFFSET"
        )
        control_bytes = numeric_macro(
            RESOURCE_HEADER, "RA8P1_ACTIVITY_CONTROL_BYTES"
        )
        display_offset = numeric_macro(
            RESOURCE_HEADER, "RA8P1_DISPLAY_STREAM_OFFSET"
        )
        tile_offset = numeric_macro(RESOURCE_HEADER, "RA8P1_DISPLAY_TILE_OFFSET")
        tile_bytes = (
            numeric_macro(RESOURCE_HEADER, "RA8P1_DISPLAY_TILE_SLOT_BYTES")
            * numeric_macro(RESOURCE_HEADER, "RA8P1_DISPLAY_TILE_SLOT_COUNT")
        )
        message_offset = numeric_macro(
            RESOURCE_HEADER, "RA8P1_ACTIVITY_MESSAGE_OFFSET"
        )
        message_bytes = numeric_macro(
            RESOURCE_HEADER, "RA8P1_ACTIVITY_MESSAGE_BYTES"
        )
        command_offset = numeric_macro(
            RESOURCE_HEADER, "RA8P1_IPC_COMMAND_OFFSET"
        )

        self.assertEqual(0, control_offset % cache_line)
        self.assertEqual(0, control_bytes % cache_line)
        self.assertEqual(0, message_offset % cache_line)
        self.assertEqual(0, message_bytes % cache_line)
        self.assertLessEqual(control_offset + control_bytes, display_offset)
        self.assertLessEqual(tile_offset + tile_bytes, message_offset)
        self.assertLessEqual(message_offset + message_bytes, command_offset)
        self.assertEqual(512, message_bytes)
        self.assertIn(
            "sizeof(rf_v13_cpu0_round_message_t) == 512u",
            FUSION_HEADER.read_text(encoding="utf-8"),
        )

    def test_mailbox_publish_and_poll_do_cache_maintenance(self) -> None:
        producer = CPU0_IPC_SOURCE.read_text(encoding="utf-8").split(
            "bool ipc_bridge_cpu0_activity_publish", 1
        )[1].split("void ipc_bridge_cpu0_publish", 1)[0]
        consumer = CPU1_IPC_SOURCE.read_text(encoding="utf-8").split(
            "bool ipc_bridge_cpu1_activity_poll", 1
        )[1].split("bool ipc_bridge_cpu1_poll", 1)[0]

        odd_write = producer.index("state->begin_sequence = sequence | 1U;")
        payload_copy = producer.index(
            "memcpy((void *)RA8P1_ACTIVITY_MESSAGE", odd_write
        )
        stages = (
            odd_write,
            producer.index("ipc_cpu0_activity_state_clean();", odd_write),
            payload_copy,
            producer.index("SCB_CleanDCache_by_Addr", payload_copy),
            producer.index("state->end_sequence = sequence;", payload_copy),
            producer.index("state->begin_sequence = sequence;", payload_copy),
        )
        self.assertEqual(tuple(sorted(stages)), stages)
        self.assertIn("SCB_InvalidateDCache_by_Addr", consumer)
        self.assertIn("RA8P1_ACTIVITY_MESSAGE", consumer)
        self.assertGreaterEqual(consumer.count("ipc_cpu1_activity_state_publish"), 2)


if __name__ == "__main__":
    unittest.main()
