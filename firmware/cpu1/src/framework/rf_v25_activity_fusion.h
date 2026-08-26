#ifndef RF_V25_ACTIVITY_FUSION_H
#define RF_V25_ACTIVITY_FUSION_H

#include <stdint.h>

#include "rf_v13_activity_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V25_ACTIVITY_ABI_MAJOR UINT16_C(25)
#define RF_V25_ACTIVITY_ABI_MINOR UINT16_C(0)
#define RF_V25_EVIDENCE_STRONG_TEXTURE UINT8_C(0x20)

typedef enum rf_v25_activity_state {
    RF_V25_ACTIVITY_NO_RF_OBSERVED = 0,
    RF_V25_ACTIVITY_UNCERTAIN = 1,
    RF_V25_ACTIVITY_WORKING = 2
} rf_v25_activity_state_t;

typedef enum rf_v25_evidence_quality {
    RF_V25_QUALITY_NONE = 0,
    RF_V25_QUALITY_WEAK = 1,
    RF_V25_QUALITY_NORMAL = 2,
    RF_V25_QUALITY_STRONG = 3
} rf_v25_evidence_quality_t;

typedef enum rf_v25_apply_result {
    RF_V25_APPLY_OUTPUT_READY = 0,
    RF_V25_APPLY_HELD_INVALID_NO_OUTPUT = 1,
    RF_V25_APPLY_IGNORED_DUPLICATE = 2,
    RF_V25_APPLY_IGNORED_STALE = 3,
    RF_V25_APPLY_BAD_ARGUMENT = 4,
    RF_V25_APPLY_BAD_MESSAGE = 5
} rf_v25_apply_result_t;

enum rf_v25_reason_flags {
    RF_V25_REASON_NONE = 0u,
    RF_V25_REASON_ON_SUPPORT = 1u << 0,
    RF_V25_REASON_ON_WEAK = 1u << 1,
    RF_V25_REASON_OFF_FULL_MISS = 1u << 2,
    RF_V25_REASON_OFF_WEAK_MISS = 1u << 3,
    RF_V25_REASON_OFF_NORMAL_RECOVERY = 1u << 4,
    RF_V25_REASON_OFF_STRONG_RESET = 1u << 5,
    RF_V25_REASON_DJI_DUAL_RECENT = 1u << 6,
    RF_V25_REASON_T12_HOP_BONUS = 1u << 7,
    RF_V25_REASON_ENTRY_SINGLE_WINDOW = 1u << 8,
    RF_V25_REASON_ENTRY_DUAL_WINDOW = 1u << 9,
    RF_V25_REASON_ENTERED_UNCERTAIN = 1u << 10,
    RF_V25_REASON_ENTERED_WORKING = 1u << 11,
    RF_V25_REASON_EXITED_WORKING = 1u << 12,
    RF_V25_REASON_RETURNED_NO_RF = 1u << 13,
    RF_V25_REASON_ROUND_OUTPUT_READY = 1u << 14,
    RF_V25_REASON_DUPLICATES_CAPPED = 1u << 15,
    RF_V25_REASON_INVALID_ROUND_FROZEN = 1u << 16
};

typedef struct rf_v25_device_profile {
    int32_t on_hit_leak_q12;
    int32_t on_miss_decay_q12;
    int32_t weak_on_scale_q12;
    int32_t on_enter_q12;
    int32_t on_dual_enter_q12;
    int32_t on_cap_q12;
    int32_t off_miss_llr_q12;
    int32_t off_exit_q12;
    int32_t off_dual_exit_q12;
    int32_t off_cap_q12;
    int32_t weak_miss_scale_q12;
    int32_t normal_off_decay_q12;
    int32_t multi_hit_scale_q12;
    int32_t multi_hit_bonus_cap_q12;
    int32_t support_llr_q12;
    int32_t strong_llr_q12;
    uint8_t support_window_rounds;
    uint8_t single_support_rounds;
    uint8_t single_strong_rounds;
    uint8_t dual_support_rounds;
    uint8_t dual_strong_rounds;
    uint8_t uncertain_exit_rounds;
    uint8_t working_min_hold_rounds;
    uint8_t reserved0;
    uint16_t exit_miss_rounds;
    uint16_t dual_exit_miss_rounds;
} rf_v25_device_profile_t;

