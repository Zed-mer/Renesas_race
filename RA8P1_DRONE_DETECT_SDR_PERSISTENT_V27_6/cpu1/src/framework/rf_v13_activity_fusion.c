#include "rf_v13_activity_fusion.h"

#include <string.h>

#define RF_V13_CONFIDENCE_Q15_ONE UINT16_C(32767)
#define RF_V13_SOURCE_NONE UINT8_C(255)
#define RF_V13_CENTER_NONE UINT8_C(255)
#define RF_V13_LLR_NONE INT32_MIN

#define RF_V13_SMOKE_CLASS_CALIBRATION                                      \
    {                                                                       \
        3u, {0u, 0u, 0u},                                                   \
        {{UINT16_C(18022), INT16_C(2048)},                                  \
         {UINT16_C(24575), INT16_C(6144)},                                  \
         {UINT16_C(29490), INT16_C(12288)}}                                 \
    }

const rf_v13_activity_config_t g_rf_v13_smoke_config = {
    RF_V13_ENERGY_LEAK_Q12,
    RF_V13_ENERGY_MIN_Q12,
    RF_V13_ENERGY_MAX_Q12,
    RF_V13_WORKING_ENTER_Q12,
    RF_V13_WORKING_EXIT_Q12,
    RF_V13_MISS_EVIDENCE_Q12,
    RF_V13_UNKNOWN_ROI_SCALE_Q12,
    RF_V13_MAX_PERIOD_BONUS_Q12,
    {
        RF_V13_SMOKE_CLASS_CALIBRATION,
        RF_V13_SMOKE_CLASS_CALIBRATION,
        RF_V13_SMOKE_CLASS_CALIBRATION,
        RF_V13_SMOKE_CLASS_CALIBRATION,
        RF_V13_SMOKE_CLASS_CALIBRATION,
    }
};

static int32_t rf_v13_clip_i32(int64_t value, int32_t low, int32_t high)
{
    if (value < (int64_t)low) {
        return low;
    }
    if (value > (int64_t)high) {
        return high;
    }
    return (int32_t)value;
}

static int32_t rf_v13_q12_multiply(int32_t left, int32_t right)
{
    int64_t product = (int64_t)left * (int64_t)right;
    if (product >= 0) {
        return (int32_t)((product + INT64_C(2048)) >> 12);
    }
    return (int32_t)(-(((-product) + INT64_C(2048)) >> 12));
}

static int rf_v13_class_is_allowed_on_slot(uint8_t class_id, uint8_t center_slot)
{
    if (center_slot >= 4u || class_id >= RF_V13_CLASS_COUNT) {
        return 0;
    }
    if (class_id == RF_V13_CLASS_DJI_CONTROL ||
        class_id == RF_V13_CLASS_DJI_VIDEO) {
        return 1;
    }
    return center_slot < 2u;
}

static uint8_t rf_v13_object_for_class(uint8_t class_id)
{
    static const uint8_t mapping[RF_V13_CLASS_COUNT] = {
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_AT9S,
        RF_V13_OBJECT_T12,
        RF_V13_OBJECT_XIAOBAWANG,
    };
    return class_id < RF_V13_CLASS_COUNT ? mapping[class_id] : UINT8_C(255);
}

static uint8_t rf_v13_band_for_slot(uint8_t center_slot)
{
    return center_slot < 2u ? RF_V13_BAND_2P4_GHZ : RF_V13_BAND_5P8_GHZ;
}

static int rf_v13_config_is_valid(const rf_v13_activity_config_t *config)
{
    uint8_t class_id;
    if (config == NULL || config->leak_q12 < 0 ||
        config->leak_q12 > RF_V13_Q12_ONE ||
        config->evidence_min_q12 >= config->evidence_max_q12 ||
        config->working_exit_q12 >= config->working_enter_q12 ||
        config->unknown_roi_scale_q12 < 0 ||
        config->unknown_roi_scale_q12 > RF_V13_Q12_ONE ||
        config->maximum_period_bonus_q12 < 0 ||
        config->maximum_period_bonus_q12 > RF_V13_MAX_PERIOD_BONUS_Q12) {
        return 0;
    }
    for (class_id = 0u; class_id < RF_V13_CLASS_COUNT; ++class_id) {
        const rf_v13_class_calibration_t *table = &config->classes[class_id];
        uint8_t index;
        uint16_t previous = 0u;
        if (table->bin_count == 0u || table->bin_count > RF_V13_LLR_BIN_COUNT_MAX) {
            return 0;
        }
        for (index = 0u; index < table->bin_count; ++index) {
            if ((index > 0u && table->bins[index].minimum_confidence_q15 <= previous) ||
                table->bins[index].minimum_confidence_q15 > RF_V13_CONFIDENCE_Q15_ONE) {
                return 0;
            }
            previous = table->bins[index].minimum_confidence_q15;
        }
    }
    return 1;
}

