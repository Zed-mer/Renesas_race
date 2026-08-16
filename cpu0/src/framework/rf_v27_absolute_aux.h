#ifndef RF_V27_ABSOLUTE_AUX_H
#define RF_V27_ABSOLUTE_AUX_H

#include <stddef.h>
#include <stdint.h>

#include "rf_v12_sparse_contract.h"
#include "rf_v13_activity_fusion.h"

#define RF_V27_ABSOLUTE_AUX_MAX_PEAKS (2U)
#define RF_V27_ABSOLUTE_AUX_HEATMAP_FREQUENCY_BINS \
    (RF_V12_HEATMAP_FREQUENCY_BINS)
#define RF_V27_ABSOLUTE_AUX_HEATMAP_TIME_BINS (RF_V12_HEATMAP_TIME_BINS)
#define RF_V27_ABSOLUTE_AUX_OUTPUT_SCALE (0.04560483992099762F)
#define RF_V27_ABSOLUTE_AUX_OUTPUT_ZERO_POINT (100)
#define RF_V27_ABSOLUTE_AUX_CALIBRATED_SCORE (0.50F)
#define RF_V27_ABSOLUTE_AUX_THRESHOLD_SCALE (1.20F)
#define RF_V27_ABSOLUTE_AUX_MIN_SCORE \
    (RF_V27_ABSOLUTE_AUX_CALIBRATED_SCORE * \
     RF_V27_ABSOLUTE_AUX_THRESHOLD_SCALE)
/* Smallest raw value whose dequantized sigmoid is at least 0.60. */
#define RF_V27_ABSOLUTE_AUX_THRESHOLD_RAW (109)

/* These bits are deliberately outside the legacy low-five evidence flags. */
#define RF_V27_EVIDENCE_MODEL_CORROBORATED (UINT8_C(1) << 6U)
#define RF_V27_EVIDENCE_CANONICAL_LLR_Q15 (UINT8_C(1) << 7U)
#define RF_V27_CANONICAL_LLR_MAX_Q12 INT32_C(32768)

typedef struct st_rf_v27_absolute_aux_evidence
{
    uint64_t detection_time_us;
    int32_t frequency_low_offset_hz;
    int32_t frequency_high_offset_hz;
    uint32_t visible_start_sample;
    uint32_t visible_end_sample;
    uint16_t confidence_q15;
    int16_t llr_q12;
    uint8_t center_slot;
    uint8_t roi_decision;
    uint8_t quality_tier;
    uint8_t reserved;
} rf_v27_absolute_aux_evidence_t;

/* Decode only the DJI-control output.  The geometry is used for matching
 * against an existing V21/V20 event; it is intentionally never put in the
 * display stream as a new model box. */
size_t rf_v27_absolute_aux_decode(
    const int8_t *heatmap,
    uint8_t center_slot,
    uint64_t detection_time_us,
    rf_v27_absolute_aux_evidence_t *output,
    size_t output_capacity);

int rf_v27_cpu0_set_model_corroborated(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index);

int rf_v27_cpu0_set_canonical_llr(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index,
    int32_t llr_q12);

#endif
