#include "rf_v20_video_postprocess.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define RF_V20_ANALYSIS_BANDWIDTH_HZ INT32_C(60000000)
#define RF_V20_RELIABLE_HALF_BANDWIDTH_HZ INT32_C(28000000)
#define RF_V20_TILE_SAMPLES INT32_C(590336)
#define RF_V20_Q8_ONE INT32_C(256)

#define RF_V20_VIDEO_TIME_RADIUS_BINS 5
#define RF_V20_VIDEO_10MHZ_BINS 34
#define RF_V20_VIDEO_20MHZ_BINS 68
#define RF_V20_VIDEO_TEMPLATE_FLANK_BINS 8
#define RF_V20_VIDEO_SEARCH_RADIUS_BINS 20
#define RF_V20_VIDEO_10MHZ_MAX_CONTRAST_CODES 12
#define RF_V20_VIDEO_20MHZ_GAIN_NUMERATOR 110
#define RF_V20_VIDEO_20MHZ_GAIN_DENOMINATOR 100
#define RF_V20_BILATERAL_MIN_CONTRAST_CODES 20
#define RF_V20_BILATERAL_MIN_SHOULDER_CODES 20

typedef struct rf_v20_support {
    int32_t start;
    int32_t end;
    int32_t outer_start;
    int32_t outer_end;
} rf_v20_support_t;

static int32_t rf_v20_round_divide_i64(int64_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return (int32_t)(-(((-numerator) + denominator / 2) / denominator));
}

static size_t rf_v20_input_index(int32_t row, int32_t column)
{
    return (size_t)(((row * (int32_t)RF_V20_INPUT_TIME_BINS) + column) *
                    (int32_t)RF_V20_INPUT_CHANNELS);
}

static int rf_v20_template_support(
    int32_t center_row,
    int32_t width,
    rf_v20_support_t *support)
{
    int32_t lower;
    if (support == NULL || width <= 0) {
        return 0;
    }
    lower = (width - 1) / 2;
    support->start = center_row - lower;
    support->end = support->start + width;
    support->outer_start =
        support->start - RF_V20_VIDEO_TEMPLATE_FLANK_BINS;
    support->outer_end =
        support->end + RF_V20_VIDEO_TEMPLATE_FLANK_BINS;
    return support->outer_start >= 0 &&
           support->outer_end <= (int32_t)RF_V20_INPUT_FREQUENCY_BINS;
}

static int64_t rf_v20_prefix_region(
    const int32_t *prefix, int32_t start, int32_t end)
{
    return (int64_t)prefix[end] - (int64_t)prefix[start];
}

static int rf_v20_template_contrast(
    const int32_t *prefix,
    int32_t center_row,
    int32_t width,
    int32_t time_count,
    int64_t *raw_score,
    int32_t *contrast_codes)
{
    rf_v20_support_t support;
    int64_t inside;
    int64_t outside;
    int64_t numerator;
    int32_t outside_count = 2 * RF_V20_VIDEO_TEMPLATE_FLANK_BINS;
    int32_t denominator;
    if (!rf_v20_template_support(center_row, width, &support) ||
        time_count <= 0 || raw_score == NULL || contrast_codes == NULL) {
        return 0;
    }
    inside = rf_v20_prefix_region(prefix, support.start, support.end);
    outside =
        rf_v20_prefix_region(prefix, support.outer_start, support.start) +
        rf_v20_prefix_region(prefix, support.end, support.outer_end);
    numerator = inside * outside_count - outside * width;
    denominator = width * outside_count * time_count;
    *raw_score = numerator;
    *contrast_codes = rf_v20_round_divide_i64(numerator, denominator);
    return 1;
}

static int32_t rf_v20_shoulder_contrast(
    int64_t region_sum,
    int32_t region_count,
    int64_t outside_sum,
    int32_t outside_count,
    int32_t time_count)
{
    int64_t numerator =
        region_sum * outside_count - outside_sum * region_count;
    int32_t denominator = region_count * outside_count * time_count;
    return rf_v20_round_divide_i64(numerator, denominator);
}

static void rf_v20_build_frequency_prefix(
    const int8_t *input,
    int32_t column_start,
    int32_t column_end,
    int32_t *prefix)
{
    int32_t row;
    prefix[0] = 0;
    for (row = 0; row < (int32_t)RF_V20_INPUT_FREQUENCY_BINS; ++row) {
        int32_t sum = 0;
        int32_t column;
        for (column = column_start; column < column_end; ++column) {
            sum += input[rf_v20_input_index(row, column)];
        }
        prefix[row + 1] = prefix[row] + sum;
    }
}

