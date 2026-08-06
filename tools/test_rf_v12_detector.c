#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rf_v12_detector.h"
#include "rf_v12_preprocess.h"
#include "npu_runner.h"

static int8_t g_heatmaps[RF_V12_CLASS_COUNT][RF_V12_HEATMAP_BYTES];
static int8_t g_model_input[RF_V12_FEATURE_BYTES];

const int8_t *npu_runner_heatmap(uint32_t class_id)
{
    return (class_id < RF_V12_CLASS_COUNT) ? g_heatmaps[class_id] : NULL;
}

static uint32_t heatmap_index(uint32_t frequency, uint32_t time)
{
    return (frequency * RF_V12_HEATMAP_TIME_BINS) + time;
}

static void reset_heatmaps(void)
{
    memset(g_heatmaps, INT8_MIN, sizeof(g_heatmaps));
}

static int fail(const char *message)
{
    fprintf(stderr, "rf_v12_detector test failed: %s\n", message);
    return 1;
}

static rf_v12_detector_input_t valid_input(uint32_t center_index,
                                           const float *c0)
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
    input.background_relative_c0 = c0;
    return input;
}

static void set_video_component(uint32_t minimum_frequency,
                                uint32_t minimum_time,
                                const int8_t values[4])
{
    uint32_t value = 0U;
    for (uint32_t df = 0U; df < 2U; ++df)
    {
        for (uint32_t dt = 0U; dt < 2U; ++dt)
        {
            g_heatmaps[RF_V12_CLASS_DJI_VIDEO]
                      [heatmap_index(minimum_frequency + df,
                                     minimum_time + dt)] = values[value++];
        }
    }
}

static int test_probability_order_and_mask(void)
{
    float c0[RF_V12_PREPROCESS_FEATURE_CELLS] = {0.0F};
    rf_v12_detector_input_t input = valid_input(0U, c0);
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

    reset_heatmaps();
    /* Raw 100 is larger than raw 89, but XIAOBAWANG's zero point is 109.
     * Cross-head arbitration must therefore rank DJI's probability first. */
    g_heatmaps[RF_V12_CLASS_DJI_CONTROL]
              [heatmap_index(dji_frequency, dji_time)] =
        RF_V12_V2_DJI_CONTROL_THRESHOLD;
    g_heatmaps[RF_V12_CLASS_XIAOBAWANG][heatmap_index(20U, 40U)] =
        RF_V12_V2_XIAOBAWANG_THRESHOLD;
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 2U) ||
        (result.best_class_id != RF_V12_CLASS_DJI_CONTROL))
    {
        return fail("global order compared raw INT8 values across heads");
    }
    if ((result.display_mask[expected_mask_index >> 3U] &
         (uint8_t)(1U << (expected_mask_index & 7U))) == 0U)
    {
        return fail("display mask frequency/time axes are transposed");
    }

    /* Non-DJI 2.4 GHz heads are forbidden at a 5.8 GHz center, including the
     * UI mask; the same DJI cell remains legal. */
    input = valid_input(2U, c0);
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 1U) ||
        (result.tile.events[0].class_id != RF_V12_CLASS_DJI_CONTROL))
    {
        return fail("class frequency gate mismatch");
    }
    return 0;
}

static void fill_video_profile(float *c0,
                               int64_t component_center_hz,
                               float shoulder_db)
{
    memset(c0, 0, sizeof(float) * RF_V12_PREPROCESS_FEATURE_CELLS);
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        const int64_t frequency_hz =
            ((-60000000LL * 204LL) +
             (int64_t)(2U * frequency + 1U) * 60000000LL) / 408LL;
        const uint64_t distance = (frequency_hz >= component_center_hz) ?
            (uint64_t)(frequency_hz - component_center_hz) :
            (uint64_t)(component_center_hz - frequency_hz);
        if ((distance >= 5500000ULL) && (distance <= 9500000ULL))
        {
            for (uint32_t time = 0U;
                 time < RF_V12_FEATURE_TIME_BINS;
                 ++time)
            {
                c0[(frequency * RF_V12_FEATURE_TIME_BINS) + time] =
                    shoulder_db;
            }
        }
    }
}

