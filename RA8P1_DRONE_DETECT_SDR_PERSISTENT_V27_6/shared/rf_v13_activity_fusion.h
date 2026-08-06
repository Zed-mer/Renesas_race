#ifndef RF_V13_ACTIVITY_FUSION_H
#define RF_V13_ACTIVITY_FUSION_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Little-endian bytes spell "V13F". */
#define RF_V13_ACTIVITY_MAGIC UINT32_C(0x46333156)
#define RF_V13_ACTIVITY_ABI_MAJOR UINT16_C(13)
#define RF_V13_ACTIVITY_ABI_MINOR UINT16_C(1)

/* V13 wraps accepted V12 detections; it does not change either NPU model ABI. */
#define RF_V13_SOURCE_V12_ABI_MAJOR UINT16_C(12)
#define RF_V13_SOURCE_V12_TILE_BYTES UINT16_C(512)
#define RF_V13_CPU0_MESSAGE_BYTES UINT16_C(512)
#define RF_V13_MAX_EVIDENCE_PER_ROUND UINT16_C(16)
#define RF_V13_CENTER_SLOT_MASK UINT8_C(0x0f)
#define RF_V13_DISPLAY_IDENTITY_COUNT 4u
#define RF_V13_LLR_BIN_COUNT_MAX 8u

/* Q20.12 evidence contract. */
#define RF_V13_Q12_ONE INT32_C(4096)
#define RF_V13_ENERGY_LEAK_Q12 INT32_C(3482) /* round(0.85 * 4096) */
#define RF_V13_ENERGY_MIN_Q12 INT32_C(-32768)
#define RF_V13_ENERGY_MAX_Q12 INT32_C(32768)
#define RF_V13_WORKING_ENTER_Q12 INT32_C(16384)
#define RF_V13_WORKING_EXIT_Q12 INT32_C(4096)
#define RF_V13_MISS_EVIDENCE_Q12 INT32_C(-1024) /* -0.25 */
#define RF_V13_UNKNOWN_ROI_SCALE_Q12 INT32_C(2048) /* 0.5 */
#define RF_V13_MAX_PERIOD_BONUS_Q12 INT32_C(1229) /* round(0.3 * 4096) */

typedef enum rf_v13_detection_class_id {
    RF_V13_CLASS_DJI_CONTROL = 0,
    RF_V13_CLASS_DJI_VIDEO = 1,
    RF_V13_CLASS_AT9S = 2,
    RF_V13_CLASS_T12 = 3,
    RF_V13_CLASS_XIAOBAWANG = 4,
    RF_V13_CLASS_COUNT = 5
} rf_v13_detection_class_id_t;

typedef enum rf_v13_object_id {
    RF_V13_OBJECT_DJI = 0,
    RF_V13_OBJECT_AT9S = 1,
    RF_V13_OBJECT_T12 = 2,
    RF_V13_OBJECT_XIAOBAWANG = 3,
    RF_V13_OBJECT_COUNT = 4
} rf_v13_object_id_t;

typedef enum rf_v13_activity_state_id {
    RF_V13_ACTIVITY_NO_RF_OBSERVED = 0,
    RF_V13_ACTIVITY_UNCERTAIN = 1,
    RF_V13_ACTIVITY_WORKING = 2
} rf_v13_activity_state_id_t;

typedef enum rf_v13_roi_decision {
    RF_V13_ROI_UNKNOWN = 0,
    RF_V13_ROI_PASS = 1,
    RF_V13_ROI_FAIL = 2
} rf_v13_roi_decision_t;

typedef enum rf_v13_apply_result {
    RF_V13_APPLY_OK = 0,
    RF_V13_APPLY_HELD_INVALID_ROUND = 1,
    RF_V13_APPLY_IGNORED_DUPLICATE = 2,
    RF_V13_APPLY_IGNORED_STALE = 3,
    RF_V13_APPLY_BAD_ARGUMENT = 4,
    RF_V13_APPLY_BAD_MESSAGE = 5
} rf_v13_apply_result_t;

enum rf_v13_band_mask {
    RF_V13_BAND_2P4_GHZ = 1u << 0,
    RF_V13_BAND_5P8_GHZ = 1u << 1
};

enum rf_v13_round_flags {
    RF_V13_ROUND_COMPLETE = 1u << 0,
    RF_V13_ROUND_V12_TILES_VALIDATED = 1u << 1,
    RF_V13_ROUND_EVIDENCE_TRUNCATED = 1u << 2
};

enum rf_v13_invalid_reason_flags {
    RF_V13_INVALID_NONE = 0u,
    RF_V13_INVALID_INCOMPLETE = 1u << 0,
    RF_V13_INVALID_CAPTURE = 1u << 1,
    RF_V13_INVALID_BACKGROUND_NOT_READY = 1u << 2,
    RF_V13_INVALID_CRC = 1u << 3,
    RF_V13_INVALID_IQ_GAP = 1u << 4,
    RF_V13_INVALID_RING_OVERFLOW = 1u << 5,
    RF_V13_INVALID_RETUNE_UNLOCKED = 1u << 6,
    RF_V13_INVALID_TILE_SEQUENCE = 1u << 7,
    RF_V13_INVALID_RESULT_TRUNCATED = 1u << 8,
    RF_V13_INVALID_TIMESTAMP = 1u << 9,
    RF_V13_INVALID_V12_ABI = 1u << 10,
    RF_V13_INVALID_MALFORMED_EVIDENCE = 1u << 11
};

