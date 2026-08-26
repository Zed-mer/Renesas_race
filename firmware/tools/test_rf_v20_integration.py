#!/usr/bin/env python3
"""Static integration guards for the V31 detection and V32 width path."""

from __future__ import annotations

import hashlib
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FRAMEWORK = ROOT / "cpu0" / "src" / "framework"
NPU_MODEL = FRAMEWORK / "npu_model"
SPARSE_CONTRACT = ROOT / "shared" / "rf_v12_sparse_contract.h"
ACTIVITY_HEADER = ROOT / "shared" / "rf_v13_activity_fusion.h"
V31_CONTRACT = FRAMEWORK / "rf_v31_detection_contract.h"
V31_MODEL_HEADER = NPU_MODEL / "rf_v31_model_data.h"
V31_SCHEDULE = FRAMEWORK / "rf_v31_model_schedule.c"
V32_MODEL_HEADER = NPU_MODEL / "rf_v32_width_model_data.h"
V32_SCHEDULE = FRAMEWORK / "rf_v32_width_schedule.c"
DETECTOR_SOURCE = FRAMEWORK / "rf_v12_detector.c"
PREPROCESS_SOURCE = FRAMEWORK / "rf_v12_preprocess.c"
ANALYSIS_SOURCE = FRAMEWORK / "analysis_pipeline.c"
IQ_NPU_SOURCE = NPU_MODEL / "iq_npu_model.c"


def source(path: pathlib.Path) -> str:
    return path.read_text(encoding="ascii")


def normalized_sha256(path: pathlib.Path) -> str:
    payload = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(payload).hexdigest()


def numeric_macro(path: pathlib.Path, name: str) -> int:
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(.+?)\s*$",
        source(path),
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"macro {name} not found in {path}")
    value = match.group(1).split("/*", 1)[0].strip()
    while value.startswith("(") and value.endswith(")"):
        value = value[1:-1].strip()
    literal = re.fullmatch(
        r"(?:U?INT(?:8|16|32|64)_C\(\s*)?"
        r"(?P<number>-?(?:0x[0-9A-Fa-f]+|[0-9]+))"
        r"\s*\)?(?:ULL|UL|LL|U|L)?",
        value,
        re.IGNORECASE,
    )
    if literal is None:
        raise AssertionError(f"macro {name} is not a literal: {value}")
    return int(literal.group("number"), 0)