static int test_video_db_threshold_and_edge(void)
{
    static const int8_t component_values[4] = {90, 90, 90, 90};
    float c0[RF_V12_PREPROCESS_FEATURE_CELLS];
    rf_v12_detector_input_t input;
    rf_v12_detector_result_t result;
    uint32_t bandwidth;

    reset_heatmaps();
    set_video_component(50U, 20U, component_values);
    fill_video_profile(c0, 0LL, 0.5F);
    input = valid_input(0U, c0);
    rf_v12_detector_decode(&input, &result);
    if (result.tile.event_count != 1U)
    {
        return fail("central V3 component was not decoded");
    }
    bandwidth = (uint32_t)(result.tile.events[0].frequency_high_offset_hz -
                           result.tile.events[0].frequency_low_offset_hz);
    if (bandwidth != RF_V12_DJI_VIDEO_10M_BANDWIDTH_HZ)
    {
        return fail("0.5 dB shoulder was treated as the old log2 threshold");
    }

    fill_video_profile(c0, 0LL, 1.0F);
    rf_v12_detector_decode(&input, &result);
    bandwidth = (uint32_t)(result.tile.events[0].frequency_high_offset_hz -
                           result.tile.events[0].frequency_low_offset_hz);
    if ((bandwidth != RF_V12_DJI_VIDEO_20M_BANDWIDTH_HZ) ||
        ((result.tile.events[0].flags & RF_V12_EVENT_VIDEO_20MHZ) == 0U))
    {
        return fail("1.0 dB occupied shoulder did not select 20 MHz");
    }

    reset_heatmaps();
    set_video_component(9U, 20U, component_values);
    memset(c0, 0, sizeof(c0));
    input = valid_input(0U, c0);
    rf_v12_detector_decode(&input, &result);
    if ((result.tile.event_count != 1U) ||
        ((result.tile.events[0].flags & RF_V12_EVENT_VIDEO_20MHZ) == 0U))
    {
        return fail("reliable-band edge snap did not force 20 MHz");
    }
    return 0;
}

static int test_video_p90_and_nms(void)
{
    static const int8_t lower_values[4] = {83, 83, 83, 83};
    static const int8_t higher_values[4] = {83, 83, 83, 100};
    float c0[RF_V12_PREPROCESS_FEATURE_CELLS] = {0.0F};
    rf_v12_detector_input_t input = valid_input(0U, c0);
    rf_v12_detector_result_t result;

    reset_heatmaps();
    set_video_component(50U, 20U, lower_values);
    set_video_component(53U, 20U, higher_values);
    rf_v12_detector_decode(&input, &result);
    if (result.tile.event_count != 1U)
    {
        return fail("V3 components did not receive per-class NMS");
    }
    /* Nearest-rank P90 of four cells selects the high cell, so the second,
     * positive-frequency component must win the overlapping-box NMS. */
    if (result.tile.events[0].frequency_low_offset_hz <= -4000000)
    {
        return fail("V3 P90 score did not select the stronger component");
    }
    return 0;
}

static int test_global_top4(void)
{
    float c0[RF_V12_PREPROCESS_FEATURE_CELLS] = {0.0F};
    rf_v12_detector_input_t input = valid_input(0U, c0);
    rf_v12_detector_result_t result;
    static const uint32_t times[5] = {1U, 12U, 23U, 34U, 45U};

    reset_heatmaps();
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

static int test_v20_roi_gate_filters_uniform_input(void)
{
    static const int8_t component_values[4] = {105, 105, 105, 105};
    float c0[RF_V12_PREPROCESS_FEATURE_CELLS] = {0.0F};
    rf_v12_detector_input_t input = valid_input(0U, c0);
    rf_v12_detector_result_t result;

    reset_heatmaps();
    memset(g_model_input, RF_V12_INPUT_ZERO_POINT, sizeof(g_model_input));
    set_video_component(50U, 20U, component_values);
    input.model_input = g_model_input;
    rf_v12_detector_decode(&input, &result);
    if (result.tile.event_count != 0U)
    {
        return fail("V20 ROI/precision gate accepted a uniform input");
    }
    return 0;
}

int main(void)
{
    if ((test_probability_order_and_mask() != 0) ||
        (test_video_db_threshold_and_edge() != 0) ||
        (test_video_p90_and_nms() != 0) ||
        (test_global_top4() != 0) ||
        (test_v20_roi_gate_filters_uniform_input() != 0))
    {
        return 1;
    }
    puts("rf_v12_detector: all host tests passed");
    return 0;
}