typedef struct rf_v25_activity_config {
    rf_v13_activity_config_t evidence;
    rf_v25_device_profile_t profiles[RF_V13_OBJECT_COUNT];
    int32_t t12_hop_bonus_q12;
} rf_v25_activity_config_t;

typedef struct rf_v25_object_state {
    uint64_t last_transition_time_us;
    int32_t on_evidence_q12;
    int32_t off_evidence_q12;
    int32_t last_on_llr_q12;
    int32_t last_off_llr_q12;
    uint32_t last_reason_flags;
    uint16_t working_age_rounds;
    uint16_t consecutive_miss_rounds;
    uint16_t active_exit_miss_rounds;
    uint8_t activity_state;
    uint8_t support_history_bits;
    uint8_t strong_history_bits;
    uint8_t control_history_bits;
    uint8_t video_history_bits;
    uint8_t dual_source_history_bits;
    uint8_t support_count;
    uint8_t strong_count;
    uint8_t dual_source_count;
    uint8_t last_round_had_entry_support;
    uint8_t last_round_had_strong_support;
    uint8_t last_round_had_dual_source;
    uint8_t last_round_quality;
    uint8_t dji_dual_profile;
    uint8_t last_positive_center_mask;
    uint8_t reserved[7];
} rf_v25_object_state_t;

typedef struct rf_v25_activity_fusion {
    rf_v13_activity_fusion_t evidence;
    rf_v25_object_state_t objects[RF_V13_OBJECT_COUNT];
    uint32_t output_generation;
    uint32_t last_output_round_index;
} rf_v25_activity_fusion_t;

typedef struct rf_v25_activity_view {
    uint64_t last_positive_time_us;
    uint64_t last_transition_time_us;
    int32_t on_evidence_q12;
    int32_t off_evidence_q12;
    int32_t last_on_llr_q12;
    int32_t last_off_llr_q12;
    uint32_t last_reason_flags;
    uint32_t last_invalid_reason_flags;
    uint32_t last_round_index;
    uint16_t working_age_rounds;
    uint16_t consecutive_miss_rounds;
    uint16_t active_exit_miss_rounds;
    uint8_t activity_state;
    uint8_t last_positive_source_class;
    uint8_t last_positive_band_mask;
    uint8_t last_positive_center_slot;
    uint8_t last_round_complete;
    uint8_t last_round_valid;
    uint8_t support_history_bits;
    uint8_t support_count;
    uint8_t strong_history_bits;
    uint8_t strong_count;
    uint8_t control_history_bits;
    uint8_t video_history_bits;
    uint8_t dual_source_history_bits;
    uint8_t dual_source_count;
    uint8_t last_round_quality;
    uint8_t dji_dual_profile;
    uint8_t last_positive_center_mask;
    uint8_t reserved[3];
} rf_v25_activity_view_t;

void rf_v25_activity_fusion_init(rf_v25_activity_fusion_t *fusion);

rf_v25_apply_result_t rf_v25_activity_fusion_apply_round(
    rf_v25_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v25_activity_config_t *config
);

int rf_v25_activity_fusion_get(
    const rf_v25_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id,
    rf_v25_activity_view_t *view
);

uint32_t rf_v25_activity_fusion_output_generation(
    const rf_v25_activity_fusion_t *fusion
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v25_device_profile_t) == 76u,
               "V25 device profile ABI changed");
_Static_assert(sizeof(rf_v25_activity_config_t) == 520u,
               "V25 activity config ABI changed");
_Static_assert(sizeof(rf_v25_object_state_t) == 56u,
               "V25 object state ABI changed");
_Static_assert(sizeof(rf_v25_activity_fusion_t) == 536u,
               "V25 fusion state ABI changed");
_Static_assert(sizeof(rf_v25_activity_view_t) == 72u,
               "V25 activity view ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
