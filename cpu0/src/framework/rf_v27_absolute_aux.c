#include "rf_v27_absolute_aux.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define RF_V27_AUX_PEAK_FREQUENCY_RADIUS (6U)
#define RF_V27_AUX_PEAK_TIME_RADIUS (4U)
#define RF_V27_AUX_DJI_HALF_BANDWIDTH_HZ (1100000)
#define RF_V27_AUX_DJI_HALF_DURATION_SAMPLES (15600U)

static uint32_t rf_v27_aux_round_u32(float value)
{
    return (value <= 0.0F) ? 0U : (uint32_t)(value + 0.5F);
}

static int64_t rf_v27_aux_round_divide(int64_t numerator, int64_t denominator)
{
    if (numerator >= 0)
    {
        return (numerator + (denominator / 2LL)) / denominator;
    }
    return -(((-numerator) + (denominator / 2LL)) / denominator);
}

static float rf_v27_aux_score(int8_t raw)
{
    const float logit =
        ((float)raw - (float)RF_V27_ABSOLUTE_AUX_OUTPUT_ZERO_POINT) *
        RF_V27_ABSOLUTE_AUX_OUTPUT_SCALE;
    return 1.0F / (1.0F + expf(-logit));
}

static int32_t rf_v27_aux_llr_q12(float score)
{
    if (score >= 0.90F)
    {
        return 11186;
    }
    if (score >= 0.75F)
    {
        return 6144;
    }
    return 2048;
}

static bool rf_v27_aux_suppressed(
    uint32_t frequency,
    uint32_t time,
    const uint8_t selected_frequency[RF_V27_ABSOLUTE_AUX_MAX_PEAKS],
    const uint8_t selected_time[RF_V27_ABSOLUTE_AUX_MAX_PEAKS],
    size_t selected_count)
{
    for (size_t index = 0U; index < selected_count; ++index)
    {
        const uint32_t frequency_distance =
            (frequency > selected_frequency[index]) ?
            (frequency - selected_frequency[index]) :
            (selected_frequency[index] - frequency);
        const uint32_t time_distance =
            (time > selected_time[index]) ?
            (time - selected_time[index]) :
            (selected_time[index] - time);
        if ((frequency_distance <= RF_V27_AUX_PEAK_FREQUENCY_RADIUS) &&
            (time_distance <= RF_V27_AUX_PEAK_TIME_RADIUS))
        {
            return true;
        }
    }
    return false;
}

static void rf_v27_aux_fill_geometry(
    uint32_t frequency,
    uint32_t time,
    float score,
    uint8_t center_slot,
    uint64_t detection_time_us,
    rf_v27_absolute_aux_evidence_t *evidence)
{
    int32_t center_frequency;
    int64_t time_center;
    int64_t frequency_low;
    int64_t frequency_high;
    int64_t time_low;
    int64_t time_high;
    const int64_t frequency_min = -28000000LL;
    const int64_t frequency_max = 28000000LL;

    /* Match rf_v12_candidate_from_center(): output cell centres are q1. */
    center_frequency = (int32_t)rf_v27_aux_round_divide(
        (-60000000LL * 102LL) +
        (((int64_t)(frequency * 2U + 1U)) * 60000000LL),
        204LL);
    time_center = rf_v27_aux_round_divide(
        ((int64_t)(time * 2U + 1U) * RF_V12_TILE_SAMPLES), 116LL);
    frequency_low = (int64_t)center_frequency -
                    RF_V27_AUX_DJI_HALF_BANDWIDTH_HZ;
    frequency_high = (int64_t)center_frequency +
                     RF_V27_AUX_DJI_HALF_BANDWIDTH_HZ;
    time_low = time_center - RF_V27_AUX_DJI_HALF_DURATION_SAMPLES;
    time_high = time_center + RF_V27_AUX_DJI_HALF_DURATION_SAMPLES;
    if (frequency_low < frequency_min) frequency_low = frequency_min;
    if (frequency_high > frequency_max) frequency_high = frequency_max;
    if (time_low < 0LL) time_low = 0LL;
    if (time_high > (int64_t)RF_V12_TILE_SAMPLES)
    {
        time_high = RF_V12_TILE_SAMPLES;
    }

    memset(evidence, 0, sizeof(*evidence));
    evidence->detection_time_us = detection_time_us;
    evidence->frequency_low_offset_hz = (int32_t)frequency_low;
    evidence->frequency_high_offset_hz = (int32_t)frequency_high;
    evidence->visible_start_sample = (uint32_t)time_low;
    evidence->visible_end_sample = (uint32_t)time_high;
    evidence->confidence_q15 = (uint16_t)rf_v27_aux_round_u32(
        score * (float)RF_V12_CONFIDENCE_Q15_ONE);
    evidence->llr_q12 = (int16_t)rf_v27_aux_llr_q12(score);
    evidence->center_slot = center_slot;
    /* This is a model-derived fixed geometry, not an ROI post-process.  It
     * is accepted as primary auxiliary evidence; old-model matches are also
     * marked corroborated below. */
    evidence->roi_decision = RF_V13_ROI_PASS;
    evidence->quality_tier = (score >= 0.90F) ? 2U : 1U;
}

