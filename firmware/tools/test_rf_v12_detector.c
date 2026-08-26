#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "npu_runner.h"
#include "rf_v12_detector.h"
#include "rf_v16_roi_calibration.h"
#include "rf_v17_subbin_calibration.h"
#include "rf_v18_source_gate.h"
#include "rf_v20_video_postprocess.h"
#include "rf_v26_partition_guard.h"
#include "rf_v31_detection_contract.h"

static int8_t g_heatmaps[RF_V12_CLASS_COUNT][RF_V12_HEATMAP_BYTES];
static int8_t g_model_input[RF_V12_FEATURE_BYTES];
static int g_guard_accept = 1;
static int g_width_success = 1;
static int32_t g_width_bandwidth_hz = RF_V20_VIDEO_10MHZ_HZ;
static uint32_t g_width_call_count;

const rf_v16_class_config_t
    g_rf_v16_class_configs[RF_V16_CLASS_COUNT] = {0};
const rf_v17_subbin_config_t
    g_rf_v17_subbin_configs[RF_V16_CLASS_COUNT] = {0};
const rf_v26_guard_model_t g_rf_v26_guard_2g4 = {0};
const rf_v26_guard_model_t g_rf_v26_guard_5g8 = {0};

const int8_t *npu_runner_heatmap(uint32_t class_id)
{
    return (class_id < RF_V12_CLASS_COUNT) ? g_heatmaps[class_id] : NULL;
}

bool npu_runner_classify_video_width(
    const void *features,
    uint32_t feature_bytes,
    int32_t center_frequency_offset_hz,
    int32_t center_sample,
    rf_v32_width_track_t *track,
    int32_t *bandwidth_hz)
{
    (void)center_frequency_offset_hz;
    (void)center_sample;
    g_width_call_count++;
    if ((features == NULL) || (feature_bytes != RF_V12_FEATURE_BYTES) ||
        (track == NULL) || (bandwidth_hz == NULL) || !g_width_success)
    {
        return false;
    }
    track->bandwidth_hz = g_width_bandwidth_hz;
    *bandwidth_hz = g_width_bandwidth_hz;
    return true;
}

void rf_v32_width_track_init(rf_v32_width_track_t *track)
{
    if (track != NULL)
    {
        memset(track, 0, sizeof(*track));
    }
}

