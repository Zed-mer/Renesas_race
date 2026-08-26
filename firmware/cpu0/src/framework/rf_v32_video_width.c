#include "rf_v32_video_width.h"

#include <string.h>

#define RF_V32_ROI_CENTER_ROW_Q8 INT32_C(12160) /* 47.5 * 256 */
#define RF_V32_ROI_CENTER_COLUMN_Q8 INT32_C(2944) /* 11.5 * 256 */
#define RF_V32_CPU_COLUMN_FIRST 6
#define RF_V32_CPU_COLUMN_END 19
#define RF_V32_CPU_COLUMN_COUNT 13
#define RF_V32_CPU_FAR_ROWS 22
#define RF_V32_CPU_SHOULDER_ROWS 26
#define RF_V32_CPU_SHOULDER_REQUIRED 17
#define RF_V32_CPU_ONE_DB_TWICE_SUM_CODES 43
#define RF_V32_EVIDENCE_LIMIT_Q8 768
#define RF_V32_SWITCH_EVIDENCE_Q8 384

static int32_t rf_v32_round_q8(int32_t value)
{
    if (value >= 0) {
        return (value + 128) / 256;
    }
    return -((-value + 128) / 256);
}

static size_t rf_v32_source_index(int32_t row, int32_t column, int32_t channel)
{
    return (size_t)(((row * (int32_t)RF_V32_SOURCE_TIME_BINS + column) *
                     (int32_t)RF_V32_SOURCE_CHANNELS) + channel);
}

static size_t rf_v32_roi_index(int32_t row, int32_t column, int32_t channel)
{
    return (size_t)(((row * (int32_t)RF_V32_ROI_TIME_BINS + column) *
                     (int32_t)RF_V32_ROI_CHANNELS) + channel);
}

int rf_v32_extract_width_roi(
    const int8_t *source_nhwc,
    size_t source_bytes,
    int32_t center_row_q8,
    int32_t center_column_q8,
    int8_t *roi_nhwc,
    size_t roi_bytes)
{
    int32_t first_row;
    int32_t first_column;
    int32_t target_row;
    if (source_nhwc == NULL || roi_nhwc == NULL ||
        source_bytes != RF_V32_SOURCE_BYTES || roi_bytes != RF_V32_ROI_BYTES) {
        return 0;
    }
    memset(roi_nhwc, 0, roi_bytes);
    for (target_row = 0; target_row < (int32_t)RF_V32_ROI_FREQUENCY_BINS;
         ++target_row) {
        int32_t column;
        for (column = 0; column < (int32_t)RF_V32_ROI_TIME_BINS; ++column) {
            roi_nhwc[rf_v32_roi_index(target_row, column, 4)] =
                RF_V32_MASK_INVALID_CODE;
        }
    }
    first_row = rf_v32_round_q8(center_row_q8 - RF_V32_ROI_CENTER_ROW_Q8);
    first_column = rf_v32_round_q8(
        center_column_q8 - RF_V32_ROI_CENTER_COLUMN_Q8);
    for (target_row = 0; target_row < (int32_t)RF_V32_ROI_FREQUENCY_BINS;
         ++target_row) {
        int32_t source_row = first_row + target_row;
        int32_t target_column;
        if (source_row < RF_V32_RELIABLE_FIRST_ROW ||
            source_row >= RF_V32_RELIABLE_END_ROW) {
            continue;
        }
        for (target_column = 0;
             target_column < (int32_t)RF_V32_ROI_TIME_BINS;
             ++target_column) {
            int32_t source_column = first_column + target_column;
            int32_t channel;
            if (source_column < 0 ||
                source_column >= (int32_t)RF_V32_SOURCE_TIME_BINS) {
                continue;
            }
            for (channel = 0; channel < (int32_t)RF_V32_SOURCE_CHANNELS;
                 ++channel) {
                roi_nhwc[rf_v32_roi_index(target_row, target_column, channel)] =
                    source_nhwc[rf_v32_source_index(
                        source_row, source_column, channel)];
            }
            roi_nhwc[rf_v32_roi_index(target_row, target_column, 4)] =
                RF_V32_MASK_VALID_CODE;
        }
    }
    return 1;
}

static int rf_v32_is_shoulder_row(int32_t row)
{
    return (row >= 16 && row <= 28) || (row >= 67 && row <= 79);
}

static int rf_v32_is_far_row(int32_t row)
{
    return (row >= 0 && row <= 10) || (row >= 85 && row <= 95);
}

static void rf_v32_sort_i16(int16_t *values, int32_t count)
{
    int32_t index;
    for (index = 1; index < count; ++index) {
        int16_t value = values[index];
        int32_t cursor = index;
        while (cursor > 0 && values[cursor - 1] > value) {
            values[cursor] = values[cursor - 1];
            --cursor;
        }
        values[cursor] = value;
    }
}