size_t rf_v27_absolute_aux_decode(
    const int8_t *heatmap,
    uint8_t center_slot,
    uint64_t detection_time_us,
    rf_v27_absolute_aux_evidence_t *output,
    size_t output_capacity)
{
    uint8_t selected_frequency[RF_V27_ABSOLUTE_AUX_MAX_PEAKS] = {0U};
    uint8_t selected_time[RF_V27_ABSOLUTE_AUX_MAX_PEAKS] = {0U};
    const int32_t threshold_raw = RF_V27_ABSOLUTE_AUX_THRESHOLD_RAW;
    size_t selected_count = 0U;

    if ((heatmap == NULL) || (output == NULL) || (output_capacity == 0U) ||
        (center_slot >= RF_V12_CENTER_COUNT))
    {
        return 0U;
    }
    if (output_capacity > RF_V27_ABSOLUTE_AUX_MAX_PEAKS)
    {
        output_capacity = RF_V27_ABSOLUTE_AUX_MAX_PEAKS;
    }

    while (selected_count < output_capacity)
    {
        int32_t best_raw = threshold_raw - 1;
        uint32_t best_frequency = 0U;
        uint32_t best_time = 0U;
        for (uint32_t frequency = 0U;
             frequency < RF_V27_ABSOLUTE_AUX_HEATMAP_FREQUENCY_BINS;
             ++frequency)
        {
            for (uint32_t time = 0U;
                 time < RF_V27_ABSOLUTE_AUX_HEATMAP_TIME_BINS;
                 ++time)
            {
                const int32_t raw = (int32_t)heatmap[
                    frequency * RF_V27_ABSOLUTE_AUX_HEATMAP_TIME_BINS + time];
                if ((raw < threshold_raw) ||
                    rf_v27_aux_suppressed(frequency, time,
                                          selected_frequency, selected_time,
                                          selected_count))
                {
                    continue;
                }
                if (raw > best_raw)
                {
                    best_raw = raw;
                    best_frequency = frequency;
                    best_time = time;
                }
            }
        }
        if (best_raw < threshold_raw)
        {
            break;
        }
        selected_frequency[selected_count] = (uint8_t)best_frequency;
        selected_time[selected_count] = (uint8_t)best_time;
        rf_v27_aux_fill_geometry(
            best_frequency,
            best_time,
            rf_v27_aux_score((int8_t)best_raw),
            center_slot,
            detection_time_us,
            &output[selected_count]);
        selected_count++;
    }
    return selected_count;
}

int rf_v27_cpu0_set_model_corroborated(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index)
{
    if ((message == NULL) ||
        (evidence_index >= message->evidence_count) ||
        (evidence_index >= RF_V13_MAX_EVIDENCE_PER_ROUND))
    {
        return 0;
    }
    message->evidence[evidence_index].evidence_flags |=
        RF_V27_EVIDENCE_MODEL_CORROBORATED;
    return 1;
}

int rf_v27_cpu0_set_canonical_llr(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index,
    int32_t llr_q12)
{
    int64_t numerator;
    uint32_t confidence_q15;
    if ((message == NULL) ||
        (evidence_index >= message->evidence_count) ||
        (evidence_index >= RF_V13_MAX_EVIDENCE_PER_ROUND) ||
        (llr_q12 < 0))
    {
        return 0;
    }
    if (llr_q12 > RF_V27_CANONICAL_LLR_MAX_Q12)
    {
        llr_q12 = RF_V27_CANONICAL_LLR_MAX_Q12;
    }
    numerator = (int64_t)llr_q12 * RF_V12_CONFIDENCE_Q15_ONE;
    confidence_q15 = (uint32_t)((numerator +
                                 RF_V27_CANONICAL_LLR_MAX_Q12 / 2) /
                                RF_V27_CANONICAL_LLR_MAX_Q12);
    if (confidence_q15 > RF_V12_CONFIDENCE_Q15_ONE)
    {
        confidence_q15 = RF_V12_CONFIDENCE_Q15_ONE;
    }
    message->evidence[evidence_index].confidence_q15 =
        (uint16_t)confidence_q15;
    message->evidence[evidence_index].evidence_flags |=
        RF_V27_EVIDENCE_CANONICAL_LLR_Q15;
    return 1;
}