int rf_v17_refine_candidate(
    const int8_t *input_nhwc,
    size_t input_bytes,
    const rf_v16_candidate_t *candidate,
    const rf_v16_class_config_t *class_configs,
    const rf_v17_subbin_config_t *subbin_configs,
    rf_v16_postprocess_result_t *result)
{
    (void)class_configs;
    (void)subbin_configs;
    if ((input_nhwc == NULL) || (input_bytes != RF_V12_FEATURE_BYTES) ||
        (candidate == NULL) || (result == NULL))
    {
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->statistics.contrast_q8 = 512;
    result->statistics.frequency_edge_q8 = 256;
    result->statistics.time_edge_q8 = 256;
    result->statistics.burstiness_q8 = 256;
    result->statistics.texture_q8 = 128;
    result->statistics.occupancy_q15 = 24575U;
    result->statistics.visible_fraction_q15 = RF_V16_Q15_ONE;
    result->display_score_q8 = 512;
    return 1;
}

int rf_v20_video_postprocess(
    const int8_t *input_nhwc,
    size_t input_bytes,
    uint64_t capture_center_frequency_hz,
    const rf_v20_video_event_t *input_event,
    rf_v20_video_postprocess_result_t *result)
{
    (void)capture_center_frequency_hz;
    if ((input_nhwc == NULL) || (input_bytes != RF_V12_FEATURE_BYTES) ||
        (input_event == NULL) || (result == NULL))
    {
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->event = *input_event;
    result->width.current_10mhz_contrast_codes = 20;
    result->width.best_20mhz_contrast_codes = 18;
    result->width.left_shoulder_contrast_codes = 8;
    result->width.right_shoulder_contrast_codes = 8;
    return 1;
}

int rf_v26_guard_accept(
    const rf_v26_guard_model_t *model,
    const int32_t raw_features[RF_V26_GUARD_FEATURE_COUNT])
{
    return ((model != NULL) && (raw_features != NULL) && g_guard_accept) ? 1 : 0;
}

static uint32_t heatmap_index(uint32_t frequency, uint32_t time)
{
    return (frequency * RF_V12_HEATMAP_TIME_BINS) + time;
}

static void reset_test_state(void)
{
    memset(g_heatmaps, INT8_MIN, sizeof(g_heatmaps));
    memset(g_model_input, 0, sizeof(g_model_input));
    g_guard_accept = 1;
    g_width_success = 1;
    g_width_bandwidth_hz = RF_V20_VIDEO_10MHZ_HZ;
    g_width_call_count = 0U;
}

static int fail(const char *message)
{
    fprintf(stderr, "rf_v12_detector test failed: %s\n", message);
    return 1;
}

static rf_v12_detector_input_t valid_input(uint32_t center_index)
{
    static const uint64_t centers[RF_V12_CENTER_COUNT] =
    {
        RF_V12_CENTER_2420_HZ,
        RF_V12_CENTER_2464_HZ,
        RF_V12_CENTER_5760_HZ,
        RF_V12_CENTER_5816_HZ
    };
    rf_v12_detector_input_t input;

    memset(&input, 0, sizeof(input));
    input.tile_sequence = 7U;
    input.round_index = 2U;
    input.center_index = center_index;
    input.center_frequency_hz = centers[center_index];
    input.capture_start_time_us = 1000000ULL;
    input.capture_end_time_us = 1010000ULL;
    input.background_generation = 3U;
    input.sdr_gain_db_q8 = 12 * 256;
    input.tile_validity = RF_V12_TILE_VALID;
    return input;
}

static int test_v31_probability_order_mask_and_center_gate(void)
{
    rf_v12_detector_input_t input = valid_input(0U);
    rf_v12_detector_result_t result;
    const uint32_t dji_frequency = 80U;
    const uint32_t dji_time = 8U;
    const uint32_t expected_x =
        (dji_frequency * RA8P1_DISPLAY_MASK_WIDTH) /
        RF_V12_HEATMAP_FREQUENCY_BINS;
    const uint32_t expected_y =
        (dji_time * RA8P1_DISPLAY_MASK_HEIGHT) /
        RF_V12_HEATMAP_TIME_BINS;
    const uint32_t expected_mask_index =
        expected_y * RA8P1_DISPLAY_MASK_WIDTH + expected_x;

    reset_test_state();
    g_heatmaps[RF_V12_CLASS_DJI_CONTROL]
              [heatmap_index(dji_frequency, dji_time)] =
        RF_V31_DJI_CONTROL_THRESHOLD_Q;
    g_heatmaps[RF_V12_CLASS_XIAOBAWANG][heatmap_index(20U, 40U)] =
        RF_V31_XIAOBAWANG_THRESHOLD_Q;
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 2U) ||
        (result.best_class_id != RF_V12_CLASS_DJI_CONTROL))
    {
        return fail("V31 global order did not use per-route quantization");
    }
    if ((result.display_mask[expected_mask_index >> 3U] &
         (uint8_t)(1U << (expected_mask_index & 7U))) == 0U)
    {
        return fail("display mask frequency/time axes are transposed");
    }
    if ((result.tile.background_generation != 0U) ||
        (result.state_roi_decision[0] != RF_V13_ROI_PASS) ||
        (result.state_quality_tier[0] != RF_V18_QUALITY_STRONG))
    {
        return fail("V31 accepted route did not publish no-background strong evidence");
    }

    input = valid_input(2U);
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 1U) ||
        (result.tile.events[0].class_id != RF_V12_CLASS_DJI_CONTROL))
    {
        return fail("V31 class center mask mismatch");
    }
    return 0;
}

