#ifndef RF_V12_DETECTOR_H
#define RF_V12_DETECTOR_H

#include <stdint.h>

#include "display_stream.h"
#include "rf_v12_sparse_contract.h"

#define RF_V12_OBJECT_COUNT (4U)

typedef struct st_rf_v12_detector_input
{
    uint32_t tile_sequence;
    uint32_t round_index;
    uint32_t center_index;
    uint64_t center_frequency_hz;
    uint64_t capture_start_time_us;
    uint64_t capture_end_time_us;
    uint16_t background_generation;
    int16_t sdr_gain_db_q8;
    uint8_t tile_validity;
    uint8_t tile_flags;
    const float *c0_db;
    const int8_t *model_input;
} rf_v12_detector_input_t;

typedef struct st_rf_v12_detector_result
{
    rf_v12_tile_payload_t tile;
    uint16_t object_presence_q15[RF_V12_OBJECT_COUNT];
    uint8_t display_mask[RA8P1_DISPLAY_MASK_BYTES];
    uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE];
    uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE];
    uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE];
    uint8_t best_class_id;
    uint16_t best_score_q15;
} rf_v12_detector_result_t;

void rf_v12_detector_decode(const rf_v12_detector_input_t *input,
                            rf_v12_detector_result_t *result);
uint8_t rf_v12_class_to_object(uint8_t class_id);

#endif