static int rf_v13_message_header_is_valid(
    const rf_v13_cpu0_round_message_t *message)
{
    return message != NULL &&
           message->magic == RF_V13_ACTIVITY_MAGIC &&
           message->abi_major == RF_V13_ACTIVITY_ABI_MAJOR &&
           message->abi_minor == RF_V13_ACTIVITY_ABI_MINOR &&
           message->message_bytes == RF_V13_CPU0_MESSAGE_BYTES &&
           message->source_v12_abi_major == RF_V13_SOURCE_V12_ABI_MAJOR &&
           message->source_v12_tile_bytes == RF_V13_SOURCE_V12_TILE_BYTES &&
           message->evidence_count <= RF_V13_MAX_EVIDENCE_PER_ROUND &&
           message->expected_slot_mask == RF_V13_CENTER_SLOT_MASK &&
           message->round_start_time_us <= message->round_end_time_us;
}

static int rf_v13_evidence_is_well_formed(
    const rf_v13_cpu0_round_message_t *message,
    const rf_v13_cpu0_evidence_t *evidence)
{
    return evidence->class_id < RF_V13_CLASS_COUNT &&
           evidence->center_slot < 4u &&
           evidence->confidence_q15 <= RF_V13_CONFIDENCE_Q15_ONE &&
           evidence->roi_decision <= RF_V13_ROI_FAIL &&
           evidence->period_bonus_q12 >= 0 &&
           evidence->detection_time_us >= message->round_start_time_us &&
           evidence->detection_time_us <= message->round_end_time_us;
}

void rf_v13_cpu0_round_message_init(
    rf_v13_cpu0_round_message_t *message,
    uint32_t message_sequence,
    uint32_t round_index,
    uint64_t round_start_time_us,
    uint64_t round_end_time_us)
{
    if (message == NULL) {
        return;
    }
    memset(message, 0, sizeof(*message));
    message->magic = RF_V13_ACTIVITY_MAGIC;
    message->abi_major = RF_V13_ACTIVITY_ABI_MAJOR;
    message->abi_minor = RF_V13_ACTIVITY_ABI_MINOR;
    message->message_bytes = RF_V13_CPU0_MESSAGE_BYTES;
    message->message_sequence = message_sequence;
    message->round_index = round_index;
    message->source_v12_abi_major = RF_V13_SOURCE_V12_ABI_MAJOR;
    message->source_v12_tile_bytes = RF_V13_SOURCE_V12_TILE_BYTES;
    message->round_start_time_us = round_start_time_us;
    message->round_end_time_us = round_end_time_us;
    message->expected_slot_mask = RF_V13_CENTER_SLOT_MASK;
}

int rf_v13_cpu0_add_v12_detection(
    rf_v13_cpu0_round_message_t *message,
    uint8_t v12_class_id,
    uint8_t center_slot,
    uint16_t confidence_q15,
    uint8_t roi_decision,
    int16_t period_bonus_q12,
    uint8_t v12_event_flags,
    uint64_t detection_time_us)
{
    rf_v13_cpu0_evidence_t *target;
    if (message == NULL || !rf_v13_message_header_is_valid(message) ||
        confidence_q15 > RF_V13_CONFIDENCE_Q15_ONE ||
        roi_decision > RF_V13_ROI_FAIL || period_bonus_q12 < 0) {
        return -1;
    }
    if (!rf_v13_class_is_allowed_on_slot(v12_class_id, center_slot)) {
        return 0;
    }
    if (message->evidence_count >= RF_V13_MAX_EVIDENCE_PER_ROUND) {
        message->round_flags |= RF_V13_ROUND_EVIDENCE_TRUNCATED;
        message->invalid_reason_flags |= RF_V13_INVALID_RESULT_TRUNCATED;
        return -1;
    }
    target = &message->evidence[message->evidence_count++];
    target->detection_time_us = detection_time_us;
    target->confidence_q15 = confidence_q15;
    target->period_bonus_q12 = period_bonus_q12 > RF_V13_MAX_PERIOD_BONUS_Q12
                                   ? (int16_t)RF_V13_MAX_PERIOD_BONUS_Q12
                                   : period_bonus_q12;
    target->class_id = v12_class_id;
    target->center_slot = center_slot;
    target->roi_decision = roi_decision;
    target->evidence_flags = v12_event_flags & UINT8_C(0x1f);
    return 1;
}

