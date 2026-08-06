#ifndef RF_V13_ROUND_BUILDER_H
#define RF_V13_ROUND_BUILDER_H

#include <stdbool.h>
#include <stdint.h>

#include "rf_v12_sparse_contract.h"

typedef struct st_rf_v13_round_builder_stats
{
    uint32_t submitted_tiles;
    uint32_t published_rounds;
    uint32_t invalid_rounds;
    uint32_t mailbox_drops;
    uint32_t malformed_tiles;
    uint32_t last_message_sequence;
    uint32_t last_round_index;
} rf_v13_round_builder_stats_t;

void rf_v13_round_builder_init(void);
void rf_v13_round_builder_reset(void);
bool rf_v13_round_builder_submit(const rf_v12_tile_payload_t *tile);
bool rf_v13_round_builder_submit_processed(
    const rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    uint32_t display_session_id,
    uint32_t display_window_sequence);
void rf_v13_round_builder_flush(void);
void rf_v13_round_builder_stats_get(rf_v13_round_builder_stats_t *stats);

#endif
