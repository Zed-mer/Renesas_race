#ifndef RF_V27_ACTIVITY_FUSION_H
#define RF_V27_ACTIVITY_FUSION_H

#include <stdint.h>

#include "rf_v13_activity_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V27_ACTIVITY_ABI_MAJOR UINT16_C(27)
#define RF_V27_ACTIVITY_ABI_MINOR UINT16_C(2)
#define RF_V27_MAX_HISTORY_ROUNDS UINT8_C(8)

/* Bits 6 and 7 are outside the V12/V18 low-five/texture flag contract.  Bit
 * 5 is deliberately left untouched because older firmware uses it for strong
 * texture.  The message and evidence structure sizes remain unchanged. */
#define RF_V27_EVIDENCE_MODEL_CORROBORATED (UINT8_C(1) << 6)
#define RF_V27_EVIDENCE_CANONICAL_LLR_Q15 (UINT8_C(1) << 7)

/* A canonical message score is an LLR in [0, 8] represented by q15.  This
 * lets CPU0 normalize model-specific calibration before the fixed CPU1 table
 * is applied, without adding fields to the IPC ABI. */
#define RF_V27_CANONICAL_LLR_MAX_Q12 INT32_C(32768)

/* External binary states.  Candidate/decaying are deliberately hidden. */
typedef enum rf_v27_activity_state {
    RF_V27_ACTIVITY_NO_RF_OBSERVED = 0,
    RF_V27_ACTIVITY_WORKING = 2
} rf_v27_activity_state_t;

typedef enum rf_v27_memory_phase {
    RF_V27_MEMORY_NO_MEMORY = 0,
    RF_V27_MEMORY_CANDIDATE = 1,
    RF_V27_MEMORY_WORKING = 2,
    RF_V27_MEMORY_DECAYING = 3
} rf_v27_memory_phase_t;

typedef enum rf_v27_quality {
    RF_V27_QUALITY_NONE = 0,
    RF_V27_QUALITY_WEAK = 1,
    RF_V27_QUALITY_NORMAL = 2,
    RF_V27_QUALITY_STRONG = 3
} rf_v27_quality_t;

typedef enum rf_v27_apply_result {
    RF_V27_APPLY_OUTPUT_READY = 0,
    RF_V27_APPLY_HELD_INVALID_NO_OUTPUT = 1,
    RF_V27_APPLY_IGNORED_DUPLICATE = 2,
    RF_V27_APPLY_IGNORED_STALE = 3,
    RF_V27_APPLY_BAD_ARGUMENT = 4,
    RF_V27_APPLY_BAD_MESSAGE = 5
} rf_v27_apply_result_t;

enum rf_v27_reason_flags {
    RF_V27_REASON_NONE = 0u,
    RF_V27_REASON_ON_SUPPORT = 1u << 0,
    RF_V27_REASON_ON_WEAK = 1u << 1,
    RF_V27_REASON_OFF_FULL_MISS = 1u << 2,
    RF_V27_REASON_OFF_WEAK_MISS = 1u << 3,
    RF_V27_REASON_OFF_RECOVERED = 1u << 4,
    RF_V27_REASON_T12_HOP_BONUS = 1u << 5,
    RF_V27_REASON_ENTRY_SUPPORT = 1u << 6,
    RF_V27_REASON_ENTRY_DUAL_SOURCE = 1u << 7,
    RF_V27_REASON_ENTERED_WORKING = 1u << 8,
    RF_V27_REASON_EXITED_WORKING = 1u << 9,
    RF_V27_REASON_ROUND_OUTPUT_READY = 1u << 10,
    RF_V27_REASON_INVALID_ROUND_FROZEN = 1u << 11,
    RF_V27_REASON_DUPLICATES_CAPPED = 1u << 12,
    RF_V27_REASON_DJI_CONTINUOUS_VIDEO_REJECTED = 1u << 13
};