void rf_v13_activity_fusion_init(rf_v13_activity_fusion_t *fusion)
{
    uint8_t object_id;
    if (fusion == NULL) {
        return;
    }
    memset(fusion, 0, sizeof(*fusion));
    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        rf_v13_object_state_t *state = &fusion->objects[object_id];
        state->activity_state = RF_V13_ACTIVITY_NO_RF_OBSERVED;
        state->last_source_class = RF_V13_SOURCE_NONE;
        state->last_center_slot = RF_V13_CENTER_NONE;
        state->last_positive_source_class = RF_V13_SOURCE_NONE;
        state->last_positive_center_slot = RF_V13_CENTER_NONE;
    }
}

int rf_v13_round_is_complete_valid(const rf_v13_cpu0_round_message_t *message)
{
    if (!rf_v13_message_header_is_valid(message)) {
        return 0;
    }
    return message->invalid_reason_flags == RF_V13_INVALID_NONE &&
           (message->round_flags &
            (RF_V13_ROUND_COMPLETE | RF_V13_ROUND_V12_TILES_VALIDATED)) ==
               (RF_V13_ROUND_COMPLETE | RF_V13_ROUND_V12_TILES_VALIDATED) &&
           (message->round_flags & RF_V13_ROUND_EVIDENCE_TRUNCATED) == 0u &&
           message->observed_slot_mask == RF_V13_CENTER_SLOT_MASK &&
           message->valid_slot_mask == RF_V13_CENTER_SLOT_MASK;
}

int32_t rf_v13_lookup_llr_q12(
    const rf_v13_activity_config_t *config,
    uint8_t class_id,
    uint16_t confidence_q15)
{
    const rf_v13_class_calibration_t *table;
    int32_t result = RF_V13_LLR_NONE;
    uint8_t index;
    if (!rf_v13_config_is_valid(config) || class_id >= RF_V13_CLASS_COUNT ||
        confidence_q15 > RF_V13_CONFIDENCE_Q15_ONE) {
        return RF_V13_LLR_NONE;
    }
    table = &config->classes[class_id];
    for (index = 0u; index < table->bin_count; ++index) {
        if (confidence_q15 < table->bins[index].minimum_confidence_q15) {
            break;
        }
        result = table->bins[index].llr_q12;
    }
    return result;
}

static uint8_t rf_v13_next_state(
    uint8_t previous_state,
    int32_t energy_q12,
    const rf_v13_activity_config_t *config)
{
    if (previous_state == RF_V13_ACTIVITY_WORKING) {
        return energy_q12 < config->working_exit_q12
                   ? RF_V13_ACTIVITY_NO_RF_OBSERVED
                   : RF_V13_ACTIVITY_WORKING;
    }
    if (energy_q12 >= config->working_enter_q12) {
        return RF_V13_ACTIVITY_WORKING;
    }
    if (energy_q12 > config->working_exit_q12) {
        return RF_V13_ACTIVITY_UNCERTAIN;
    }
    return RF_V13_ACTIVITY_NO_RF_OBSERVED;
}

static uint32_t rf_v13_transition_reason(uint8_t previous, uint8_t current)
{
    if (previous == current) {
        return RF_V13_REASON_NONE;
    }
    if (current == RF_V13_ACTIVITY_WORKING) {
        return RF_V13_REASON_ENTERED_WORKING;
    }
    if (previous == RF_V13_ACTIVITY_WORKING) {
        return RF_V13_REASON_EXITED_WORKING;
    }
    if (current == RF_V13_ACTIVITY_UNCERTAIN) {
        return RF_V13_REASON_ENTERED_UNCERTAIN;
    }
    return RF_V13_REASON_RETURNED_NO_RF;
}

