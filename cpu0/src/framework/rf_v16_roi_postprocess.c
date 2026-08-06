#include "rf_v16_roi_postprocess.h"

#include <limits.h>
#include <string.h>

#define RF_V16_ANALYSIS_BANDWIDTH_HZ INT32_C(60000000)
#define RF_V16_TILE_SAMPLES INT32_C(590336)
#define RF_V16_INPUT_ZERO_POINT INT32_C(-1)
#define RF_V16_VIDEO_20MHZ_FLAG UINT8_C(0x04)
#define RF_V16_INVALID_SCORE INT32_C(-1073741824)

static const int32_t rf_v16_channel_mean_q20[4] = {
    INT32_C(622842), INT32_C(3228918), INT32_C(24), INT32_C(-46)
};

static const int32_t rf_v16_channel_step_q20[4] = {
    INT32_C(520844), INT32_C(137434), INT32_C(139997), INT32_C(180931)
};

static const int32_t rf_v16_bandwidth_hz[RF_V16_CLASS_COUNT] = {
    INT32_C(2200000), INT32_C(10000000), INT32_C(8000000),
    INT32_C(1700000), INT32_C(2400000)
};

static const int32_t rf_v16_duration_samples[RF_V16_CLASS_COUNT] = {
    INT32_C(31200), INT32_C(66000), INT32_C(123000),
    INT32_C(276000), INT32_C(123000)
};

typedef struct rf_v16_slice {
    int32_t start;
    int32_t end;
} rf_v16_slice_t;

typedef struct rf_v16_geometry {
    int32_t row_q8;
    int32_t column_q8;
    int32_t height_q8;
    int32_t width_q8;
} rf_v16_geometry_t;

static int32_t rf_v16_round_divide_i64(int64_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return (int32_t)(-(((-numerator) + denominator / 2) / denominator));
}

static int32_t rf_v16_ceil_divide_i32(int32_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (numerator + denominator - 1) / denominator;
    }
    return -((-numerator) / denominator);
}

static int16_t rf_v16_clip_i16(int32_t value)
{
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)value;
}