/* Low five bits mirror V12 event flags. ROI is carried separately. */
enum rf_v13_evidence_flags {
    RF_V13_EVIDENCE_TIME_CLIPPED = 1u << 0,
    RF_V13_EVIDENCE_FREQUENCY_CLIPPED = 1u << 1,
    RF_V13_EVIDENCE_VIDEO_20MHZ = 1u << 2,
    RF_V13_EVIDENCE_BANDWIDTH_AMBIGUOUS = 1u << 3,
    RF_V13_EVIDENCE_NEEDS_REVIEW = 1u << 4
};

enum rf_v13_state_reason_flags {
    RF_V13_REASON_NONE = 0u,
    RF_V13_REASON_VALID_POSITIVE = 1u << 0,
    RF_V13_REASON_VALID_NEGATIVE = 1u << 1,
    RF_V13_REASON_INVALID_ROUND_HELD = 1u << 2,
    RF_V13_REASON_ENTERED_WORKING = 1u << 3,
    RF_V13_REASON_EXITED_WORKING = 1u << 4,
    RF_V13_REASON_ENTERED_UNCERTAIN = 1u << 5,
    RF_V13_REASON_RETURNED_NO_RF = 1u << 6,
    RF_V13_REASON_DUPLICATE_EVIDENCE_COLLAPSED = 1u << 7,
    RF_V13_REASON_DJI_MULTI_SOURCE = 1u << 8,
    RF_V13_REASON_CLIPPED_HIGH = 1u << 9,
    RF_V13_REASON_CLIPPED_LOW = 1u << 10,
    RF_V13_REASON_ROUND_INDEX_GAP = 1u << 11
};

typedef struct rf_v13_llr_bin {
    uint16_t minimum_confidence_q15;
    int16_t llr_q12;
} rf_v13_llr_bin_t;

typedef struct rf_v13_class_calibration {
    uint8_t bin_count;
    uint8_t reserved[3];
    rf_v13_llr_bin_t bins[RF_V13_LLR_BIN_COUNT_MAX];
} rf_v13_class_calibration_t;

typedef struct rf_v13_activity_config {
    int32_t leak_q12;
    int32_t evidence_min_q12;
    int32_t evidence_max_q12;
    int32_t working_enter_q12;
    int32_t working_exit_q12;
    int32_t miss_evidence_q12;
    int32_t unknown_roi_scale_q12;
    int32_t maximum_period_bonus_q12;
    rf_v13_class_calibration_t classes[RF_V13_CLASS_COUNT];
} rf_v13_activity_config_t;

/* One accepted V12 detection. Box coordinates stay in the V12 result stream. */
typedef struct rf_v13_cpu0_evidence {
    uint64_t detection_time_us;
    uint16_t confidence_q15;
    int16_t period_bonus_q12;
    uint8_t class_id;
    uint8_t center_slot;
    uint8_t roi_decision;
    uint8_t evidence_flags;
} rf_v13_cpu0_evidence_t;

/* CPU0 emits one message only after closing a four-frequency scan round. */
typedef struct rf_v13_cpu0_round_message {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint16_t message_bytes;
    uint16_t evidence_count;
    uint32_t message_sequence;
    uint32_t round_index;
    uint32_t first_v12_tile_sequence;
    uint32_t last_v12_tile_sequence;
    uint16_t source_v12_abi_major;
    uint16_t source_v12_tile_bytes;
    uint64_t round_start_time_us;
    uint64_t round_end_time_us;
    uint32_t invalid_reason_flags;
    uint8_t expected_slot_mask;
    uint8_t observed_slot_mask;
    uint8_t valid_slot_mask;
    uint8_t round_flags;
    rf_v13_cpu0_evidence_t evidence[RF_V13_MAX_EVIDENCE_PER_ROUND];
    uint32_t display_session_id[RF_V13_DISPLAY_IDENTITY_COUNT];
    uint32_t display_window_sequence[RF_V13_DISPLAY_IDENTITY_COUNT];
    uint8_t display_identity_mask;
    uint8_t display_identity_conflict_mask;
    uint8_t reserved_identity[2];
    uint8_t reserved[164];
} rf_v13_cpu0_round_message_t;

typedef union rf_v13_ipc_slot {
    rf_v13_cpu0_round_message_t activity;
    uint8_t bytes[RF_V13_CPU0_MESSAGE_BYTES];
} rf_v13_ipc_slot_t;

