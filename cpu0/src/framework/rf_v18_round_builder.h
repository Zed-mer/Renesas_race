#ifndef RF_V18_ROUND_BUILDER_H
#define RF_V18_ROUND_BUILDER_H

#include <stddef.h>
#include <stdint.h>

#include "rf_v12_sparse_contract.h"
#include "rf_v13_activity_fusion.h"
#include "rf_v18_activity_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V18_ROUND_BUILDER_ABI_MAJOR UINT16_C(18)
#define RF_V18_ROUND_BUILDER_ABI_MINOR UINT16_C(0)

typedef struct rf_v18_event_aux {
    int16_t period_bonus_q12;
    uint8_t roi_decision;
    /* 0 is display-only/no state evidence, 1 is normal, 2 is strong. */
    uint8_t quality_tier;
} rf_v18_event_aux_t;

typedef enum rf_v18_round_add_result {
    RF_V18_ROUND_TILE_ACCEPTED = 0,
    RF_V18_ROUND_TILE_ACCEPTED_INVALID = 1,
    RF_V18_ROUND_BAD_ARGUMENT = 2,
    RF_V18_ROUND_BAD_STATE = 3,
    RF_V18_ROUND_BAD_TILE_HEADER = 4,
    RF_V18_ROUND_DUPLICATE_SLOT = 5,
    RF_V18_ROUND_WRONG_ROUND = 6
} rf_v18_round_add_result_t;

typedef struct rf_v18_round_builder {
    rf_v13_cpu0_round_message_t message;
    uint32_t last_tile_sequence;
    uint8_t active;
    uint8_t tile_count;
    uint8_t seen_slot_mask;
    uint8_t header_valid_slot_mask;
} rf_v18_round_builder_t;

void rf_v18_round_builder_init(rf_v18_round_builder_t *builder);

int rf_v18_round_builder_begin(
    rf_v18_round_builder_t *builder,
    uint32_t message_sequence,
    uint32_t round_index,
    uint64_t round_start_time_us
);

rf_v18_round_add_result_t rf_v18_round_builder_add_tile(
    rf_v18_round_builder_t *builder,
    const rf_v12_tile_payload_t *tile,
    const rf_v18_event_aux_t *event_aux,
    size_t event_aux_count
);

int rf_v18_round_builder_finish(
    rf_v18_round_builder_t *builder,
    rf_v13_cpu0_round_message_t *output
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v18_event_aux_t) == 4u,
               "V18 event aux ABI changed");
_Static_assert(sizeof(rf_v18_round_builder_t) <= 544u,
               "V18 round builder unexpectedly grew");
#endif

#ifdef __cplusplus
}
#endif

#endif