class RfV31V32IntegrationTests(unittest.TestCase):
    def test_model_sources_match_the_approved_v31_v32_payloads(self) -> None:
        expected = {
            "rf_v31_main_model.c":
                "6a42d9e8666d49aab9eb9546c42044395350f49105f63216c103a7537428aa9d",
            "rf_v31_dji_video_model.c":
                "6f6ad4ebeffccf84f5cd97adca78beb1ad6836f4322ba9079c4c55e6373cd504",
            "rf_v31_dji_control_model.c":
                "3ca4118878eb40431127cd7e47fa8957982b3c409b4b28f76298ebc80f3b7241",
            "rf_v31_t12_model.c":
                "f89f8ee152b4ba44e121c715827bfe9997f74d086d650701cbb2120ecfae697a",
            "rf_v32_width_model.c":
                "d463bea1b2f1f41f8fe5ff24258bf72d8e82f7d5b1364cd011d708ccabff3766",
        }
        for filename, digest in expected.items():
            with self.subTest(filename=filename):
                self.assertEqual(digest, normalized_sha256(NPU_MODEL / filename))

        self.assertEqual(
            278196,
            numeric_macro(V31_MODEL_HEADER, "RF_V31_MODEL_COMMAND_WEIGHT_BYTES"),
        )
        self.assertEqual(20092, numeric_macro(V32_MODEL_HEADER, "RF_V32_WIDTH_COMMAND_BYTES"))
        self.assertEqual(10144, numeric_macro(V32_MODEL_HEADER, "RF_V32_WIDTH_WEIGHT_BYTES"))

    def test_v31_arena_tensor_and_four_model_schedule_contract(self) -> None:
        self.assertEqual(192176, numeric_macro(V31_CONTRACT, "RF_V31_SHARED_ARENA_BYTES"))
        self.assertEqual(198256, numeric_macro(V31_CONTRACT, "RF_V31_ARENA_HARD_LIMIT_BYTES"))
        self.assertEqual(93840, numeric_macro(V31_CONTRACT, "RF_V31_INPUT_BYTES"))
        self.assertEqual(5916, numeric_macro(V31_CONTRACT, "RF_V31_HEATMAP_BYTES"))
        self.assertEqual(192176, numeric_macro(V31_MODEL_HEADER, "RF_V31_MAIN_SCRATCH_BYTES"))

        schedule = source(V31_SCHEDULE)
        stages = (
            "&g_rf_v31_main_model",
            "heatmaps[RF_V31_XIAOBAWANG]",
            "heatmaps[RF_V31_AT9S]",
            "&g_rf_v31_dji_video_model",
            "heatmaps[RF_V31_DJI_VIDEO]",
            "&g_rf_v31_dji_control_model",
            "heatmaps[RF_V31_DJI_CONTROL]",
            "&g_rf_v31_t12_model",
            "heatmaps[RF_V31_T12]",
        )
        positions = tuple(schedule.index(stage) for stage in stages)
        self.assertEqual(tuple(sorted(positions)), positions)
        self.assertEqual(4, schedule.count("status = run_one("))

        inference = source(IQ_NPU_SOURCE)
        self.assertIn("s_iq_npu_arena[RF_V31_SHARED_ARENA_BYTES]", inference)
        self.assertIn("rf_v31_run_selected_models", inference)
        self.assertNotIn("g_rf_v21_nonvideo_model", inference)
        self.assertNotIn("g_rf_v21_v20_video_model", inference)
        self.assertNotIn("g_rf_v24_t12_specialist_model", inference)
        self.assertNotIn("g_rf_v27_absolute_model", inference)

    def test_v32_runs_after_p93_guard_and_is_limited_to_four_rois(self) -> None:
        detector = source(DETECTOR_SOURCE)
        self.assertEqual(4, numeric_macro(DETECTOR_SOURCE, "RF_V32_MAX_ROIS_PER_TILE"))
        self.assertEqual(4, numeric_macro(DETECTOR_SOURCE, "RF_V32_WIDTH_TRACK_COUNT"))
        self.assertRegex(
            detector,
            re.compile(
                r"class_id\s*==\s*RF_V12_CLASS_DJI_VIDEO\s*\)\s*\?\s*"
                r"RF_V32_MAX_ROIS_PER_TILE\s*:\s*RF_V12_CANDIDATES_PER_CLASS",
                re.DOTALL,
            ),
        )

        refine = detector.split("static bool rf_v31_refine_candidate", 1)[1].split(
            "static void rf_v12_build_display_mask", 1
        )[0]
        stages = (
            refine.index("rf_v20_video_postprocess"),
            refine.index("rf_v26_guard_accept"),
            refine.index("rf_v32_track_for_proposal"),
            refine.index("npu_runner_classify_video_width"),
        )
        self.assertEqual(tuple(sorted(stages)), stages)

        width_inference = source(IQ_NPU_SOURCE).split(
            "int iq_npu_model_classify_video_width", 1
        )[1].split("static uint32_t iq_npu_checksum", 1)[0]
        width_stages = (
            width_inference.index("rf_v32_extract_width_roi"),
            width_inference.index("rf_v32_cpu_width_classify"),
            width_inference.index("rf_v32_run_width_specialist"),
            width_inference.index("rf_v32_width_track_apply"),
        )
        self.assertEqual(tuple(sorted(width_stages)), width_stages)
        self.assertEqual(11520, numeric_macro(V32_MODEL_HEADER, "RF_V32_WIDTH_INPUT_BYTES"))
        self.assertEqual(9216, numeric_macro(V32_MODEL_HEADER, "RF_V32_WIDTH_INPUT_OFFSET"))
        self.assertEqual(32, numeric_macro(V32_MODEL_HEADER, "RF_V32_WIDTH_OUTPUT_OFFSET"))
        self.assertIn("shared_arena_bytes < RF_V31_SHARED_ARENA_BYTES", source(V32_SCHEDULE))

    def test_all_five_classes_use_v31_routes_and_fixed_geometry(self) -> None:
        detector = source(DETECTOR_SOURCE)
        self.assertIn("rf_v31_decode_class(class_id", detector)
        self.assertNotIn("rf_v12_decode_video", detector)
        self.assertNotIn("rf_v24_t12_decode_specialist", detector)
        self.assertNotIn("RF_V21_NONVIDEO_", detector)
        self.assertIn("refined.state_roi_decision = RF_V13_ROI_PASS", detector)
        self.assertIn("refined.state_quality_tier = RF_V18_QUALITY_STRONG", detector)

        expected = {
            "RF_V31_DJI_CONTROL_THRESHOLD_Q": 92,
            "RF_V31_DJI_VIDEO_THRESHOLD_Q": 83,
            "RF_V31_AT9S_THRESHOLD_Q": 106,
            "RF_V31_T12_THRESHOLD_Q": 105,
            "RF_V31_XIAOBAWANG_THRESHOLD_Q": 107,
            "RF_V31_DJI_CONTROL_BANDWIDTH_HZ": 2200000,
            "RF_V31_DJI_VIDEO_10M_BANDWIDTH_HZ": 10000000,
            "RF_V31_DJI_VIDEO_20M_BANDWIDTH_HZ": 20000000,
            "RF_V31_AT9S_BANDWIDTH_HZ": 8000000,
            "RF_V31_T12_BANDWIDTH_HZ": 1700000,
            "RF_V31_XIAOBAWANG_BANDWIDTH_HZ": 2400000,
        }
        for name, value in expected.items():
            with self.subTest(name=name):
                self.assertEqual(value, numeric_macro(V31_CONTRACT, name))

    def test_no_startup_background_is_wired_through_cpu0(self) -> None:
        preprocess = source(PREPROCESS_SOURCE)
        finalize = preprocess.split("rf_v12_preprocess_finalize(", 1)[1].split(
            "bool rf_v12_preprocess_finalize_synthetic", 1
        )[0]
        self.assertIn("rf_v26_build_input", finalize)
        self.assertIn("info.background_generation = 0U", finalize)
        self.assertNotIn("RF_V12_PREPROCESS_BACKGROUND_NOT_READY", finalize)
        self.assertNotIn("rf_v12_freeze_background", finalize)

        analysis = source(ANALYSIS_SOURCE)
        self.assertIn("detector_input.background_generation = 0U", analysis)
        self.assertIn("g_analysis.preprocessing_valid = 1U", analysis)

    def test_cross_core_payload_abi_remains_512_bytes(self) -> None:
        self.assertIn(
            "sizeof(rf_v12_tile_payload_t) == 512u",
            source(SPARSE_CONTRACT),
        )
        self.assertIn(
            "sizeof(rf_v13_cpu0_round_message_t) == 512u",
            source(ACTIVITY_HEADER),
        )


if __name__ == "__main__":
    unittest.main()
