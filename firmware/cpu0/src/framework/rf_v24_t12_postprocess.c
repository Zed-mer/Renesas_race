#include "rf_v24_t12_postprocess.h"

#include "rf_v24_t12_confidence_calibration.h"

#include <stdint.h>
#include <string.h>

#define RF_V24_T12_ANALYSIS_BANDWIDTH_HZ INT32_C(60000000)
#define RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ INT32_C(30000000)

#include "rf_v24_t12_score_lut.inc"

typedef struct rf_v24_t12_candidate {
    rf_v24_t12_event_t event;
    uint16_t insertion_order;
} rf_v24_t12_candidate_t;

static int32_t rf_v24_t12_round_divide_i64(
    int64_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return (int32_t)(-(((-numerator) + denominator / 2) / denominator));
}

static int rf_v24_t12_is_local_maximum(
    const int8_t *heatmap, int32_t row, int32_t column)
{
    int32_t row_begin = row > 0 ? row - 1 : 0;
    int32_t row_end = row + 1 < (int32_t)RF_V24_T12_HEATMAP_FREQUENCY_BINS
                          ? row + 1
                          : (int32_t)RF_V24_T12_HEATMAP_FREQUENCY_BINS - 1;
    int32_t column_begin = column > 0 ? column - 1 : 0;
    int32_t column_end = column + 1 < (int32_t)RF_V24_T12_HEATMAP_TIME_BINS
                             ? column + 1
                             : (int32_t)RF_V24_T12_HEATMAP_TIME_BINS - 1;
    int8_t current = heatmap[row * (int32_t)RF_V24_T12_HEATMAP_TIME_BINS + column];
    int32_t neighbor_row;
    for (neighbor_row = row_begin; neighbor_row <= row_end; ++neighbor_row) {
        int32_t neighbor_column;
        for (neighbor_column = column_begin; neighbor_column <= column_end;
             ++neighbor_column) {
            if (neighbor_row == row && neighbor_column == column) {
                continue;
            }
            if (heatmap[neighbor_row *
                            (int32_t)RF_V24_T12_HEATMAP_TIME_BINS +
                        neighbor_column] > current) {
                return 0;
            }
        }
    }
    return 1;
}

uint16_t rf_v24_t12_specialist_score_q15(int8_t raw_logit)
{
    return g_rf_v24_t12_specialist_score_q15[(int32_t)raw_logit + 128];
}

uint16_t rf_v24_t12_v21_score_q15(int8_t raw_logit)
{
    return g_rf_v24_t12_v21_score_q15[(int32_t)raw_logit + 128];
}

