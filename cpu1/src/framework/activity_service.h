#ifndef CPU1_RF_V25_ACTIVITY_SERVICE_H
#define CPU1_RF_V25_ACTIVITY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rf_v25_activity_fusion.h"

#define RF_V25_ACTIVITY_SERVICE_PROOF_MAGIC   (0x41353256UL) /* V25A */
#define RF_V25_ACTIVITY_SERVICE_PROOF_VERSION (1U)
#define RF_V25_ACTIVITY_SERVICE_FLAG_ACTIVE   (1UL << 0)
#define RF_V25_ACTIVITY_SERVICE_NO_RESULT     (0xFFFFFFFFUL)
#define RF_V25_ROUND_DECISION_HAS_MESSAGE     (1U << 0)
#define RF_V25_ROUND_DECISION_OUTPUT_VALID    (1U << 1)
#define RF_V25_ROUND_DECISION_CPU0_EPOCH_RESET (1U << 2)

typedef struct st_rf_v25_activity_round_decision
{
    uint32_t message_sequence;
    uint32_t round_index;
    uint32_t apply_result;
    uint32_t fusion_reset_count;
    uint32_t display_session_id[RF_V13_DISPLAY_IDENTITY_COUNT];
    uint32_t display_window_sequence[RF_V13_DISPLAY_IDENTITY_COUNT];
    uint8_t display_identity_mask;
    uint8_t display_identity_conflict_mask;
    uint8_t object_activity_state[RF_V13_OBJECT_COUNT];
    uint8_t flags;
    uint8_t reserved;
} rf_v25_activity_round_decision_t;

typedef struct st_rf_v25_activity_service_proof
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t flags;
    uint32_t fusion_reset_count;
    uint32_t mailbox_message_count;
    uint32_t applied_round_count;
    uint32_t held_invalid_round_count;
    uint32_t duplicate_round_count;
    uint32_t stale_round_count;
    uint32_t bad_message_count;
    uint32_t bad_argument_count;
    uint32_t last_apply_result;
    uint32_t last_message_sequence;
    uint32_t last_round_index;
    rf_v25_activity_fusion_t fusion;
} rf_v25_activity_service_proof_t;

extern volatile rf_v25_activity_service_proof_t g_rf_v25_activity_proof;
/* Diagnostic mirror retained outside the proof ABI for ELF/runtime checks. */
extern volatile uint32_t g_rf_v25_activity_output_generation;

void rf_v25_activity_service_init(void);
bool rf_v25_activity_service_poll(void);
bool rf_v25_activity_service_take_round_decision(
    rf_v25_activity_round_decision_t *decision);
void rf_v25_activity_service_snapshot(
    rf_v25_activity_service_proof_t *snapshot);

typedef char rf_v25_activity_service_fusion_offset_must_be_56[
    (offsetof(rf_v25_activity_service_proof_t, fusion) == 56U) ? 1 : -1];
typedef char rf_v25_activity_service_proof_size_must_be_592[
    (sizeof(rf_v25_activity_service_proof_t) == 592U) ? 1 : -1];
typedef char rf_v25_activity_round_decision_size_must_be_56[
    (sizeof(rf_v25_activity_round_decision_t) == 56U) ? 1 : -1];

#endif
