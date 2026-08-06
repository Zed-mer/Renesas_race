#!/usr/bin/env python3
"""Static integration guards for the V20 split-head inference path."""

from __future__ import annotations

import hashlib
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FRAMEWORK = ROOT / "cpu0" / "src" / "framework"
NPU_MODEL = FRAMEWORK / "npu_model"
CONTRACT = ROOT / "shared" / "rf_v12_sparse_contract.h"
V20_HEADER = FRAMEWORK / "rf_v20_video_postprocess.h"
V20_SOURCE = FRAMEWORK / "rf_v20_video_postprocess.c"
DETECTOR_SOURCE = FRAMEWORK / "rf_v12_detector.c"
IQ_NPU_SOURCE = NPU_MODEL / "iq_npu_model.c"
V2_MODEL_SOURCE = NPU_MODEL / "rf_v12_v2_model.c"
V20_MODEL_HEADER = NPU_MODEL / "rf_v20_model_data.h"
V20_MODEL_SOURCE = NPU_MODEL / "rf_v20_v3_model.c"
ACTIVITY_HEADER = ROOT / "shared" / "rf_v13_activity_fusion.h"


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


def byte_array(path: pathlib.Path, name: str) -> bytes:
    match = re.search(
        rf"static const uint8_t\s+{re.escape(name)}\[[^\]]+\]"
        r".*?=\s*\{(?P<body>.*?)\};",
        source(path),
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"array {name} not found in {path}")
    return bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group("body"))
    )