/* All scalar evidence values use signed Q20.12, as in V13/V25. */
typedef struct rf_v27_device_profile {
    int32_t on_hit_leak_q12;
    int32_t on_miss_decay_q12;
    int32_t weak_on_scale_q12;
    int32_t on_enter_q12;
    int32_t on_dual_enter_q12;
    int32_t on_cap_q12;
    int32_t support_llr_q12;
    int32_t strong_llr_q12;
    int32_t off_miss_llr_q12;
    int32_t off_weak_scale_q12;
    int32_t off_support_decay_q12;
    int32_t off_exit_q12;
    int32_t off_recent_strong_q12;
    uint8_t history_rounds;
    uint8_t single_support_rounds;
    uint8_t single_strong_rounds;
    uint8_t dual_support_rounds;
    uint8_t dual_strong_rounds;
    uint8_t candidate_timeout_rounds;
    uint8_t strong_memory_rounds;
    uint8_t minimum_working_rounds;
    uint16_t exit_miss_rounds;
    uint16_t recent_strong_exit_miss_rounds;
} rf_v27_device_profile_t;

typedef struct rf_v27_activity_config {
    rf_v13_activity_config_t evidence;
    rf_v27_device_profile_t profiles[RF_V13_OBJECT_COUNT];
    int32_t multi_center_scale_q12;
    int32_t multi_center_bonus_cap_q12;
    int32_t t12_hop_bonus_q12;
    int32_t roi_fail_agreement_scale_q12;
    int32_t model_agreement_bonus_q12;
} rf_v27_activity_config_t;

typedef struct rf_v27_object_state {
    uint64_t last_transition_time_us;
    int32_t on_evidence_q12;
    int32_t off_evidence_q12;
    int32_t last_on_llr_q12;
    int32_t last_off_llr_q12;
    uint32_t last_reason_flags;
    uint16_t working_age_rounds;
    uint16_t consecutive_empty_rounds;
    uint16_t candidate_age_rounds;
    uint16_t rounds_since_strong;
    uint8_t activity_state;
    uint8_t memory_phase;
    uint8_t support_history_bits;
    uint8_t strong_history_bits;
    uint8_t control_history_bits;
    uint8_t video_history_bits;
    uint8_t model_agreement_history_bits;
    uint8_t support_count;
    uint8_t strong_count;
    uint8_t last_round_quality;
    uint8_t last_positive_center_mask;
    uint8_t last_source_mask;
    uint8_t reserved[4];
} rf_v27_object_state_t;

typedef struct rf_v27_activity_fusion {
    /* Keeps validation, duplicate and stale-round handling identical to V13. */
    rf_v13_activity_fusion_t evidence;
    rf_v27_object_state_t objects[RF_V13_OBJECT_COUNT];
    uint32_t output_generation;
    uint32_t last_output_round_index;
} rf_v27_activity_fusion_t;

typedef struct rf_v27_activity_view {
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
    uint16_t consecutive_empty_rounds;
    uint16_t candidate_age_rounds;
    uint16_t rounds_since_strong;
    uint8_t activity_state;
    uint8_t memory_phase;
    uint8_t last_positive_source_class;
    uint8_t last_positive_band_mask;
    uint8_t last_positive_center_slot;
    uint8_t last_round_complete;
    uint8_t last_round_valid;
    uint8_t support_history_bits;
    uint8_t support_count;
    uint8_t strong_history_bits;
    uint8_t strong_count;
    uint8_t last_round_quality;
    uint8_t last_positive_center_mask;
    uint8_t model_agreement_history_bits;
    uint8_t reserved[1];
} rf_v27_activity_view_t;

/* Call immediately after adding/canonicalizing a CPU0 evidence item that was
 * observed by at least two detector views.  This does not add a second item
 * and therefore cannot double-count one physical event. */
int rf_v27_cpu0_set_model_corroborated(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index
);

/* Replace confidence_q15 with a canonical [0,8] LLR score.  The original
 * detector confidence remains available in the V12 result stream; only the
 * compact four-frequency evidence message uses this normalized value. */
int rf_v27_cpu0_set_canonical_llr(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index,
    int32_t llr_q12
);

void rf_v27_activity_fusion_init(rf_v27_activity_fusion_t *fusion);

rf_v27_apply_result_t rf_v27_activity_fusion_apply_round(
    rf_v27_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v27_activity_config_t *config
);

int rf_v27_activity_fusion_get(
    const rf_v27_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id,
    rf_v27_activity_view_t *view
);

uint32_t rf_v27_activity_fusion_output_generation(
    const rf_v27_activity_fusion_t *fusion
);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v27_device_profile_t) == 64u,
               "V27 device profile ABI changed");
_Static_assert(sizeof(rf_v27_object_state_t) == 56u,
               "V27 object state ABI changed");
_Static_assert(sizeof(rf_v27_activity_fusion_t) == 536u,
               "V27 fusion state ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