int rf_v32_cpu_width_classify(
    const int8_t *roi_nhwc,
    size_t roi_bytes,
    rf_v32_cpu_width_evidence_t *evidence)
{
    int16_t profile_sums[RF_V32_ROI_FREQUENCY_BINS];
    int16_t far_sums[RF_V32_CPU_FAR_ROWS];
    int32_t far_count = 0;
    int32_t row;
    int32_t median_twice;
    int32_t shoulder_above = 0;
    if (roi_nhwc == NULL || evidence == NULL || roi_bytes != RF_V32_ROI_BYTES) {
        return 0;
    }
    memset(evidence, 0, sizeof(*evidence));
    for (row = 0; row < (int32_t)RF_V32_ROI_FREQUENCY_BINS; ++row) {
        int32_t column;
        int32_t sum = 0;
        if (!rf_v32_is_shoulder_row(row) && !rf_v32_is_far_row(row)) {
            profile_sums[row] = 0;
            continue;
        }
        for (column = RF_V32_CPU_COLUMN_FIRST;
             column < RF_V32_CPU_COLUMN_END; ++column) {
            if (roi_nhwc[rf_v32_roi_index(row, column, 4)] !=
                RF_V32_MASK_VALID_CODE) {
                return 0;
            }
            sum += roi_nhwc[rf_v32_roi_index(row, column, 0)];
        }
        profile_sums[row] = (int16_t)sum;
        if (rf_v32_is_far_row(row)) {
            far_sums[far_count++] = (int16_t)sum;
        }
    }
    if (far_count != RF_V32_CPU_FAR_ROWS) {
        return 0;
    }
    rf_v32_sort_i16(far_sums, RF_V32_CPU_FAR_ROWS);
    median_twice = (int32_t)far_sums[10] + (int32_t)far_sums[11];
    for (row = 0; row < (int32_t)RF_V32_ROI_FREQUENCY_BINS; ++row) {
        if (rf_v32_is_shoulder_row(row) &&
            2 * (int32_t)profile_sums[row] >=
                median_twice + RF_V32_CPU_ONE_DB_TWICE_SUM_CODES) {
            ++shoulder_above;
        }
    }
    evidence->available = 1u;
    evidence->shoulder_rows_above = (uint8_t)shoulder_above;
    evidence->shoulder_rows_total = RF_V32_CPU_SHOULDER_ROWS;
    evidence->bandwidth_hz =
        shoulder_above >= RF_V32_CPU_SHOULDER_REQUIRED
            ? RF_V32_WIDTH_20MHZ_HZ
            : RF_V32_WIDTH_10MHZ_HZ;
    return 1;
}

void rf_v32_width_track_init(rf_v32_width_track_t *track)
{
    if (track != NULL) {
        memset(track, 0, sizeof(*track));
    }
}

static int16_t rf_v32_clip_evidence(int32_t value)
{
    if (value > RF_V32_EVIDENCE_LIMIT_Q8) {
        return RF_V32_EVIDENCE_LIMIT_Q8;
    }
    if (value < -RF_V32_EVIDENCE_LIMIT_Q8) {
        return -RF_V32_EVIDENCE_LIMIT_Q8;
    }
    return (int16_t)value;
}

int32_t rf_v32_width_track_apply(
    rf_v32_width_track_t *track,
    int cpu_valid,
    int32_t cpu_bandwidth_hz,
    int cnn_valid,
    int8_t cnn_output_code)
{
    int vote_wide;
    int32_t delta;
    if (track == NULL) {
        return 0;
    }
    /* CNN-first is deliberate: actual V31 proposal centres make the legacy
     * shoulder rule less reliable than the directional width specialist. */
    if (cnn_valid) {
        vote_wide = cnn_output_code >= RF_V32_CNN_DECISION_CODE;
        if (cnn_output_code <= RF_V32_CNN_CONFIDENT_10_MAX_CODE) {
            delta = -256;
        } else if (cnn_output_code >= RF_V32_CNN_CONFIDENT_20_MIN_CODE) {
            delta = 256;
        } else {
            delta = vote_wide ? 64 : -64;
        }
    } else if (cpu_valid &&
               (cpu_bandwidth_hz == RF_V32_WIDTH_10MHZ_HZ ||
                cpu_bandwidth_hz == RF_V32_WIDTH_20MHZ_HZ)) {
        vote_wide = cpu_bandwidth_hz == RF_V32_WIDTH_20MHZ_HZ;
        delta = vote_wide ? 256 : -256;
    } else {
        return track->bandwidth_hz;
    }
    track->evidence_q8 = rf_v32_clip_evidence(
        (3 * (int32_t)track->evidence_q8) / 4 + delta);
    if (track->observation_count < UINT8_MAX) {
        track->observation_count++;
    }
    if (track->bandwidth_hz == 0) {
        track->bandwidth_hz = vote_wide
                                  ? RF_V32_WIDTH_20MHZ_HZ
                                  : RF_V32_WIDTH_10MHZ_HZ;
    } else if (track->bandwidth_hz == RF_V32_WIDTH_10MHZ_HZ &&
               track->evidence_q8 >= RF_V32_SWITCH_EVIDENCE_Q8) {
        track->bandwidth_hz = RF_V32_WIDTH_20MHZ_HZ;
    } else if (track->bandwidth_hz == RF_V32_WIDTH_20MHZ_HZ &&
               track->evidence_q8 <= -RF_V32_SWITCH_EVIDENCE_Q8) {
        track->bandwidth_hz = RF_V32_WIDTH_10MHZ_HZ;
    }
    return track->bandwidth_hz;
}