class RfV20IntegrationTests(unittest.TestCase):
    def test_model_blobs_are_the_approved_v2_and_v20_payloads(self) -> None:
        expected = (
            (
                V2_MODEL_SOURCE,
                "g_rf_v12_v2_command",
                2384,
                "2f191b49e77173da00566c98a815569fc7eb6f482df0ecd38346462fcc621746",
            ),
            (
                V2_MODEL_SOURCE,
                "g_rf_v12_v2_weights",
                10320,
                "4997538e726f5595063dc5e5a483e1100bff76c3b44d4dcf49ca4f13d3d8f853",
            ),
            (
                V20_MODEL_SOURCE,
                "g_rf_v20_v3_command",
                25996,
                "6feb559c7c2f9effd2f5fd6f92e184cf2e4ebc350df1e4b6758fc045afbcfbbf",
            ),
            (
                V20_MODEL_SOURCE,
                "g_rf_v20_v3_weights",
                11616,
                "29127ecf2b0df9ea62bebb526011e26182837020fdf382993c4ab7a5240fed3e",
            ),
        )
        for path, name, byte_count, digest in expected:
            with self.subTest(name=name):
                payload = byte_array(path, name)
                self.assertEqual(byte_count, len(payload))
                self.assertEqual(digest, hashlib.sha256(payload).hexdigest())

        self.assertEqual(
            "de5ea8fde47bc71002f7ac477bc9bfefcb1ed6d1e30d41f1ad2728f85894b30a",
            normalized_sha256(V20_MODEL_SOURCE),
        )

    def test_v20_arena_and_tensor_contract(self) -> None:
        self.assertEqual(189312, numeric_macro(CONTRACT, "RF_V12_SHARED_ARENA_BYTES"))
        self.assertEqual(94656, numeric_macro(CONTRACT, "RF_V12_V2_ARENA_INPUT_OFFSET"))
        self.assertEqual(47328, numeric_macro(CONTRACT, "RF_V20_V3_ARENA_INPUT_OFFSET"))
        self.assertEqual(0, numeric_macro(CONTRACT, "RF_V20_V3_VIDEO_OFFSET"))
        self.assertEqual(89, numeric_macro(CONTRACT, "RF_V20_V3_VIDEO_ZERO_POINT"))
        self.assertEqual(83, numeric_macro(CONTRACT, "RF_V20_V3_VIDEO_THRESHOLD"))
        self.assertEqual(161488, numeric_macro(V20_MODEL_HEADER, "RF_V20_V3_SCRATCH_BYTES"))
        self.assertEqual(93840, numeric_macro(V20_MODEL_HEADER, "RF_V20_V3_INPUT_BYTES"))

        descriptor = source(V20_MODEL_SOURCE).split(
            "const rf_v20_model_blob_t g_rf_v20_v3_model = {", 1
        )[1]
        self.assertIn(".input_offset = 47328u", descriptor)
        self.assertIn(".input_bytes = RF_V20_V3_INPUT_BYTES", descriptor)
        self.assertIn(".output_count = RF_V20_V3_OUTPUT_COUNT", descriptor)
        self.assertIn(".output_offset = {0u, 0u, 0u, 0u, 0u}", descriptor)
        self.assertIn(".output_bytes = {5916u, 0u, 0u, 0u, 0u}", descriptor)

    def test_v2_outputs_are_saved_before_v20_overwrites_the_arena(self) -> None:
        contract = source(CONTRACT)
        for name, value in (
            ("RF_V12_V2_OUTPUT_XIAOBAWANG", 0),
            ("RF_V12_V2_OUTPUT_VIDEO_IGNORED", 1),
            ("RF_V12_V2_OUTPUT_T12", 2),
            ("RF_V12_V2_OUTPUT_DJI_CONTROL", 3),
            ("RF_V12_V2_OUTPUT_AT9S", 4),
        ):
            self.assertRegex(contract, rf"\b{re.escape(name)}\s*=\s*{value}\b")

        inference = source(IQ_NPU_SOURCE)
        stages = (
            "memcpy(s_iq_npu_arena + g_rf_v12_v2_model.input_offset",
            "iq_npu_invoke_blob(g_rf_v12_v2_model.command",
            "g_rf_v12_v2_model.output_offset[RF_V12_V2_OUTPUT_XIAOBAWANG]",
            "g_rf_v12_v2_model.output_offset[RF_V12_V2_OUTPUT_T12]",
            "g_rf_v12_v2_model.output_offset[RF_V12_V2_OUTPUT_DJI_CONTROL]",
            "g_rf_v12_v2_model.output_offset[RF_V12_V2_OUTPUT_AT9S]",
            "memcpy(s_iq_npu_arena + g_rf_v20_v3_model.input_offset",
            "iq_npu_invoke_blob(g_rf_v20_v3_model.command",
            "s_iq_npu_arena + g_rf_v20_v3_model.output_offset[0]",
        )
        positions = tuple(inference.index(stage) for stage in stages)
        self.assertEqual(tuple(sorted(positions)), positions)
        self.assertNotIn("g_rf_v12_v3_model", inference)
        self.assertFalse((NPU_MODEL / "rf_v12_v3_model.c").exists())

    def test_v20_score_tiers_map_to_existing_sprt_buckets(self) -> None:
        self.assertEqual(83, numeric_macro(V20_HEADER, "RF_V20_VIDEO_HEATMAP_THRESHOLD_Q8"))
        self.assertEqual(90, numeric_macro(V20_HEADER, "RF_V20_VIDEO_SCORE_055_Q8"))
        self.assertEqual(97, numeric_macro(V20_HEADER, "RF_V20_VIDEO_SCORE_075_Q8"))
        self.assertEqual(105, numeric_macro(V20_HEADER, "RF_V20_VIDEO_SCORE_090_Q8"))
        self.assertEqual(-140, numeric_macro(V20_HEADER, "RF_V20_VIDEO_DISPLAY_SCORE_Q8"))

        tiers = source(V20_SOURCE).split(
            "uint8_t rf_v20_video_score_tier", 1
        )[1].split("int rf_v20_video_postprocess", 1)[0]
        for threshold, tier in (
            ("RF_V20_VIDEO_SCORE_090_Q8", 3),
            ("RF_V20_VIDEO_SCORE_075_Q8", 2),
            ("RF_V20_VIDEO_SCORE_055_Q8", 1),
        ):
            self.assertRegex(
                tiers,
                rf"(?s)raw_logit\s*>=\s*{threshold}.*?return\s+{tier}u",
            )
        self.assertRegex(tiers, r"return\s+0u")
        self.assertRegex(
            source(DETECTOR_SOURCE),
            r"tier_confidence_q15\[4\]\s*=\s*\{\s*"
            r"0U,\s*18022U,\s*24575U,\s*29490U\s*\}",
        )

    def test_cross_core_payload_abi_remains_512_bytes(self) -> None:
        sparse_contract = source(CONTRACT)
        activity_contract = source(ACTIVITY_HEADER)
        self.assertIn(
            "sizeof(rf_v12_tile_payload_t) == 512u",
            sparse_contract,
        )
        self.assertIn(
            "sizeof(rf_v13_cpu0_round_message_t) == 512u",
            activity_contract,
        )


if __name__ == "__main__":
    unittest.main()