static void rf_v20_width_evidence(
    const int8_t *input,
    const rf_v20_video_event_t *event,
    rf_v20_video_width_evidence_t *evidence)
{
    int32_t row_q8;
    int32_t column_q8;
    int32_t center_row;
    int32_t center_column;
    int32_t column_start;
    int32_t column_end;
    int32_t time_count;
    int32_t prefix[RF_V20_INPUT_FREQUENCY_BINS + 1u];
    rf_v20_support_t current_support;
    rf_v20_support_t wide_support;
    int64_t current_raw;
    int32_t current_codes;
    int64_t best_raw = INT64_MIN;
    int32_t best_codes = 0;
    int32_t best_row = 0;
    int32_t candidate_row;
    int have_best = 0;

    memset(evidence, 0, sizeof(*evidence));
    row_q8 = rf_v20_round_divide_i64(
                 (int64_t)(event->center_frequency_offset_hz +
                           RF_V20_ANALYSIS_BANDWIDTH_HZ / 2) *
                     RF_V20_INPUT_FREQUENCY_BINS * RF_V20_Q8_ONE,
                 RF_V20_ANALYSIS_BANDWIDTH_HZ) -
             RF_V20_Q8_ONE / 2;
    column_q8 = rf_v20_round_divide_i64(
                    (int64_t)event->center_sample * RF_V20_INPUT_TIME_BINS *
                        RF_V20_Q8_ONE,
                    RF_V20_TILE_SAMPLES) -
                RF_V20_Q8_ONE / 2;
    center_row = rf_v20_round_divide_i64(
        row_q8 - RF_V20_Q8_ONE / 2, RF_V20_Q8_ONE);
    center_column = rf_v20_round_divide_i64(column_q8, RF_V20_Q8_ONE);
    column_start = center_column - RF_V20_VIDEO_TIME_RADIUS_BINS;
    column_end = center_column + RF_V20_VIDEO_TIME_RADIUS_BINS + 1;
    if (column_start < 0) {
        column_start = 0;
    }
    if (column_end > (int32_t)RF_V20_INPUT_TIME_BINS) {
        column_end = (int32_t)RF_V20_INPUT_TIME_BINS;
    }
    time_count = column_end - column_start;
    if (time_count <= 0) {
        return;
    }
    rf_v20_build_frequency_prefix(input, column_start, column_end, prefix);
    if (!rf_v20_template_support(
            center_row, RF_V20_VIDEO_10MHZ_BINS, &current_support) ||
        !rf_v20_template_support(
            center_row, RF_V20_VIDEO_20MHZ_BINS, &wide_support) ||
        !rf_v20_template_contrast(
            prefix, center_row, RF_V20_VIDEO_10MHZ_BINS, time_count,
            &current_raw, &current_codes)) {
        return;
    }

    for (candidate_row = center_row - RF_V20_VIDEO_SEARCH_RADIUS_BINS;
         candidate_row <= center_row + RF_V20_VIDEO_SEARCH_RADIUS_BINS;
         ++candidate_row) {
        int64_t raw;
        int32_t codes;
        int32_t distance;
        int32_t best_distance;
        if (!rf_v20_template_contrast(
                prefix, candidate_row, RF_V20_VIDEO_20MHZ_BINS, time_count,
                &raw, &codes)) {
            continue;
        }
        if (evidence->searched_center_count < UINT8_MAX) {
            evidence->searched_center_count++;
        }
        distance = abs(candidate_row - center_row);
        best_distance = abs(best_row - center_row);
        if (!have_best || raw > best_raw ||
            (raw == best_raw && distance < best_distance) ||
            (raw == best_raw && distance == best_distance &&
             candidate_row < best_row)) {
            have_best = 1;
            best_raw = raw;
            best_codes = codes;
            best_row = candidate_row;
        }
    }
    if (!have_best) {
        evidence->current_10mhz_contrast_codes = (int16_t)current_codes;
        return;
    }
    evidence->current_10mhz_contrast_codes = (int16_t)current_codes;
    evidence->best_20mhz_contrast_codes = (int16_t)best_codes;
    evidence->best_20mhz_shift_bins = (int8_t)(best_row - center_row);
    evidence->full_frequency_support = 1u;
    evidence->base_upgrade_to_20mhz =
        (uint8_t)(best_codes > 0 &&
                  current_codes < RF_V20_VIDEO_10MHZ_MAX_CONTRAST_CODES &&
                  (int64_t)best_codes * RF_V20_VIDEO_20MHZ_GAIN_DENOMINATOR >=
                      (int64_t)current_codes *
                          RF_V20_VIDEO_20MHZ_GAIN_NUMERATOR);

    if (rf_v20_template_support(
            best_row, RF_V20_VIDEO_20MHZ_BINS, &wide_support) &&
        rf_v20_template_support(
            best_row, RF_V20_VIDEO_10MHZ_BINS, &current_support)) {
        int64_t outside_sum =
            rf_v20_prefix_region(
                prefix, wide_support.outer_start, wide_support.start) +
            rf_v20_prefix_region(
                prefix, wide_support.end, wide_support.outer_end);
        int32_t outside_count = 2 * RF_V20_VIDEO_TEMPLATE_FLANK_BINS;
        int32_t left_count = current_support.start - wide_support.start;
        int32_t right_count = wide_support.end - current_support.end;
        int32_t left_codes = rf_v20_shoulder_contrast(
            rf_v20_prefix_region(
                prefix, wide_support.start, current_support.start),
            left_count, outside_sum, outside_count, time_count);
        int32_t right_codes = rf_v20_shoulder_contrast(
            rf_v20_prefix_region(
                prefix, current_support.end, wide_support.end),
            right_count, outside_sum, outside_count, time_count);
        evidence->left_shoulder_contrast_codes = (int16_t)left_codes;
        evidence->right_shoulder_contrast_codes = (int16_t)right_codes;
        evidence->bilateral_upgrade_to_20mhz =
            (uint8_t)(best_codes >= RF_V20_BILATERAL_MIN_CONTRAST_CODES &&
                      left_codes >= RF_V20_BILATERAL_MIN_SHOULDER_CODES &&
                      right_codes >= RF_V20_BILATERAL_MIN_SHOULDER_CODES);
    }
}