static rf_v24_t12_event_t rf_v24_t12_make_event(
    int32_t row,
    int32_t column,
    uint16_t score_q15,
    uint8_t source_mask)
{
    rf_v24_t12_event_t event;
    int64_t start_numerator;
    int64_t low_numerator;
    memset(&event, 0, sizeof(event));
    start_numerator =
        (int64_t)(2 * column + 1) * RF_V24_T12_TILE_SAMPLES -
        (int64_t)RF_V24_T12_DURATION_SAMPLES *
            RF_V24_T12_HEATMAP_TIME_BINS;
    event.canonical_start_sample = rf_v24_t12_round_divide_i64(
        start_numerator, 2 * (int32_t)RF_V24_T12_HEATMAP_TIME_BINS);
    event.canonical_end_sample =
        event.canonical_start_sample + RF_V24_T12_DURATION_SAMPLES;
    event.center_sample =
        event.canonical_start_sample + RF_V24_T12_DURATION_SAMPLES / 2;

    low_numerator =
        (int64_t)(2 * row + 1) * RF_V24_T12_ANALYSIS_BANDWIDTH_HZ -
        (int64_t)(RF_V24_T12_ANALYSIS_BANDWIDTH_HZ +
                  RF_V24_T12_BANDWIDTH_HZ) *
            RF_V24_T12_HEATMAP_FREQUENCY_BINS;
    event.canonical_frequency_low_offset_hz = rf_v24_t12_round_divide_i64(
        low_numerator,
        2 * (int32_t)RF_V24_T12_HEATMAP_FREQUENCY_BINS);
    event.canonical_frequency_high_offset_hz =
        event.canonical_frequency_low_offset_hz + RF_V24_T12_BANDWIDTH_HZ;
    event.center_frequency_offset_hz =
        event.canonical_frequency_low_offset_hz + RF_V24_T12_BANDWIDTH_HZ / 2;

    event.visible_start_sample = event.canonical_start_sample < 0
                                     ? 0
                                     : event.canonical_start_sample;
    event.visible_end_sample =
        event.canonical_end_sample > (int32_t)RF_V24_T12_TILE_SAMPLES
            ? (int32_t)RF_V24_T12_TILE_SAMPLES
            : event.canonical_end_sample;
    event.visible_frequency_low_offset_hz =
        event.canonical_frequency_low_offset_hz <
                -RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ
            ? -RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ
            : event.canonical_frequency_low_offset_hz;
    event.visible_frequency_high_offset_hz =
        event.canonical_frequency_high_offset_hz >
                RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ
            ? RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ
            : event.canonical_frequency_high_offset_hz;
    if (event.canonical_start_sample < 0) {
        event.event_flags |= RF_V24_T12_TIME_LEFT_CLIPPED;
    }
    if (event.canonical_end_sample > (int32_t)RF_V24_T12_TILE_SAMPLES) {
        event.event_flags |= RF_V24_T12_TIME_RIGHT_CLIPPED;
    }
    if (event.canonical_frequency_low_offset_hz <
        -RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ) {
        event.event_flags |= RF_V24_T12_FREQUENCY_LOW_CLIPPED;
    }
    if (event.canonical_frequency_high_offset_hz >
        RF_V24_T12_HALF_ANALYSIS_BANDWIDTH_HZ) {
        event.event_flags |= RF_V24_T12_FREQUENCY_HIGH_CLIPPED;
    }
    event.raw_score_q15 = score_q15;
    event.calibrated_confidence_q15 =
        rf_v24_t12_calibrate_confidence_q15(score_q15);
    event.source_mask = source_mask;
    event.heatmap_row = (uint8_t)row;
    event.heatmap_column = (uint8_t)column;
    return event;
}

static void rf_v24_t12_insert_top_k(
    rf_v24_t12_candidate_t *values,
    size_t *count,
    const rf_v24_t12_candidate_t *candidate)
{
    size_t position = 0u;
    while (position < *count &&
           values[position].event.raw_score_q15 >=
               candidate->event.raw_score_q15) {
        ++position;
    }
    if (position >= RF_V24_T12_TOP_K_PER_SOURCE) {
        return;
    }
    if (*count < RF_V24_T12_TOP_K_PER_SOURCE) {
        ++*count;
    }
    if (position + 1u < *count) {
        memmove(
            &values[position + 1u],
            &values[position],
            (*count - position - 1u) * sizeof(values[0]));
    }
    values[position] = *candidate;
}

static size_t rf_v24_t12_source_candidates(
    const int8_t *heatmap,
    uint8_t source_mask,
    rf_v24_t12_candidate_t *candidates,
    uint16_t *insertion_order)
{
    size_t count = 0u;
    int32_t row;
    for (row = 0; row < (int32_t)RF_V24_T12_HEATMAP_FREQUENCY_BINS; ++row) {
        int32_t column;
        for (column = 0; column < (int32_t)RF_V24_T12_HEATMAP_TIME_BINS;
             ++column) {
            int8_t raw;
            uint16_t score;
            rf_v24_t12_candidate_t candidate;
            if (!rf_v24_t12_is_local_maximum(heatmap, row, column)) {
                continue;
            }
            raw = heatmap[row *
                              (int32_t)RF_V24_T12_HEATMAP_TIME_BINS +
                          column];
            score = source_mask == RF_V24_T12_SOURCE_SPECIALIST
                        ? rf_v24_t12_specialist_score_q15(raw)
                        : rf_v24_t12_v21_score_q15(raw);
            if (score < RF_V24_T12_EVENT_THRESHOLD_Q15) {
                continue;
            }
            candidate.event =
                rf_v24_t12_make_event(row, column, score, source_mask);
            candidate.insertion_order = (*insertion_order)++;
            rf_v24_t12_insert_top_k(candidates, &count, &candidate);
        }
    }
    return count;
}