static int test_global_top4(void)
{
    rf_v12_detector_input_t input = valid_input(0U);
    rf_v12_detector_result_t result;
    static const uint32_t times[5] = {1U, 12U, 23U, 34U, 45U};

    reset_test_state();
    for (uint32_t i = 0U; i < 5U; ++i)
    {
        g_heatmaps[RF_V12_CLASS_DJI_CONTROL]
                  [heatmap_index(50U, times[i])] = (int8_t)(120 - i);
    }
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != RF_V12_MAX_BOXES_PER_TILE) ||
        ((result.tile.flags & RF_V12_TILE_RESULT_TRUNCATED) == 0U))
    {
        return fail("global top-4/truncation contract mismatch");
    }
    for (uint32_t i = 1U; i < result.tile.event_count; ++i)
    {
        if (result.tile.events[i - 1U].confidence_q15 <
            result.tile.events[i].confidence_q15)
        {
            return fail("global top-4 is not probability sorted");
        }
    }
    return 0;
}

static int test_video_guard_width_and_roi_limit(void)
{
    rf_v12_detector_input_t input = valid_input(0U);
    rf_v12_detector_result_t result;
    static const uint32_t frequencies[6] = {18U, 34U, 50U, 66U, 82U, 96U};

    reset_test_state();
    input.model_input = g_model_input;
    g_width_bandwidth_hz = RF_V20_VIDEO_20MHZ_HZ;
    for (uint32_t i = 0U; i < 6U; ++i)
    {
        g_heatmaps[RF_V12_CLASS_DJI_VIDEO]
                  [heatmap_index(frequencies[i], 28U)] = (int8_t)(90 + i);
    }
    rf_v12_detector_decode(&input, &result);
    if ((g_width_call_count != 4U) || (result.tile.event_count != 4U))
    {
        return fail("V32 width specialist exceeded or missed the four-ROI limit");
    }
    for (uint32_t i = 0U; i < result.tile.event_count; ++i)
    {
        const uint32_t bandwidth = (uint32_t)(
            result.tile.events[i].frequency_high_offset_hz -
            result.tile.events[i].frequency_low_offset_hz);
        if ((bandwidth != RF_V31_DJI_VIDEO_20M_BANDWIDTH_HZ) ||
            ((result.tile.events[i].flags & RF_V12_EVENT_VIDEO_20MHZ) == 0U) ||
            (result.state_roi_decision[i] != RF_V13_ROI_PASS) ||
            (result.state_quality_tier[i] != RF_V18_QUALITY_STRONG))
        {
            return fail("V32 20 MHz decision did not rebuild the final fixed box");
        }
    }

    reset_test_state();
    input.model_input = g_model_input;
    g_heatmaps[RF_V12_CLASS_DJI_VIDEO][heatmap_index(50U, 28U)] = 100;
    g_guard_accept = 0;
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 0U) || (g_width_call_count != 0U))
    {
        return fail("V32 ran before the P93 guard accepted the proposal");
    }

    reset_test_state();
    input.model_input = g_model_input;
    g_heatmaps[RF_V12_CLASS_DJI_VIDEO][heatmap_index(50U, 28U)] = 100;
    g_width_success = 0;
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 0U) || (g_width_call_count != 1U))
    {
        return fail("failed V32 observation did not reject the video proposal");
    }

    reset_test_state();
    input.model_input = g_model_input;
    g_heatmaps[RF_V12_CLASS_DJI_VIDEO][heatmap_index(50U, 28U)] = 100;
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 1U) ||
        ((result.tile.events[0].flags & RF_V12_EVENT_VIDEO_20MHZ) != 0U) ||
        ((uint32_t)(result.tile.events[0].frequency_high_offset_hz -
                    result.tile.events[0].frequency_low_offset_hz) !=
         RF_V31_DJI_VIDEO_10M_BANDWIDTH_HZ))
    {
        return fail("V32 10 MHz decision did not rebuild the final fixed box");
    }
    return 0;
}

int main(void)
{
    if ((test_v31_probability_order_mask_and_center_gate() != 0) ||
        (test_global_top4() != 0) ||
        (test_video_guard_width_and_roi_limit() != 0))
    {
        return 1;
    }
    puts("rf_v12_detector: V31/V32 host tests passed");
    return 0;
}
