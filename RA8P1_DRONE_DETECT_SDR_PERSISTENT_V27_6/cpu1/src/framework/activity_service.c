#include "activity_service.h"

#include <string.h>

#include "ipc_bridge.h"
#include "rf_v25_activity_calibration.h"

typedef struct st_rf_v25_activity_service_context
{
    rf_v25_activity_fusion_t fusion;
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
    rf_v25_activity_round_decision_t pending_decision;
    bool decision_pending;
} rf_v25_activity_service_context_t;

static rf_v25_activity_service_context_t g_activity_service;

volatile rf_v25_activity_service_proof_t g_rf_v25_activity_proof;
volatile uint32_t g_rf_v25_activity_output_generation;

static void rf_v25_activity_service_publish_proof(void)
{
    rf_v25_activity_service_proof_t proof;

    memset(&proof, 0, sizeof(proof));
    proof.magic = RF_V25_ACTIVITY_SERVICE_PROOF_MAGIC;
    proof.version = RF_V25_ACTIVITY_SERVICE_PROOF_VERSION;
    proof.size = (uint16_t)sizeof(proof);
    proof.flags = RF_V25_ACTIVITY_SERVICE_FLAG_ACTIVE;
    proof.fusion_reset_count = g_activity_service.fusion_reset_count;
    proof.mailbox_message_count = g_activity_service.mailbox_message_count;
    proof.applied_round_count = g_activity_service.applied_round_count;
    proof.held_invalid_round_count =
        g_activity_service.held_invalid_round_count;
    proof.duplicate_round_count = g_activity_service.duplicate_round_count;
    proof.stale_round_count = g_activity_service.stale_round_count;
    proof.bad_message_count = g_activity_service.bad_message_count;
    proof.bad_argument_count = g_activity_service.bad_argument_count;
    proof.last_apply_result = g_activity_service.last_apply_result;
    proof.last_message_sequence = g_activity_service.last_message_sequence;
    proof.last_round_index = g_activity_service.last_round_index;
    proof.fusion = g_activity_service.fusion;
    g_rf_v25_activity_output_generation =
        rf_v25_activity_fusion_output_generation(&g_activity_service.fusion);
    g_rf_v25_activity_proof = proof;
}

void rf_v25_activity_service_init(void)
{
    memset(&g_activity_service, 0, sizeof(g_activity_service));
    rf_v25_activity_fusion_init(&g_activity_service.fusion);
    g_activity_service.last_apply_result =
        RF_V25_ACTIVITY_SERVICE_NO_RESULT;
    rf_v25_activity_service_publish_proof();
}

bool rf_v25_activity_service_poll(void)
{
    rf_v13_cpu0_round_message_t message;
    rf_v25_activity_round_decision_t decision;
    rf_v25_apply_result_t result;
    bool output_ready = false;
    bool cpu0_epoch_changed = false;
    const bool message_ready = ipc_bridge_cpu1_activity_poll(
        &message,
        &cpu0_epoch_changed);

    memset(&decision, 0, sizeof(decision));

    if (cpu0_epoch_changed)
    {
        rf_v25_activity_fusion_init(&g_activity_service.fusion);
        g_activity_service.fusion_reset_count++;
        g_activity_service.last_apply_result =
            RF_V25_ACTIVITY_SERVICE_NO_RESULT;
        g_activity_service.last_message_sequence = 0U;
        g_activity_service.last_round_index = 0U;
        decision.flags |= RF_V25_ROUND_DECISION_CPU0_EPOCH_RESET;
        decision.fusion_reset_count =
            g_activity_service.fusion_reset_count;
    }
    if (!message_ready)
    {
        if (cpu0_epoch_changed)
        {
            g_activity_service.pending_decision = decision;
            g_activity_service.decision_pending = true;
            rf_v25_activity_service_publish_proof();
        }
        return false;
    }

    decision.flags |= RF_V25_ROUND_DECISION_HAS_MESSAGE;
    decision.message_sequence = message.message_sequence;
    decision.round_index = message.round_index;
    decision.fusion_reset_count = g_activity_service.fusion_reset_count;
    memcpy(decision.display_session_id,
           message.display_session_id,
           sizeof(decision.display_session_id));
    memcpy(decision.display_window_sequence,
           message.display_window_sequence,
           sizeof(decision.display_window_sequence));
    decision.display_identity_mask = message.display_identity_mask;
    decision.display_identity_conflict_mask =
        message.display_identity_conflict_mask;
    g_activity_service.mailbox_message_count++;
    g_activity_service.last_message_sequence = message.message_sequence;
    g_activity_service.last_round_index = message.round_index;
    result = rf_v25_activity_fusion_apply_round(
        &g_activity_service.fusion,
        &message,
        &g_rf_v25_activity_config);
    g_activity_service.last_apply_result = (uint32_t)result;
    decision.apply_result = (uint32_t)result;

    switch (result)
    {
        case RF_V25_APPLY_OUTPUT_READY:
            g_activity_service.applied_round_count++;
            output_ready = true;
            decision.flags |= RF_V25_ROUND_DECISION_OUTPUT_VALID;
            for (uint32_t object = 0U;
                 object < RF_V13_OBJECT_COUNT;
                 ++object)
            {
                decision.object_activity_state[object] =
                    g_activity_service.fusion.objects[object].activity_state;
            }
            break;
        case RF_V25_APPLY_HELD_INVALID_NO_OUTPUT:
            g_activity_service.held_invalid_round_count++;
            break;
        case RF_V25_APPLY_IGNORED_DUPLICATE:
            g_activity_service.duplicate_round_count++;
            break;
        case RF_V25_APPLY_IGNORED_STALE:
            g_activity_service.stale_round_count++;
            break;
        case RF_V25_APPLY_BAD_MESSAGE:
            g_activity_service.bad_message_count++;
            break;
        case RF_V25_APPLY_BAD_ARGUMENT:
        default:
            g_activity_service.bad_argument_count++;
            break;
    }
    g_activity_service.pending_decision = decision;
    g_activity_service.decision_pending = true;
    rf_v25_activity_service_publish_proof();
    return output_ready;
}

bool rf_v25_activity_service_take_round_decision(
    rf_v25_activity_round_decision_t *decision)
{
    if ((decision == NULL) || !g_activity_service.decision_pending)
    {
        return false;
    }
    *decision = g_activity_service.pending_decision;
    memset(&g_activity_service.pending_decision, 0,
           sizeof(g_activity_service.pending_decision));
    g_activity_service.decision_pending = false;
    return true;
}

void rf_v25_activity_service_snapshot(
    rf_v25_activity_service_proof_t *snapshot)
{
    if (snapshot != NULL)
    {
        *snapshot = g_rf_v25_activity_proof;
    }
}