static int rf_v24_t12_candidate_before(
    const rf_v24_t12_candidate_t *left,
    const rf_v24_t12_candidate_t *right)
{
    if (left->event.raw_score_q15 != right->event.raw_score_q15) {
        return left->event.raw_score_q15 > right->event.raw_score_q15;
    }
    return left->insertion_order < right->insertion_order;
}

static void rf_v24_t12_sort_candidates(
    rf_v24_t12_candidate_t *values, size_t count)
{
    size_t index;
    for (index = 1u; index < count; ++index) {
        rf_v24_t12_candidate_t value = values[index];
        size_t position = index;
        while (position > 0u &&
               rf_v24_t12_candidate_before(&value, &values[position - 1u])) {
            values[position] = values[position - 1u];
            --position;
        }
        values[position] = value;
    }
}

static int rf_v24_t12_overlaps_nms(
    const rf_v24_t12_event_t *left, const rf_v24_t12_event_t *right)
{
    int32_t time_start = left->canonical_start_sample >
                                 right->canonical_start_sample
                             ? left->canonical_start_sample
                             : right->canonical_start_sample;
    int32_t time_end = left->canonical_end_sample < right->canonical_end_sample
                           ? left->canonical_end_sample
                           : right->canonical_end_sample;
    int32_t frequency_start =
        left->canonical_frequency_low_offset_hz >
                right->canonical_frequency_low_offset_hz
            ? left->canonical_frequency_low_offset_hz
            : right->canonical_frequency_low_offset_hz;
    int32_t frequency_end =
        left->canonical_frequency_high_offset_hz <
                right->canonical_frequency_high_offset_hz
            ? left->canonical_frequency_high_offset_hz
            : right->canonical_frequency_high_offset_hz;
    int64_t intersection;
    int64_t area =
        (int64_t)RF_V24_T12_DURATION_SAMPLES * RF_V24_T12_BANDWIDTH_HZ;
    int64_t union_area;
    if (time_end <= time_start || frequency_end <= frequency_start) {
        return 0;
    }
    intersection =
        (int64_t)(time_end - time_start) * (frequency_end - frequency_start);
    union_area = 2 * area - intersection;
    return intersection * 10 >= union_area * 3;
}

size_t rf_v24_t12_postprocess(
    const int8_t *v21_heatmap,
    size_t v21_heatmap_bytes,
    const int8_t *specialist_heatmap,
    size_t specialist_heatmap_bytes,
    uint64_t capture_center_frequency_hz,
    rf_v24_t12_event_t *events,
    size_t event_capacity)
{
    rf_v24_t12_candidate_t candidates[RF_V24_T12_MAX_EVENTS];
    uint16_t insertion_order = 0u;
    size_t count;
    size_t specialist_count;
    size_t index;
    size_t selected = 0u;
    if (v21_heatmap == NULL || specialist_heatmap == NULL || events == NULL ||
        v21_heatmap_bytes != RF_V24_T12_HEATMAP_BYTES ||
        specialist_heatmap_bytes != RF_V24_T12_HEATMAP_BYTES ||
        event_capacity == 0u ||
        (capture_center_frequency_hz != UINT64_C(2420000000) &&
         capture_center_frequency_hz != UINT64_C(2464000000))) {
        return 0u;
    }
    specialist_count = rf_v24_t12_source_candidates(
        specialist_heatmap,
        RF_V24_T12_SOURCE_SPECIALIST,
        candidates,
        &insertion_order);
    count = specialist_count + rf_v24_t12_source_candidates(
                                     v21_heatmap,
                                     RF_V24_T12_SOURCE_V21,
                                     &candidates[specialist_count],
                                     &insertion_order);
    rf_v24_t12_sort_candidates(candidates, count);
    for (index = 0u; index < count; ++index) {
        size_t output_index;
        int suppressed = 0;
        for (output_index = 0u; output_index < selected; ++output_index) {
            if (rf_v24_t12_overlaps_nms(
                    &candidates[index].event, &events[output_index])) {
                events[output_index].source_mask |=
                    candidates[index].event.source_mask;
                suppressed = 1;
                break;
            }
        }
        if (!suppressed && selected < event_capacity) {
            events[selected++] = candidates[index].event;
        }
    }
    return selected;
}
