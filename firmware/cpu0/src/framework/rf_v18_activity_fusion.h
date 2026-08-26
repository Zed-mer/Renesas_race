#ifndef RF_V18_ACTIVITY_FUSION_H
#define RF_V18_ACTIVITY_FUSION_H

#include <stddef.h>
#include <stdint.h>

#include "rf_v13_activity_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V18_ACTIVITY_ABI_MAJOR UINT16_C(18)
#define RF_V18_ACTIVITY_ABI_MINOR UINT16_C(0)
#define RF_V18_TILE_ACTIVITY_OUTPUT_ALLOWED UINT8_C(0)

/* V13 preserves bits 5..7 in the 512-byte message even though V12 uses 0..4. */
#define RF_V18_EVIDENCE_STRONG_TEXTURE UINT8_C(0x20)

typedef enum rf_v18_activity_state_id {
    RF_V18_ACTIVITY_NO_RF_OBSERVED = 0,
    RF_V18_ACTIVITY_UNCERTAIN = 1,
    RF_V18_ACTIVITY_WORKING = 2
} rf_v18_activity_state_id_t;

typedef enum rf_v18_apply_result {
    RF_V18_APPLY_OUTPUT_READY = 0,
    RF_V18_APPLY_HELD_INVALID_NO_OUTPUT = 1,
    RF_V18_APPLY_IGNORED_DUPLICATE = 2,
    RF_V18_APPLY_IGNORED_STALE = 3,
    RF_V18_APPLY_BAD_ARGUMENT = 4,
    RF_V18_APPLY_BAD_MESSAGE = 5
} rf_v18_apply_result_t;

enum rf_v18_reason_flags {
    RF_V18_REASON_NONE = 0u,
    RF_V18_REASON_ENTRY_SUPPORT = 1u << 0,
    RF_V18_REASON_BELOW_ENTRY_SUPPORT = 1u << 1,
    RF_V18_REASON_DJI_DUAL_SOURCE = 1u << 2,
    RF_V18_REASON_DJI_STRONG_TEXTURE = 1u << 3,
    RF_V18_REASON_DJI_FAST_ENTRY_WINDOW_MET = 1u << 4,
    RF_V18_REASON_DJI_SINGLE_SOURCE_WINDOW_MET = 1u << 5,
    RF_V18_REASON_DJI_DUAL_SOURCE_WINDOW_MET = 1u << 6,
    RF_V18_REASON_OTHER_ENTRY_WINDOW_MET = 1u << 7,
    RF_V18_REASON_ENTRY_BLOCKED_BY_SUPPORT_WINDOW = 1u << 8,
    RF_V18_REASON_DJI_UNCERTAIN_ENTRY_DEBOUNCE = 1u << 9,
    RF_V18_REASON_UNCERTAIN_EXIT_DEBOUNCE = 1u << 10,
    RF_V18_REASON_WORKING_MIN_HOLD = 1u << 11,
    RF_V18_REASON_WORKING_EXIT_MISS_DEBOUNCE = 1u << 12,
    RF_V18_REASON_ENTERED_WORKING = 1u << 13,
    RF_V18_REASON_EXITED_WORKING = 1u << 14,
    RF_V18_REASON_ENTERED_UNCERTAIN = 1u << 15,
    RF_V18_REASON_RETURNED_NO_RF = 1u << 16,
    RF_V18_REASON_ROUND_OUTPUT_READY = 1u << 17
};

typedef struct rf_v18_activity_config {
    rf_v13_activity_config_t evidence;
    int32_t entry_support_llr_q12;
    int32_t strong_support_llr_q12;
    int32_t dji_working_enter_q12;
    int32_t dji_dual_working_enter_q12;
    uint8_t entry_support_window_rounds;
    uint8_t dji_fast_entry_support_rounds;
    uint8_t dji_fast_entry_strong_rounds;
    uint8_t dji_single_source_support_rounds;
    uint8_t dji_single_source_strong_rounds;
    uint8_t dji_dual_source_support_rounds;
    uint8_t dji_dual_source_strong_rounds;
    uint8_t other_entry_support_rounds;
    uint8_t dji_uncertain_enter_support_rounds;
    uint8_t uncertain_exit_miss_rounds;
    uint16_t dji_uncertain_exit_miss_rounds;
    uint16_t working_exit_miss_rounds;
    uint16_t working_min_hold_rounds;
} rf_v18_activity_config_t;

typedef struct rf_v18_object_debounce {
    uint64_t last_transition_time_us;
    uint32_t last_reason_flags;
    uint16_t working_age_rounds;
    uint16_t consecutive_miss_rounds;
    uint8_t activity_state;
    uint8_t support_history_bits;
    uint8_t support_count;
    uint8_t last_round_had_entry_support;
    uint8_t strong_history_bits;
    uint8_t strong_count;
    uint8_t last_round_had_strong_support;
    uint8_t dual_source_history_bits;
    uint8_t dual_source_count;
    uint8_t last_round_had_dual_source;
    uint8_t reserved[6];
} rf_v18_object_debounce_t;

typedef struct rf_v18_activity_fusion {
    rf_v13_activity_fusion_t evidence;
    rf_v18_object_debounce_t objects[RF_V13_OBJECT_COUNT];
    uint32_t output_generation;
    uint32_t last_output_round_index;
} rf_v18_activity_fusion_t;

typedef struct rf_v18_activity_view {
    uint64_t last_positive_time_us;
    uint64_t last_transition_time_us;
    int32_t energy_q12;
    int32_t last_llr_q12;
    uint32_t last_reason_flags;
    uint32_t last_invalid_reason_flags;
    uint32_t last_round_index;
    uint16_t working_age_rounds;
    uint16_t consecutive_miss_rounds;
    uint8_t activity_state;
    uint8_t last_positive_source_class;
    uint8_t last_positive_band_mask;
    uint8_t last_positive_center_slot;
    uint8_t last_round_complete;
    uint8_t last_round_valid;
    uint8_t support_history_bits;
    uint8_t support_count;
    uint8_t last_round_had_entry_support;
    uint8_t strong_history_bits;
    uint8_t strong_count;
    uint8_t last_round_had_strong_support;
    uint8_t dual_source_history_bits;
    uint8_t dual_source_count;
    uint8_t last_round_had_dual_source;
} rf_v18_activity_view_t;

void rf_v18_activity_fusion_init(rf_v18_activity_fusion_t *fusion);

rf_v18_apply_result_t rf_v18_activity_fusion_apply_round(
    rf_v18_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v18_activity_config_t *config
);

int rf_v18_activity_fusion_get(
    const rf_v18_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id,
    rf_v18_activity_view_t *view
);

uint32_t rf_v18_activity_fusion_output_generation(
    const rf_v18_activity_fusion_t *fusion
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v18_activity_config_t) == 244u,
               "V18 activity config ABI changed");
_Static_assert(sizeof(rf_v18_object_debounce_t) == 32u,
               "V18 object debounce ABI changed");
_Static_assert(sizeof(rf_v18_activity_fusion_t) == 440u,
               "V18 fusion state ABI changed");
_Static_assert(sizeof(rf_v18_activity_view_t) == 56u,
               "V18 activity view ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