static int16_t rf_v20_frequency_bias_q8(
    uint64_t capture_center_frequency_hz,
    const rf_v20_video_event_t *event)
{
    int32_t half_bandwidth = event->bandwidth_hz / 2;
    int high_edge = event->center_frequency_offset_hz + half_bandwidth >
                    RF_V20_RELIABLE_HALF_BANDWIDTH_HZ;
    int low_edge = event->center_frequency_offset_hz - half_bandwidth <
                   -RF_V20_RELIABLE_HALF_BANDWIDTH_HZ;
    if (capture_center_frequency_hz == UINT64_C(2420000000) &&
        !high_edge && !low_edge) {
        return INT16_C(192);
    }
    if (capture_center_frequency_hz == UINT64_C(2464000000) &&
        !high_edge && !low_edge) {
        return INT16_C(-672);
    }
    if (capture_center_frequency_hz == UINT64_C(5760000000) && high_edge) {
        return INT16_C(-3136);
    }
    if (capture_center_frequency_hz == UINT64_C(5816000000) && high_edge) {
        return INT16_C(-2144);
    }
    if (capture_center_frequency_hz == UINT64_C(5816000000) && low_edge) {
        return INT16_C(512);
    }
    return 0;
}

int rf_v20_video_heatmap_accepts(int8_t raw_logit)
{
    return raw_logit >= RF_V20_VIDEO_HEATMAP_THRESHOLD_Q8;
}

uint8_t rf_v20_video_score_tier(int8_t raw_logit)
{
    if (raw_logit >= RF_V20_VIDEO_SCORE_090_Q8) {
        return 3u;
    }
    if (raw_logit >= RF_V20_VIDEO_SCORE_075_Q8) {
        return 2u;
    }
    if (raw_logit >= RF_V20_VIDEO_SCORE_055_Q8) {
        return 1u;
    }
    return 0u;
}

int rf_v20_video_postprocess(
    const int8_t *input_nhwc,
    size_t input_bytes,
    uint64_t capture_center_frequency_hz,
    const rf_v20_video_event_t *input_event,
    rf_v20_video_postprocess_result_t *result)
{
    int upgrade;
    if (input_nhwc == NULL || input_event == NULL || result == NULL ||
        input_bytes != RF_V20_INPUT_BYTES ||
        (input_event->bandwidth_hz != RF_V20_VIDEO_10MHZ_HZ &&
         input_event->bandwidth_hz != RF_V20_VIDEO_20MHZ_HZ)) {
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->event = *input_event;
    if (result->event.bandwidth_hz == RF_V20_VIDEO_10MHZ_HZ) {
        rf_v20_width_evidence(input_nhwc, &result->event, &result->width);
        upgrade = result->width.base_upgrade_to_20mhz != 0u ||
                  result->width.bilateral_upgrade_to_20mhz != 0u;
        if (upgrade) {
            result->width_shift_hz = rf_v20_round_divide_i64(
                (int64_t)result->width.best_20mhz_shift_bins *
                    RF_V20_ANALYSIS_BANDWIDTH_HZ,
                RF_V20_INPUT_FREQUENCY_BINS);
            result->event.center_frequency_offset_hz +=
                result->width_shift_hz;
            result->event.bandwidth_hz = RF_V20_VIDEO_20MHZ_HZ;
            result->width_upgraded = 1u;
        }
    } else {
        result->width.full_frequency_support = 1u;
    }
    result->frequency_bias_q8 = rf_v20_frequency_bias_q8(
        capture_center_frequency_hz, &result->event);
    result->frequency_bias_shift_hz = rf_v20_round_divide_i64(
        (int64_t)result->frequency_bias_q8 * RF_V20_ANALYSIS_BANDWIDTH_HZ,
        RF_V20_Q8_ONE * RF_V20_INPUT_FREQUENCY_BINS);
    result->event.center_frequency_offset_hz +=
        result->frequency_bias_shift_hz;
    return 1;
}