typedef struct rf_v13_object_state {
    int32_t energy_q12;
    int32_t last_llr_q12;
    uint64_t last_message_time_us;
    uint64_t last_positive_time_us;
    uint64_t last_transition_time_us;
    uint32_t last_round_index;
    uint32_t last_reason_flags;
    uint32_t last_transition_reason_flags;
    uint32_t last_invalid_reason_flags;
    uint32_t last_message_sequence;
    uint8_t activity_state;
    uint8_t last_source_class;
    uint8_t last_band_mask;
    uint8_t last_center_slot;
    uint8_t last_round_complete;
    uint8_t last_round_valid;
    uint8_t last_observed_slot_mask;
    uint8_t last_valid_slot_mask;
    uint8_t last_positive_source_class;
    uint8_t last_positive_band_mask;
    uint8_t last_positive_center_slot;
    uint8_t reserved[5];
} rf_v13_object_state_t;

typedef struct rf_v13_activity_fusion {
    rf_v13_object_state_t objects[RF_V13_OBJECT_COUNT];
    uint32_t last_message_sequence;
    uint32_t last_round_index;
    uint8_t initialized;
    uint8_t reserved[7];
} rf_v13_activity_fusion_t;

extern const rf_v13_activity_config_t g_rf_v13_smoke_config;

void rf_v13_cpu0_round_message_init(
    rf_v13_cpu0_round_message_t *message,
    uint32_t message_sequence,
    uint32_t round_index,
    uint64_t round_start_time_us,
    uint64_t round_end_time_us
);

/* Returns 1 when appended, 0 when class/band is disallowed, -1 on overflow. */
int rf_v13_cpu0_add_v12_detection(
    rf_v13_cpu0_round_message_t *message,
    uint8_t v12_class_id,
    uint8_t center_slot,
    uint16_t confidence_q15,
    uint8_t roi_decision,
    int16_t period_bonus_q12,
    uint8_t v12_event_flags,
    uint64_t detection_time_us
);

void rf_v13_activity_fusion_init(rf_v13_activity_fusion_t *fusion);
int rf_v13_round_is_complete_valid(const rf_v13_cpu0_round_message_t *message);
int32_t rf_v13_lookup_llr_q12(
    const rf_v13_activity_config_t *config,
    uint8_t class_id,
    uint16_t confidence_q15
);
rf_v13_apply_result_t rf_v13_activity_fusion_apply_round(
    rf_v13_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v13_activity_config_t *config
);
rf_v13_apply_result_t rf_v13_activity_fusion_apply_smoke(
    rf_v13_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message
);
const rf_v13_object_state_t *rf_v13_activity_fusion_get(
    const rf_v13_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id
);

#define RF_V13_JOIN_INNER(left, right) left##right
#define RF_V13_JOIN(left, right) RF_V13_JOIN_INNER(left, right)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define RF_V13_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
#define RF_V13_STATIC_ASSERT(condition, message) \
    typedef char RF_V13_JOIN(rf_v13_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

RF_V13_STATIC_ASSERT(CHAR_BIT == 8, "V13 requires 8-bit bytes");
RF_V13_STATIC_ASSERT(sizeof(int32_t) == 4u, "V13 requires 32-bit int32_t");
RF_V13_STATIC_ASSERT(sizeof(uint64_t) == 8u, "V13 requires 64-bit uint64_t");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_cpu0_evidence_t) == 16u,
                     "V13 evidence ABI changed");
RF_V13_STATIC_ASSERT(offsetof(rf_v13_cpu0_evidence_t, detection_time_us) == 0u,
                     "V13 evidence timestamp offset changed");
RF_V13_STATIC_ASSERT(offsetof(rf_v13_cpu0_round_message_t, evidence) == 56u,
                     "V13 evidence array offset changed");
RF_V13_STATIC_ASSERT(
    offsetof(rf_v13_cpu0_round_message_t, display_session_id) == 312u,
    "V13 display session identity offset changed");
RF_V13_STATIC_ASSERT(
    offsetof(rf_v13_cpu0_round_message_t, display_window_sequence) == 328u,
    "V13 display window identity offset changed");
RF_V13_STATIC_ASSERT(
    offsetof(rf_v13_cpu0_round_message_t, display_identity_mask) == 344u,
    "V13 display identity mask offset changed");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_cpu0_round_message_t) == 512u,
                     "V13 CPU0 message must fit one 512-byte IPC slot");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_ipc_slot_t) == 512u,
                     "V13 IPC slot ABI changed");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_class_calibration_t) == 36u,
                     "V13 class calibration ABI changed");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_activity_config_t) == 212u,
                     "V13 activity config ABI changed");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_object_state_t) == 72u,
                     "V13 object state ABI changed");
RF_V13_STATIC_ASSERT(sizeof(rf_v13_activity_fusion_t) == 304u,
                     "V13 fusion state ABI changed");

#undef RF_V13_STATIC_ASSERT
#undef RF_V13_JOIN
#undef RF_V13_JOIN_INNER

#ifdef __cplusplus
}
#endif

#endif