static uint16_t rf_v16_clip_u16(int32_t value)
{
    if (value < 0) {
        return 0u;
    }
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static int32_t rf_v16_abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t rf_v17_round_divide_even_i64(
    int64_t numerator, int64_t denominator)
{
    uint64_t magnitude;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t positive_denominator;
    int negative;
    if (denominator <= 0) {
        return 0;
    }
    negative = numerator < 0;
    magnitude = negative ? (uint64_t)(-numerator) : (uint64_t)numerator;
    positive_denominator = (uint64_t)denominator;
    quotient = magnitude / positive_denominator;
    remainder = magnitude % positive_denominator;
    if (remainder * 2u > positive_denominator ||
        (remainder * 2u == positive_denominator &&
         (quotient & 1u) != 0u)) {
        quotient += 1u;
    }
    return negative ? -(int32_t)quotient : (int32_t)quotient;
}

static rf_v16_slice_t rf_v16_slice_q8(
    int32_t center_q8,
    int32_t size_q8,
    int32_t limit)
{
    rf_v16_slice_t result;
    result.start = rf_v16_ceil_divide_i32(
        2 * center_q8 - size_q8, 2 * RF_V16_Q8_ONE);
    result.end = (2 * center_q8 + size_q8) / (2 * RF_V16_Q8_ONE) + 1;
    if (2 * center_q8 + size_q8 < 0 &&
        (2 * center_q8 + size_q8) % (2 * RF_V16_Q8_ONE) != 0) {
        --result.end;
    }
    if (result.start < 0) {
        result.start = 0;
    } else if (result.start > limit) {
        result.start = limit;
    }
    if (result.end < result.start) {
        result.end = result.start;
    } else if (result.end > limit) {
        result.end = limit;
    }
    return result;
}

static size_t rf_v16_input_index(int32_t row, int32_t column, int32_t channel)
{
    return (size_t)(((row * (int32_t)RF_V16_INPUT_TIME_BINS) + column) *
                    (int32_t)RF_V16_INPUT_CHANNELS + channel);
}

static int64_t rf_v16_sum_channel_q20(
    const int8_t *input,
    int32_t channel,
    rf_v16_slice_t rows,
    rf_v16_slice_t columns,
    int absolute)
{
    int64_t total = 0;
    int32_t row;
    for (row = rows.start; row < rows.end; ++row) {
        int32_t column;
        for (column = columns.start; column < columns.end; ++column) {
            int32_t raw = input[rf_v16_input_index(row, column, channel)];
            int64_t value =
                (int64_t)rf_v16_channel_mean_q20[channel] +
                (int64_t)(raw - RF_V16_INPUT_ZERO_POINT) *
                    rf_v16_channel_step_q20[channel];
            if (absolute && value < 0) {
                value = -value;
            }
            total += value;
        }
    }
    return total;
}

static int32_t rf_v16_area(rf_v16_slice_t rows, rf_v16_slice_t columns)
{
    return (rows.end - rows.start) * (columns.end - columns.start);
}

static int32_t rf_v16_mean_channel_q8(
    const int8_t *input,
    int32_t channel,
    rf_v16_slice_t rows,
    rf_v16_slice_t columns)
{
    int32_t count = rf_v16_area(rows, columns);
    int64_t total;
    int32_t mean_q20;
    if (count <= 0) {
        return 0;
    }
    total = rf_v16_sum_channel_q20(input, channel, rows, columns, 0);
    mean_q20 = rf_v16_round_divide_i64(total, count);
    return rf_v16_round_divide_i64(mean_q20, INT32_C(4096));
}

static int32_t rf_v16_sum_channel_q8(
    const int8_t *input,
    int32_t channel,
    rf_v16_slice_t rows,
    rf_v16_slice_t columns)
{
    return rf_v16_round_divide_i64(
        rf_v16_sum_channel_q20(input, channel, rows, columns, 0),
        INT32_C(4096));
}

static int32_t rf_v16_mean_abs_channel_q8(
    const int8_t *input,
    int32_t channel,
    rf_v16_slice_t rows,
    rf_v16_slice_t columns)
{
    int32_t count = rf_v16_area(rows, columns);
    int64_t total;
    int32_t mean_q20;
    if (count <= 0) {
        return 0;
    }
    total = rf_v16_sum_channel_q20(input, channel, rows, columns, 1);
    mean_q20 = rf_v16_round_divide_i64(total, count);
    return rf_v16_round_divide_i64(mean_q20, INT32_C(4096));
}

static int rf_v16_compute_statistics(
    const int8_t *input,
    const rf_v16_geometry_t *geometry,
    const rf_v16_center_config_t *config,
    rf_v16_roi_statistics_t *statistics)
{
    rf_v16_slice_t rows;
    rf_v16_slice_t columns;
    rf_v16_slice_t outer_rows;
    rf_v16_slice_t outer_columns;
    rf_v16_slice_t edge_rows;
    rf_v16_slice_t edge_columns;
    int32_t inside_count;
    int32_t outer_count;
    int32_t ring_count;
    int32_t inside_sum_c0;
    int32_t outer_sum_c0;
    int32_t inside_mean_c0;
    int32_t ring_mean_c0;
    int32_t frequency_low;
    int32_t frequency_high;
    int32_t time_low;
    int32_t time_high;
    int32_t low_mean;
    int32_t high_mean;
    int32_t occupied = 0;
    int32_t row;
    int64_t expected_area_q16;
    int64_t visible_numerator;

    rows = rf_v16_slice_q8(
        geometry->row_q8, geometry->height_q8,
        (int32_t)RF_V16_INPUT_FREQUENCY_BINS);
    columns = rf_v16_slice_q8(
        geometry->column_q8, geometry->width_q8,
        (int32_t)RF_V16_INPUT_TIME_BINS);
    inside_count = rf_v16_area(rows, columns);
    outer_rows = rf_v16_slice_q8(
        geometry->row_q8,
        geometry->height_q8 +
            2 * (int32_t)config->ring_frequency_bins * RF_V16_Q8_ONE,
        (int32_t)RF_V16_INPUT_FREQUENCY_BINS);
    outer_columns = rf_v16_slice_q8(
        geometry->column_q8,
        geometry->width_q8 +
            2 * (int32_t)config->ring_time_bins * RF_V16_Q8_ONE,
        (int32_t)RF_V16_INPUT_TIME_BINS);
    outer_count = rf_v16_area(outer_rows, outer_columns);
    ring_count = outer_count - inside_count;
    if (ring_count < 0) {
        ring_count = 0;
    }
    inside_sum_c0 = rf_v16_sum_channel_q8(
        input, 0, rows, columns);
    outer_sum_c0 = rf_v16_sum_channel_q8(
        input, 0, outer_rows, outer_columns);
    inside_mean_c0 = rf_v16_round_divide_i64(
        inside_sum_c0, inside_count > 0 ? inside_count : 1);
    ring_mean_c0 = ring_count > 0
                       ? rf_v16_round_divide_i64(
                             outer_sum_c0 - inside_sum_c0, ring_count)
                       : 0;
    statistics->contrast_q8 = rf_v16_clip_i16(
        inside_mean_c0 - ring_mean_c0);

    frequency_low = rf_v16_round_divide_i64(
        2 * geometry->row_q8 - geometry->height_q8,
        2 * RF_V16_Q8_ONE);
    frequency_high = rf_v16_round_divide_i64(
        2 * geometry->row_q8 + geometry->height_q8,
        2 * RF_V16_Q8_ONE);
    edge_columns = rf_v16_slice_q8(
        geometry->column_q8,
        geometry->width_q8 * 3 / 4 > RF_V16_Q8_ONE
            ? geometry->width_q8 * 3 / 4
            : RF_V16_Q8_ONE,
        (int32_t)RF_V16_INPUT_TIME_BINS);
    low_mean = 0;
    high_mean = 0;
    if (frequency_low >= 0 &&
        frequency_low < (int32_t)RF_V16_INPUT_FREQUENCY_BINS) {
        rf_v16_slice_t one = {frequency_low, frequency_low + 1};
        low_mean = rf_v16_mean_channel_q8(input, 2, one, edge_columns);
    }
    if (frequency_high >= 0 &&
        frequency_high < (int32_t)RF_V16_INPUT_FREQUENCY_BINS) {
        rf_v16_slice_t one = {frequency_high, frequency_high + 1};
        high_mean = rf_v16_mean_channel_q8(input, 2, one, edge_columns);
    }
    statistics->frequency_edge_q8 = rf_v16_clip_i16(
        rf_v16_round_divide_i64(low_mean - high_mean, 2));

    time_low = rf_v16_round_divide_i64(
        2 * geometry->column_q8 - geometry->width_q8,
        2 * RF_V16_Q8_ONE);
    time_high = rf_v16_round_divide_i64(
        2 * geometry->column_q8 + geometry->width_q8,
        2 * RF_V16_Q8_ONE);
    edge_rows = rf_v16_slice_q8(
        geometry->row_q8,
        geometry->height_q8 * 3 / 4 > RF_V16_Q8_ONE
            ? geometry->height_q8 * 3 / 4
            : RF_V16_Q8_ONE,
        (int32_t)RF_V16_INPUT_FREQUENCY_BINS);
    low_mean = 0;
    high_mean = 0;
    if (time_low >= 0 && time_low < (int32_t)RF_V16_INPUT_TIME_BINS) {
        rf_v16_slice_t one = {time_low, time_low + 1};
        low_mean = rf_v16_mean_channel_q8(input, 3, edge_rows, one);
    }
    if (time_high >= 0 && time_high < (int32_t)RF_V16_INPUT_TIME_BINS) {
        rf_v16_slice_t one = {time_high, time_high + 1};
        high_mean = rf_v16_mean_channel_q8(input, 3, edge_rows, one);
    }
    statistics->time_edge_q8 = rf_v16_clip_i16(
        rf_v16_round_divide_i64(low_mean - high_mean, 2));
    statistics->burstiness_q8 = rf_v16_clip_i16(
        rf_v16_mean_channel_q8(input, 1, rows, columns));
    statistics->texture_q8 = rf_v16_clip_i16(
        rf_v16_round_divide_i64(
            rf_v16_mean_abs_channel_q8(input, 2, rows, columns) +
                rf_v16_mean_abs_channel_q8(input, 3, rows, columns),
            2));

    for (row = rows.start; row < rows.end; ++row) {
        int32_t column;
        for (column = columns.start; column < columns.end; ++column) {
            int32_t raw = input[rf_v16_input_index(row, column, 0)];
            int64_t value_q20 = (int64_t)rf_v16_channel_mean_q20[0] +
                                (int64_t)(raw - RF_V16_INPUT_ZERO_POINT) *
                                    rf_v16_channel_step_q20[0];
            if (value_q20 >
                (int64_t)(ring_mean_c0 + RF_V16_Q8_ONE) * INT32_C(4096)) {
                ++occupied;
            }
        }
    }
    statistics->occupancy_q15 = rf_v16_clip_u16(
        rf_v16_round_divide_i64(
            (int64_t)occupied * RF_V16_Q15_ONE,
            inside_count > 0 ? inside_count : 1));
    expected_area_q16 =
        (int64_t)geometry->height_q8 * geometry->width_q8;
    visible_numerator =
        (int64_t)inside_count * RF_V16_Q15_ONE * RF_V16_Q8_ONE *
        RF_V16_Q8_ONE;
    statistics->visible_fraction_q15 = rf_v16_clip_u16(
        expected_area_q16 > 0
            ? rf_v16_round_divide_i64(
                  visible_numerator, (int32_t)expected_area_q16)
            : 0);
    if (statistics->visible_fraction_q15 > RF_V16_Q15_ONE) {
        statistics->visible_fraction_q15 = RF_V16_Q15_ONE;
    }
    return inside_count > 0;
}

int32_t rf_v16_linear_score_q8(
    const rf_v16_roi_statistics_t *statistics,
    const rf_v16_linear_score_config_t *config)
{
    int32_t occupancy_q8;
    int64_t total_q16;
    if (statistics == NULL || config == NULL) {
        return RF_V16_INVALID_SCORE;
    }
    occupancy_q8 = rf_v16_round_divide_i64(
        (int64_t)statistics->occupancy_q15 * RF_V16_Q8_ONE,
        RF_V16_Q15_ONE);
    total_q16 =
        (int64_t)config->contrast_weight_q8 * statistics->contrast_q8 +
        (int64_t)config->frequency_edge_weight_q8 *
            statistics->frequency_edge_q8 +
        (int64_t)config->time_edge_weight_q8 * statistics->time_edge_q8 +
        (int64_t)config->burstiness_weight_q8 * statistics->burstiness_q8 +
        (int64_t)config->occupancy_weight_q8 * occupancy_q8 +
        (int64_t)config->texture_weight_q8 * statistics->texture_q8;
    return rf_v16_round_divide_i64(total_q16, RF_V16_Q8_ONE);
}

int rf_v16_composite_accepts(
    const rf_v16_roi_statistics_t *statistics,
    const rf_v16_composite_rule_t *rule)
{
    uint16_t flags;
    if (statistics == NULL || rule == NULL) {
        return 0;
    }
    flags = rule->enabled_flags;
    if ((flags & RF_V16_RULE_MIN_FREQUENCY_EDGE) != 0u &&
        statistics->frequency_edge_q8 < rule->minimum_frequency_edge_q8) {
        return 0;
    }
    if ((flags & RF_V16_RULE_MAX_TEXTURE) != 0u &&
        statistics->texture_q8 > rule->maximum_texture_q8) {
        return 0;
    }
    if ((flags & RF_V16_RULE_MAX_BURSTINESS) != 0u &&
        statistics->burstiness_q8 > rule->maximum_burstiness_q8) {
        return 0;
    }
    if ((flags & RF_V16_RULE_MIN_CONTRAST) != 0u &&
        statistics->contrast_q8 < rule->minimum_contrast_q8) {
        return 0;
    }
    if ((flags & RF_V16_RULE_MIN_OCCUPANCY) != 0u &&
        statistics->occupancy_q15 < rule->minimum_occupancy_q15) {
        return 0;
    }
    if ((flags & RF_V16_RULE_MIN_VISIBLE_FRACTION) != 0u &&
        statistics->visible_fraction_q15 <
            rule->minimum_visible_fraction_q15) {
        return 0;
    }
    return 1;
}

static int rf_v16_gate_accepts(
    const rf_v16_roi_statistics_t *statistics,
    const rf_v16_gate_config_t *config,
    int32_t *score_q8)
{
    if (config->kind == RF_V16_GATE_COMPOSITE) {
        *score_q8 = INT32_MIN;
        return rf_v16_composite_accepts(statistics, &config->rule);
    }
    if (config->kind != RF_V16_GATE_LINEAR) {
        *score_q8 = RF_V16_INVALID_SCORE;
        return 0;
    }
    *score_q8 = rf_v16_linear_score_q8(statistics, &config->score);
    return *score_q8 >= config->threshold_q8;
}

static rf_v16_geometry_t rf_v16_candidate_geometry(
    const rf_v16_candidate_t *candidate)
{
    rf_v16_geometry_t geometry;
    int32_t bandwidth = rf_v16_bandwidth_hz[candidate->class_id];
    int32_t duration = rf_v16_duration_samples[candidate->class_id];
    if (candidate->class_id == 1u &&
        (candidate->event_flags & RF_V16_VIDEO_20MHZ_FLAG) != 0u) {
        bandwidth = INT32_C(20000000);
    }
    geometry.row_q8 = rf_v16_round_divide_i64(
                          (int64_t)(candidate->center_frequency_offset_hz +
                                    RF_V16_ANALYSIS_BANDWIDTH_HZ / 2) *
                              RF_V16_INPUT_FREQUENCY_BINS * RF_V16_Q8_ONE,
                          RF_V16_ANALYSIS_BANDWIDTH_HZ) -
                      RF_V16_Q8_ONE / 2;
    geometry.column_q8 = rf_v16_round_divide_i64(
                             (int64_t)candidate->center_sample *
                                 RF_V16_INPUT_TIME_BINS * RF_V16_Q8_ONE,
                             RF_V16_TILE_SAMPLES) -
                         RF_V16_Q8_ONE / 2;
    geometry.height_q8 = rf_v16_round_divide_i64(
        (int64_t)bandwidth * RF_V16_INPUT_FREQUENCY_BINS * RF_V16_Q8_ONE,
        RF_V16_ANALYSIS_BANDWIDTH_HZ);
    geometry.width_q8 = rf_v16_round_divide_i64(
        (int64_t)duration * RF_V16_INPUT_TIME_BINS * RF_V16_Q8_ONE,
        RF_V16_TILE_SAMPLES);
    return geometry;
}

int rf_v16_postprocess_candidate(
    const int8_t *input_nhwc,
    size_t input_bytes,
    const rf_v16_candidate_t *candidate,
    const rf_v16_class_config_t *class_configs,
    rf_v16_postprocess_result_t *result)
{
    const rf_v16_class_config_t *class_config;
    rf_v16_geometry_t original_geometry;
    rf_v16_geometry_t geometry;
    rf_v16_roi_statistics_t original_statistics;
    rf_v16_roi_statistics_t best_statistics;
    int32_t original_score;
    int32_t best_score;
    int32_t best_frequency = 0;
    int32_t best_time = 0;
    int32_t offset_frequency;
    if (input_nhwc == NULL || candidate == NULL || result == NULL ||
        input_bytes != RF_V16_INPUT_BYTES ||
        candidate->class_id >= RF_V16_CLASS_COUNT) {
        return 0;
    }
    if (class_configs == NULL) {
        class_configs = g_rf_v16_class_configs;
    }
    class_config = &class_configs[candidate->class_id];
    original_geometry = rf_v16_candidate_geometry(candidate);
    if (!rf_v16_compute_statistics(
            input_nhwc, &original_geometry, &class_config->center,
            &original_statistics)) {
        return 0;
    }
    original_score = rf_v16_linear_score_q8(
        &original_statistics, &class_config->center.score);
    if (original_statistics.visible_fraction_q15 <
        class_config->center.minimum_visible_fraction_q15) {
        original_score = RF_V16_INVALID_SCORE;
    }
    best_score = original_score;
    best_statistics = original_statistics;
    for (offset_frequency =
             -(int32_t)class_config->center.frequency_radius_bins;
         offset_frequency <=
             (int32_t)class_config->center.frequency_radius_bins;
         ++offset_frequency) {
        int32_t offset_time;
        for (offset_time = -(int32_t)class_config->center.time_radius_bins;
             offset_time <= (int32_t)class_config->center.time_radius_bins;
             ++offset_time) {
            rf_v16_roi_statistics_t statistics;
            int32_t score;
            int32_t candidate_distance;
            int32_t current_distance;
            if (offset_frequency == 0 && offset_time == 0) {
                continue;
            }
            geometry = original_geometry;
            geometry.row_q8 += offset_frequency * RF_V16_Q8_ONE;
            geometry.column_q8 += offset_time * RF_V16_Q8_ONE;
            if (!rf_v16_compute_statistics(
                    input_nhwc, &geometry, &class_config->center,
                    &statistics)) {
                continue;
            }
            score = rf_v16_linear_score_q8(
                &statistics, &class_config->center.score);
            if (statistics.visible_fraction_q15 <
                class_config->center.minimum_visible_fraction_q15) {
                score = RF_V16_INVALID_SCORE;
            }
            score -= class_config->center.movement_penalty_q8 *
                     (rf_v16_abs_i32(offset_frequency) +
                      rf_v16_abs_i32(offset_time));
            candidate_distance = rf_v16_abs_i32(offset_frequency) +
                                 rf_v16_abs_i32(offset_time);
            current_distance = rf_v16_abs_i32(best_frequency) +
                               rf_v16_abs_i32(best_time);
            if (score > best_score ||
                (score == best_score && candidate_distance < current_distance) ||
                (score == best_score && candidate_distance == current_distance &&
                 rf_v16_abs_i32(offset_time) < rf_v16_abs_i32(best_time))) {
                best_score = score;
                best_frequency = offset_frequency;
                best_time = offset_time;
                best_statistics = statistics;
            }
        }
    }
    if (best_score <
        original_score + class_config->center.minimum_score_gain_q8) {
        best_score = original_score;
        best_frequency = 0;
        best_time = 0;
        best_statistics = original_statistics;
    }
    memset(result, 0, sizeof(*result));
    result->statistics = best_statistics;
    result->offset_frequency_bins = (int8_t)best_frequency;
    result->offset_time_bins = (int8_t)best_time;
    result->frequency_shift_hz = rf_v16_round_divide_i64(
        (int64_t)best_frequency * RF_V16_ANALYSIS_BANDWIDTH_HZ,
        RF_V16_INPUT_FREQUENCY_BINS);
    result->time_shift_samples = rf_v16_round_divide_i64(
        (int64_t)best_time * RF_V16_TILE_SAMPLES,
        RF_V16_INPUT_TIME_BINS);
    result->display_accept = (uint8_t)rf_v16_gate_accepts(
        &best_statistics, &class_config->display_gate,
        &result->display_score_q8);
    result->state_roi_decision =
        rf_v16_gate_accepts(
            &best_statistics, &class_config->state_gate,
            &result->state_score_q8)
            ? RF_V16_ROI_PASS
            : RF_V16_ROI_FAIL;
    return 1;
}

static int32_t rf_v17_score_at(
    const int8_t *input_nhwc,
    const rf_v16_geometry_t *original_geometry,
    const rf_v16_center_config_t *center_config,
    uint8_t method,
    int32_t integer_frequency_offset,
    int32_t integer_time_offset)
{
    rf_v16_geometry_t geometry = *original_geometry;
    rf_v16_roi_statistics_t statistics;
    int32_t score;
    geometry.row_q8 += integer_frequency_offset * RF_V16_Q8_ONE;
    geometry.column_q8 += integer_time_offset * RF_V16_Q8_ONE;
    if (!rf_v16_compute_statistics(
            input_nhwc, &geometry, center_config, &statistics) ||
        statistics.visible_fraction_q15 <
            center_config->minimum_visible_fraction_q15) {
        return RF_V16_INVALID_SCORE;
    }
    if (method == RF_V17_SUBBIN_CONTRAST) {
        return statistics.contrast_q8;
    }
    if (method != RF_V17_SUBBIN_LOCALIZATION) {
        return RF_V16_INVALID_SCORE;
    }
    score = rf_v16_linear_score_q8(&statistics, &center_config->score);
    score -= center_config->movement_penalty_q8 *
             (rf_v16_abs_i32(integer_frequency_offset) +
              rf_v16_abs_i32(integer_time_offset));
    return score;
}

static int32_t rf_v17_parabolic_steps(
    int32_t minus,
    int32_t center,
    int32_t plus,
    const rf_v17_subbin_config_t *config)
{
    int64_t curvature;
    int64_t numerator;
    int64_t denominator;
    int32_t steps;
    int32_t maximum_steps;
    if (center < minus || center < plus ||
        minus == RF_V16_INVALID_SCORE ||
        center == RF_V16_INVALID_SCORE ||
        plus == RF_V16_INVALID_SCORE) {
        return 0;
    }
    curvature = (int64_t)minus - 2 * (int64_t)center + (int64_t)plus;
    if (curvature >= -1) {
        return 0;
    }
    numerator = (int64_t)minus - (int64_t)plus;
    denominator = 2 * curvature;
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    steps = rf_v17_round_divide_even_i64(
        numerator * config->scale_numerator * config->subdivisions,
        denominator * config->scale_denominator);
    maximum_steps = rf_v17_round_divide_even_i64(
        (int64_t)config->scale_numerator * config->subdivisions,
        2 * (int64_t)config->scale_denominator);
    if (steps > maximum_steps) {
        return maximum_steps;
    }
    if (steps < -maximum_steps) {
        return -maximum_steps;
    }
    return steps;
}

int rf_v17_postprocess_candidate(
    const int8_t *input_nhwc,
    size_t input_bytes,
    const rf_v16_candidate_t *candidate,
    const rf_v16_class_config_t *class_configs,
    const rf_v17_subbin_config_t *subbin_configs,
    rf_v16_postprocess_result_t *result)
{
    const rf_v16_class_config_t *class_config;
    const rf_v17_subbin_config_t *subbin;
    rf_v16_geometry_t original_geometry;
    int32_t center_score;
    int32_t frequency_steps;
    int32_t time_steps;
    int32_t base_frequency;
    int32_t base_time;
    if (!rf_v16_postprocess_candidate(
            input_nhwc, input_bytes, candidate, class_configs, result)) {
        return 0;
    }
    if (subbin_configs == NULL || result->display_accept == 0u) {
        return 1;
    }
    if (class_configs == NULL) {
        class_configs = g_rf_v16_class_configs;
    }
    class_config = &class_configs[candidate->class_id];
    subbin = &subbin_configs[candidate->class_id];
    if (subbin->method == RF_V17_SUBBIN_DISABLED ||
        subbin->subdivisions == 0u || subbin->scale_numerator == 0u ||
        subbin->scale_denominator == 0u) {
        return 1;
    }
    if (subbin->method != RF_V17_SUBBIN_LOCALIZATION &&
        subbin->method != RF_V17_SUBBIN_CONTRAST) {
        return 0;
    }
    original_geometry = rf_v16_candidate_geometry(candidate);
    base_frequency = result->offset_frequency_bins;
    base_time = result->offset_time_bins;
    center_score = rf_v17_score_at(
        input_nhwc, &original_geometry, &class_config->center,
        subbin->method, base_frequency, base_time);
    frequency_steps = rf_v17_parabolic_steps(
        rf_v17_score_at(
            input_nhwc, &original_geometry, &class_config->center,
            subbin->method, base_frequency - 1, base_time),
        center_score,
        rf_v17_score_at(
            input_nhwc, &original_geometry, &class_config->center,
            subbin->method, base_frequency + 1, base_time),
        subbin);
    time_steps = rf_v17_parabolic_steps(
        rf_v17_score_at(
            input_nhwc, &original_geometry, &class_config->center,
            subbin->method, base_frequency, base_time - 1),
        center_score,
        rf_v17_score_at(
            input_nhwc, &original_geometry, &class_config->center,
            subbin->method, base_frequency, base_time + 1),
        subbin);
    result->frequency_shift_hz += rf_v17_round_divide_even_i64(
        (int64_t)frequency_steps * RF_V16_ANALYSIS_BANDWIDTH_HZ,
        (int64_t)subbin->subdivisions * RF_V16_INPUT_FREQUENCY_BINS);
    result->time_shift_samples += rf_v17_round_divide_even_i64(
        (int64_t)time_steps * RF_V16_TILE_SAMPLES,
        (int64_t)subbin->subdivisions * RF_V16_INPUT_TIME_BINS);
    result->subbin_frequency_steps = (int8_t)frequency_steps;
    result->subbin_time_steps = (int8_t)time_steps;
    result->subbin_subdivisions = subbin->subdivisions;
    result->subbin_method = subbin->method;
    return 1;
}