rf_v13_apply_result_t rf_v13_activity_fusion_apply_round(
    rf_v13_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v13_activity_config_t *config)
{
    int32_t best_llr[RF_V13_OBJECT_COUNT];
    uint8_t best_index[RF_V13_OBJECT_COUNT];
    uint8_t accepted_count[RF_V13_OBJECT_COUNT];
    uint8_t dji_source_mask = 0u;
    uint8_t index;
    uint8_t object_id;
    int complete_valid;

    if (fusion == NULL || message == NULL || config == NULL) {
        return RF_V13_APPLY_BAD_ARGUMENT;
    }
    if (!rf_v13_message_header_is_valid(message) ||
        !rf_v13_config_is_valid(config)) {
        return RF_V13_APPLY_BAD_MESSAGE;
    }
    for (index = 0u; index < message->evidence_count; ++index) {
        if (!rf_v13_evidence_is_well_formed(message, &message->evidence[index])) {
            return RF_V13_APPLY_BAD_MESSAGE;
        }
    }
    if (fusion->initialized != 0u) {
        if (message->message_sequence == fusion->last_message_sequence ||
            message->round_index == fusion->last_round_index) {
            return RF_V13_APPLY_IGNORED_DUPLICATE;
        }
        if (message->message_sequence < fusion->last_message_sequence ||
            message->round_index < fusion->last_round_index) {
            return RF_V13_APPLY_IGNORED_STALE;
        }
    }

    complete_valid = rf_v13_round_is_complete_valid(message);
    if (!complete_valid) {
        for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
            rf_v13_object_state_t *state = &fusion->objects[object_id];
            state->last_llr_q12 = 0;
            state->last_message_time_us = message->round_end_time_us;
            state->last_round_index = message->round_index;
            state->last_reason_flags = RF_V13_REASON_INVALID_ROUND_HELD;
            state->last_invalid_reason_flags = message->invalid_reason_flags;
            state->last_message_sequence = message->message_sequence;
            state->last_round_complete =
                (message->round_flags & RF_V13_ROUND_COMPLETE) != 0u;
            state->last_round_valid = 0u;
            state->last_observed_slot_mask = message->observed_slot_mask;
            state->last_valid_slot_mask = message->valid_slot_mask;
        }
        fusion->last_message_sequence = message->message_sequence;
        fusion->last_round_index = message->round_index;
        fusion->initialized = 1u;
        return RF_V13_APPLY_HELD_INVALID_ROUND;
    }

    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        best_llr[object_id] = RF_V13_LLR_NONE;
        best_index[object_id] = UINT8_C(255);
        accepted_count[object_id] = 0u;
    }
    for (index = 0u; index < message->evidence_count; ++index) {
        const rf_v13_cpu0_evidence_t *item = &message->evidence[index];
        int32_t llr;
        int32_t roi_scale;
        int32_t bonus;
        if (!rf_v13_class_is_allowed_on_slot(item->class_id, item->center_slot) ||
            item->roi_decision == RF_V13_ROI_FAIL) {
            continue;
        }
        llr = rf_v13_lookup_llr_q12(config, item->class_id,
                                    item->confidence_q15);
        if (llr == RF_V13_LLR_NONE) {
            continue;
        }
        bonus = item->period_bonus_q12;
        if (bonus > config->maximum_period_bonus_q12) {
            bonus = config->maximum_period_bonus_q12;
        }
        roi_scale = item->roi_decision == RF_V13_ROI_PASS
                        ? RF_V13_Q12_ONE
                        : config->unknown_roi_scale_q12;
        llr = rf_v13_q12_multiply(llr + bonus, roi_scale);
        if (llr <= 0) {
            continue;
        }
        object_id = rf_v13_object_for_class(item->class_id);
        ++accepted_count[object_id];
        if (object_id == RF_V13_OBJECT_DJI) {
            dji_source_mask |= (uint8_t)(1u << item->class_id);
        }
        if (best_llr[object_id] == RF_V13_LLR_NONE ||
            llr > best_llr[object_id] ||
            (llr == best_llr[object_id] &&
             item->confidence_q15 >
                 message->evidence[best_index[object_id]].confidence_q15)) {
            best_llr[object_id] = llr;
            best_index[object_id] = index;
        }
    }

    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        rf_v13_object_state_t *state = &fusion->objects[object_id];
        uint8_t previous_state = state->activity_state;
        uint8_t current_state;
        int32_t llr = best_llr[object_id] == RF_V13_LLR_NONE
                          ? config->miss_evidence_q12
                          : best_llr[object_id];
        int32_t leaked = rf_v13_q12_multiply(state->energy_q12,
                                             config->leak_q12);
        int64_t unbounded = (int64_t)leaked + (int64_t)llr;
        uint32_t reasons = best_llr[object_id] == RF_V13_LLR_NONE
                               ? RF_V13_REASON_VALID_NEGATIVE
                               : RF_V13_REASON_VALID_POSITIVE;
        uint32_t transition;
        if (accepted_count[object_id] > 1u) {
            reasons |= RF_V13_REASON_DUPLICATE_EVIDENCE_COLLAPSED;
        }
        if (object_id == RF_V13_OBJECT_DJI &&
            (dji_source_mask & 0x03u) == 0x03u) {
            reasons |= RF_V13_REASON_DJI_MULTI_SOURCE;
        }
        if (fusion->initialized != 0u &&
            message->round_index > fusion->last_round_index + 1u) {
            reasons |= RF_V13_REASON_ROUND_INDEX_GAP;
        }
        if (unbounded > config->evidence_max_q12) {
            reasons |= RF_V13_REASON_CLIPPED_HIGH;
        } else if (unbounded < config->evidence_min_q12) {
            reasons |= RF_V13_REASON_CLIPPED_LOW;
        }
        state->energy_q12 = rf_v13_clip_i32(
            unbounded, config->evidence_min_q12, config->evidence_max_q12);
        current_state = rf_v13_next_state(previous_state, state->energy_q12,
                                           config);
        transition = rf_v13_transition_reason(previous_state, current_state);
        reasons |= transition;
        state->last_llr_q12 = llr;
        state->last_message_time_us = message->round_end_time_us;
        state->last_round_index = message->round_index;
        state->last_reason_flags = reasons;
        state->last_invalid_reason_flags = RF_V13_INVALID_NONE;
        state->last_message_sequence = message->message_sequence;
        state->activity_state = current_state;
        state->last_round_complete = 1u;
        state->last_round_valid = 1u;
        state->last_observed_slot_mask = message->observed_slot_mask;
        state->last_valid_slot_mask = message->valid_slot_mask;
        if (transition != RF_V13_REASON_NONE) {
            state->last_transition_reason_flags = transition;
            state->last_transition_time_us = message->round_end_time_us;
        }
        if (best_index[object_id] != UINT8_C(255)) {
            const rf_v13_cpu0_evidence_t *best =
                &message->evidence[best_index[object_id]];
            state->last_positive_time_us = best->detection_time_us;
            state->last_source_class = best->class_id;
            state->last_band_mask = rf_v13_band_for_slot(best->center_slot);
            state->last_center_slot = best->center_slot;
            state->last_positive_source_class = best->class_id;
            state->last_positive_band_mask =
                rf_v13_band_for_slot(best->center_slot);
            state->last_positive_center_slot = best->center_slot;
        }
    }
    fusion->last_message_sequence = message->message_sequence;
    fusion->last_round_index = message->round_index;
    fusion->initialized = 1u;
    return RF_V13_APPLY_OK;
}

rf_v13_apply_result_t rf_v13_activity_fusion_apply_smoke(
    rf_v13_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message)
{
    return rf_v13_activity_fusion_apply_round(
        fusion, message, &g_rf_v13_smoke_config);
}

const rf_v13_object_state_t *rf_v13_activity_fusion_get(
    const rf_v13_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id)
{
    if (fusion == NULL || object_id < RF_V13_OBJECT_DJI ||
        object_id >= RF_V13_OBJECT_COUNT) {
        return NULL;
    }
    return &fusion->objects[object_id];
}

#undef RF_V13_SMOKE_CLASS_CALIBRATION
#undef RF_V13_CONFIDENCE_Q15_ONE
#undef RF_V13_SOURCE_NONE
#undef RF_V13_CENTER_NONE
#undef RF_V13_LLR_NONE
