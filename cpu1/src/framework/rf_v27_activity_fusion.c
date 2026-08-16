#include "rf_v27_activity_fusion.h"

#include <limits.h>
#include <string.h>

#define RF_V27_CLASS_SLOT_COUNT 4u
#define RF_V27_CONFIDENCE_Q15_ONE UINT32_C(32767)

typedef struct rf_v27_round_object {
    int32_t on_llr_q12;
    uint8_t quality;
    uint8_t source_mask;
    uint8_t center_mask;
    uint8_t accepted_count;
    uint8_t model_corroborated;
} rf_v27_round_object_t;

int rf_v27_cpu0_set_model_corroborated(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index)
{
    if (message == NULL || evidence_index >= message->evidence_count ||
        evidence_index >= RF_V13_MAX_EVIDENCE_PER_ROUND) {
        return 0;
    }
    message->evidence[evidence_index].evidence_flags |=
        RF_V27_EVIDENCE_MODEL_CORROBORATED;
    return 1;
}

int rf_v27_cpu0_set_canonical_llr(
    rf_v13_cpu0_round_message_t *message,
    uint16_t evidence_index,
    int32_t llr_q12)
{
    rf_v13_cpu0_evidence_t *item;
    int64_t numerator;
    uint32_t confidence_q15;
    if (message == NULL || evidence_index >= message->evidence_count ||
        evidence_index >= RF_V13_MAX_EVIDENCE_PER_ROUND || llr_q12 < 0) {
        return 0;
    }
    if (llr_q12 > RF_V27_CANONICAL_LLR_MAX_Q12) {
        llr_q12 = RF_V27_CANONICAL_LLR_MAX_Q12;
    }
    /* Round-to-nearest conversion from q12 [0,8] to q15 [0,32767]. */
    numerator = (int64_t)llr_q12 * RF_V27_CONFIDENCE_Q15_ONE;
    confidence_q15 = (uint32_t)((numerator + RF_V27_CANONICAL_LLR_MAX_Q12 / 2) /
                                RF_V27_CANONICAL_LLR_MAX_Q12);
    if (confidence_q15 > RF_V27_CONFIDENCE_Q15_ONE) {
        confidence_q15 = RF_V27_CONFIDENCE_Q15_ONE;
    }
    item = &message->evidence[evidence_index];
    item->confidence_q15 = (uint16_t)confidence_q15;
    item->evidence_flags |= RF_V27_EVIDENCE_CANONICAL_LLR_Q15;
    return 1;
}

static uint16_t rf_v27_sat_inc_u16(uint16_t value)
{
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1u);
}

static uint16_t rf_v27_sat_dec_u16(uint16_t value)
{
    return value == 0u ? 0u : (uint16_t)(value - 1u);
}

static uint8_t rf_v27_popcount_u8(uint8_t value)
{
    uint8_t count = 0u;
    while (value != 0u) {
        count = (uint8_t)(count + (value & 1u));
        value >>= 1;
    }
    return count;
}

static int32_t rf_v27_mul_q12(int32_t left, int32_t right)
{
    int64_t product = (int64_t)left * (int64_t)right;
    if (product >= 0) {
        return (int32_t)((product + INT64_C(2048)) >> 12);
    }
    return (int32_t)(-(((-product) + INT64_C(2048)) >> 12));
}

static int32_t rf_v27_sat_add(int32_t left, int32_t right)
{
    int64_t value = (int64_t)left + (int64_t)right;
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

static int32_t rf_v27_clamp(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static uint8_t rf_v27_class_object(uint8_t class_id)
{
    static const uint8_t map[RF_V13_CLASS_COUNT] = {
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_AT9S,
        RF_V13_OBJECT_T12,
        RF_V13_OBJECT_XIAOBAWANG
    };
    return class_id < RF_V13_CLASS_COUNT ? map[class_id] : UINT8_MAX;
}

static int rf_v27_class_allowed(uint8_t class_id, uint8_t center_slot)
{
    if (class_id >= RF_V13_CLASS_COUNT || center_slot >= RF_V27_CLASS_SLOT_COUNT) {
        return 0;
    }
    return class_id == RF_V13_CLASS_DJI_CONTROL ||
                   class_id == RF_V13_CLASS_DJI_VIDEO || center_slot < 2u;
}

static int rf_v27_profile_valid(const rf_v27_device_profile_t *profile)
{
    if (profile == NULL || profile->on_hit_leak_q12 < 0 ||
        profile->on_hit_leak_q12 > RF_V13_Q12_ONE ||
        profile->on_miss_decay_q12 < 0 ||
        profile->on_miss_decay_q12 > RF_V13_Q12_ONE ||
        profile->weak_on_scale_q12 < 0 ||
        profile->weak_on_scale_q12 > RF_V13_Q12_ONE ||
        profile->off_weak_scale_q12 < 0 ||
        profile->off_weak_scale_q12 > RF_V13_Q12_ONE ||
        profile->off_support_decay_q12 < 0 ||
        profile->off_support_decay_q12 > RF_V13_Q12_ONE ||
        profile->on_enter_q12 <= 0 || profile->on_dual_enter_q12 <= 0 ||
        profile->on_cap_q12 < profile->on_enter_q12 ||
        profile->on_cap_q12 < profile->on_dual_enter_q12 ||
        profile->support_llr_q12 <= 0 ||
        profile->strong_llr_q12 < profile->support_llr_q12 ||
        profile->off_miss_llr_q12 <= 0 || profile->off_exit_q12 <= 0 ||
        profile->off_recent_strong_q12 <= 0 || profile->history_rounds == 0u ||
        profile->history_rounds > RF_V27_MAX_HISTORY_ROUNDS ||
        profile->single_support_rounds == 0u ||
        profile->single_support_rounds > profile->history_rounds ||
        profile->single_strong_rounds > profile->single_support_rounds ||
        profile->dual_support_rounds == 0u ||
        profile->dual_support_rounds > profile->history_rounds ||
        profile->dual_strong_rounds > profile->dual_support_rounds ||
        profile->candidate_timeout_rounds == 0u ||
        profile->strong_memory_rounds == 0u ||
        profile->minimum_working_rounds == 0u || profile->exit_miss_rounds == 0u ||
        profile->recent_strong_exit_miss_rounds < profile->exit_miss_rounds) {
        return 0;
    }
    return 1;
}

static int rf_v27_config_valid(const rf_v27_activity_config_t *config)
{
    uint8_t object_id;
    uint8_t class_id;
    if (config == NULL || config->evidence.leak_q12 < 0 ||
        config->evidence.leak_q12 > RF_V13_Q12_ONE ||
        config->evidence.evidence_min_q12 >=
            config->evidence.evidence_max_q12 ||
        config->evidence.working_exit_q12 >=
            config->evidence.working_enter_q12 ||
        config->evidence.unknown_roi_scale_q12 < 0 ||
        config->evidence.unknown_roi_scale_q12 > RF_V13_Q12_ONE ||
        config->evidence.maximum_period_bonus_q12 < 0 ||
        config->evidence.maximum_period_bonus_q12 >
            RF_V13_MAX_PERIOD_BONUS_Q12 ||
        config->multi_center_scale_q12 < 0 ||
        config->multi_center_scale_q12 > RF_V13_Q12_ONE ||
        config->multi_center_bonus_cap_q12 < 0 ||
        config->roi_fail_agreement_scale_q12 < 0 ||
        config->roi_fail_agreement_scale_q12 > RF_V13_Q12_ONE ||
        config->model_agreement_bonus_q12 < 0 ||
        config->t12_hop_bonus_q12 < 0 ||
        config->t12_hop_bonus_q12 > config->evidence.maximum_period_bonus_q12) {
        return 0;
    }
    for (class_id = 0u; class_id < RF_V13_CLASS_COUNT; ++class_id) {
        const rf_v13_class_calibration_t *table =
            &config->evidence.classes[class_id];
        uint8_t index;
        uint16_t previous = 0u;
        if (table->bin_count == 0u ||
            table->bin_count > RF_V13_LLR_BIN_COUNT_MAX) {
            return 0;
        }
        for (index = 0u; index < table->bin_count; ++index) {
            if ((index > 0u &&
                 table->bins[index].minimum_confidence_q15 <= previous) ||
                table->bins[index].minimum_confidence_q15 >
                    RF_V27_CONFIDENCE_Q15_ONE) {
                return 0;
            }
            previous = table->bins[index].minimum_confidence_q15;
        }
    }
    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        if (!rf_v27_profile_valid(&config->profiles[object_id])) {
            return 0;
        }
    }
    return 1;
}

static int32_t rf_v27_event_llr(
    const rf_v13_cpu0_evidence_t *item,
    const rf_v27_activity_config_t *config)
{
    int32_t llr;
    int32_t bonus;
    int32_t scale;
    int64_t canonical;
    if (item == NULL || item->class_id >= RF_V13_CLASS_COUNT ||
        !rf_v27_class_allowed(item->class_id, item->center_slot)) {
        return 0;
    }
    if ((item->evidence_flags & RF_V27_EVIDENCE_CANONICAL_LLR_Q15) != 0u) {
        canonical = (int64_t)item->confidence_q15 *
                    RF_V27_CANONICAL_LLR_MAX_Q12;
        llr = (int32_t)((canonical + RF_V27_CONFIDENCE_Q15_ONE / 2) /
                        RF_V27_CONFIDENCE_Q15_ONE);
    } else {
        llr = rf_v13_lookup_llr_q12(
            &config->evidence, item->class_id, item->confidence_q15);
    }
    if (llr == INT32_MIN) {
        return 0;
    }
    bonus = item->period_bonus_q12;
    if (bonus > config->evidence.maximum_period_bonus_q12) {
        bonus = config->evidence.maximum_period_bonus_q12;
    }
    if (item->roi_decision == RF_V13_ROI_PASS) {
        scale = RF_V13_Q12_ONE;
    } else if (item->roi_decision == RF_V13_ROI_UNKNOWN) {
        scale = config->evidence.unknown_roi_scale_q12;
    } else if ((item->evidence_flags &
                RF_V27_EVIDENCE_MODEL_CORROBORATED) != 0u) {
        /* A failed ROI may retain weak candidate memory only when two model
         * views corroborate the same physical event. */
        scale = config->roi_fail_agreement_scale_q12;
    } else {
        return 0;
    }
    llr = rf_v27_mul_q12(rf_v27_sat_add(llr, bonus), scale);
    if ((item->evidence_flags & RF_V27_EVIDENCE_MODEL_CORROBORATED) != 0u) {
        llr = rf_v27_sat_add(llr, config->model_agreement_bonus_q12);
    }
    return llr > 0 ? llr : 0;
}

static void rf_v27_collect_round(
    const rf_v13_cpu0_round_message_t *message,
    const rf_v27_activity_config_t *config,
    rf_v27_round_object_t objects[RF_V13_OBJECT_COUNT])
{
    int32_t best[RF_V13_CLASS_COUNT][RF_V27_CLASS_SLOT_COUNT];
    uint8_t seen[RF_V13_CLASS_COUNT][RF_V27_CLASS_SLOT_COUNT];
    uint16_t index;
    uint8_t class_id;
    uint8_t slot;
    uint8_t object_id;

    memset(best, 0, sizeof(best));
    memset(seen, 0, sizeof(seen));
    memset(objects, 0, sizeof(*objects) * RF_V13_OBJECT_COUNT);
    for (index = 0u; index < message->evidence_count; ++index) {
        const rf_v13_cpu0_evidence_t *item = &message->evidence[index];
        int32_t llr = rf_v27_event_llr(item, config);
        if (llr <= 0 || item->class_id >= RF_V13_CLASS_COUNT ||
            item->center_slot >= RF_V27_CLASS_SLOT_COUNT) {
            continue;
        }
        class_id = item->class_id;
        slot = item->center_slot;
        object_id = rf_v27_class_object(class_id);
        if (objects[object_id].accepted_count != UINT8_MAX) {
            objects[object_id].accepted_count = (uint8_t)(
                objects[object_id].accepted_count + 1u);
        }
        if ((item->evidence_flags &
             RF_V27_EVIDENCE_MODEL_CORROBORATED) != 0u) {
            objects[object_id].model_corroborated = 1u;
        }
        if (seen[class_id][slot] == 0u || llr > best[class_id][slot]) {
            seen[class_id][slot] = 1u;
            best[class_id][slot] = llr;
        }
    }

    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        const rf_v27_device_profile_t *profile = &config->profiles[object_id];
        int32_t maximum = 0;
        int32_t extras = 0;
        for (class_id = 0u; class_id < RF_V13_CLASS_COUNT; ++class_id) {
            if (rf_v27_class_object(class_id) != object_id) {
                continue;
            }
            for (slot = 0u; slot < RF_V27_CLASS_SLOT_COUNT; ++slot) {
                int32_t value;
                if (seen[class_id][slot] == 0u) {
                    continue;
                }
                value = best[class_id][slot];
                objects[object_id].source_mask |= (uint8_t)(1u << class_id);
                objects[object_id].center_mask |= (uint8_t)(1u << slot);
                if (value > maximum) {
                    extras = rf_v27_sat_add(extras, maximum);
                    maximum = value;
                } else {
                    extras = rf_v27_sat_add(extras, value);
                }
            }
        }
        if (maximum <= 0) {
            objects[object_id].quality = RF_V27_QUALITY_NONE;
            continue;
        }
        {
            int32_t bonus = rf_v27_mul_q12(
                config->multi_center_scale_q12, extras);
            if (bonus > config->multi_center_bonus_cap_q12) {
                bonus = config->multi_center_bonus_cap_q12;
            }
            objects[object_id].on_llr_q12 = rf_v27_clamp(
                rf_v27_sat_add(maximum, bonus), 0, profile->on_cap_q12);
        }
        objects[object_id].quality =
            objects[object_id].on_llr_q12 >= profile->strong_llr_q12
                ? RF_V27_QUALITY_STRONG
                : objects[object_id].on_llr_q12 >= profile->support_llr_q12
                      ? RF_V27_QUALITY_NORMAL
                      : RF_V27_QUALITY_WEAK;
    }
}

static uint32_t rf_v27_transition_reason(uint8_t previous, uint8_t current)
{
    if (previous == current) {
        return RF_V27_REASON_NONE;
    }
    return current == RF_V27_ACTIVITY_WORKING
               ? RF_V27_REASON_ENTERED_WORKING
               : RF_V27_REASON_EXITED_WORKING;
}

void rf_v27_activity_fusion_init(rf_v27_activity_fusion_t *fusion)
{
    if (fusion == NULL) {
        return;
    }
    memset(fusion, 0, sizeof(*fusion));
    rf_v13_activity_fusion_init(&fusion->evidence);
    for (uint8_t object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        fusion->objects[object_id].memory_phase = RF_V27_MEMORY_NO_MEMORY;
        fusion->objects[object_id].activity_state =
            RF_V27_ACTIVITY_NO_RF_OBSERVED;
        fusion->objects[object_id].rounds_since_strong = UINT16_MAX;
    }
}

rf_v27_apply_result_t rf_v27_activity_fusion_apply_round(
    rf_v27_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v27_activity_config_t *config)
{
    rf_v13_apply_result_t base_result;
    rf_v27_round_object_t rounds[RF_V13_OBJECT_COUNT];
    uint8_t object_id;

    if (fusion == NULL || message == NULL || !rf_v27_config_valid(config)) {
        return RF_V27_APPLY_BAD_ARGUMENT;
    }
    base_result = rf_v13_activity_fusion_apply_round(
        &fusion->evidence, message, &config->evidence);
    if (base_result == RF_V13_APPLY_HELD_INVALID_ROUND) {
        for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
            fusion->objects[object_id].last_reason_flags =
                RF_V27_REASON_INVALID_ROUND_FROZEN;
        }
        return RF_V27_APPLY_HELD_INVALID_NO_OUTPUT;
    }
    if (base_result == RF_V13_APPLY_IGNORED_DUPLICATE) {
        return RF_V27_APPLY_IGNORED_DUPLICATE;
    }
    if (base_result == RF_V13_APPLY_IGNORED_STALE) {
        return RF_V27_APPLY_IGNORED_STALE;
    }
    if (base_result == RF_V13_APPLY_BAD_ARGUMENT) {
        return RF_V27_APPLY_BAD_ARGUMENT;
    }
    if (base_result != RF_V13_APPLY_OK) {
        return RF_V27_APPLY_BAD_MESSAGE;
    }

    rf_v27_collect_round(message, config, rounds);
    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        const rf_v27_device_profile_t *profile = &config->profiles[object_id];
        const rf_v27_round_object_t *observation = &rounds[object_id];
        rf_v27_object_state_t *state = &fusion->objects[object_id];
        uint8_t previous_phase = state->memory_phase;
        uint8_t previous_state = state->activity_state;
        uint8_t mask = profile->history_rounds == 8u
                           ? UINT8_C(0xff)
                           : (uint8_t)((1u << profile->history_rounds) - 1u);
        uint8_t normal = observation->quality >= RF_V27_QUALITY_NORMAL;
        uint8_t strong = observation->quality == RF_V27_QUALITY_STRONG;
        uint8_t weak = observation->quality == RF_V27_QUALITY_WEAK;
        uint8_t support = normal ? 1u : 0u;
        uint8_t strong_support = strong ? 1u : 0u;
        uint8_t control = object_id == RF_V13_OBJECT_DJI && support != 0u &&
                                  (observation->source_mask &
                                   (1u << RF_V13_CLASS_DJI_CONTROL)) != 0u;
        uint8_t video = object_id == RF_V13_OBJECT_DJI && support != 0u &&
                                (observation->source_mask &
                                 (1u << RF_V13_CLASS_DJI_VIDEO)) != 0u;
        uint8_t model_agreement =
            normal != 0u && observation->model_corroborated != 0u;
        int32_t on_evidence;
        int32_t off_evidence = state->off_evidence_q12;
        int32_t off_llr = 0;
        int32_t on_increment = observation->on_llr_q12;
        uint8_t entry_met;
        uint8_t phase;
        uint8_t current;
        uint32_t reasons = RF_V27_REASON_ROUND_OUTPUT_READY;

        state->support_history_bits = (uint8_t)(
            ((state->support_history_bits << 1) | support) & mask);
        state->strong_history_bits = (uint8_t)(
            ((state->strong_history_bits << 1) | strong_support) & mask);
        state->control_history_bits = (uint8_t)(
            ((state->control_history_bits << 1) | control) & mask);
        state->video_history_bits = (uint8_t)(
            ((state->video_history_bits << 1) | video) & mask);
        state->model_agreement_history_bits = (uint8_t)(
            ((state->model_agreement_history_bits << 1) |
             model_agreement) & mask);
        state->support_count = rf_v27_popcount_u8(state->support_history_bits);
        state->strong_count = rf_v27_popcount_u8(state->strong_history_bits);
        if (support != 0u) {
            reasons |= RF_V27_REASON_ON_SUPPORT;
        } else if (weak) {
            reasons |= RF_V27_REASON_ON_WEAK;
        }
        if (object_id == RF_V13_OBJECT_T12 && support != 0u &&
            state->last_positive_center_mask != 0u &&
            observation->center_mask != 0u &&
            (state->last_positive_center_mask & observation->center_mask) == 0u) {
            on_increment = rf_v27_sat_add(on_increment, config->t12_hop_bonus_q12);
            reasons |= RF_V27_REASON_T12_HOP_BONUS;
        }
        if (normal) {
            on_evidence = rf_v27_sat_add(
                rf_v27_mul_q12(profile->on_hit_leak_q12,
                               state->on_evidence_q12),
                on_increment);
        } else if (weak) {
            on_evidence = rf_v27_sat_add(
                rf_v27_mul_q12(profile->on_miss_decay_q12,
                               state->on_evidence_q12),
                rf_v27_mul_q12(profile->weak_on_scale_q12,
                               observation->on_llr_q12));
        } else {
            on_evidence = rf_v27_mul_q12(
                profile->on_miss_decay_q12, state->on_evidence_q12);
        }
        on_evidence = rf_v27_clamp(on_evidence, 0, profile->on_cap_q12);

        state->consecutive_empty_rounds =
            normal ? 0u
                   : weak ? rf_v27_sat_dec_u16(state->consecutive_empty_rounds)
                          : rf_v27_sat_inc_u16(state->consecutive_empty_rounds);
        state->candidate_age_rounds =
            state->memory_phase == RF_V27_MEMORY_CANDIDATE
                ? rf_v27_sat_inc_u16(state->candidate_age_rounds)
                : 0u;
        state->working_age_rounds =
            (state->memory_phase == RF_V27_MEMORY_WORKING ||
             state->memory_phase == RF_V27_MEMORY_DECAYING)
                ? rf_v27_sat_inc_u16(state->working_age_rounds)
                : 0u;
        state->rounds_since_strong =
            strong ? 0u : rf_v27_sat_inc_u16(state->rounds_since_strong);

        {
            uint8_t single =
                state->support_count >= profile->single_support_rounds &&
                        state->strong_count >= profile->single_strong_rounds;
            uint8_t dual = object_id == RF_V13_OBJECT_DJI &&
                           ((state->control_history_bits != 0u &&
                             state->video_history_bits != 0u) ||
                            state->model_agreement_history_bits != 0u) &&
                           state->support_count >= profile->dual_support_rounds &&
                           state->strong_count >= profile->dual_strong_rounds;
            int32_t threshold = dual ? profile->on_dual_enter_q12
                                     : profile->on_enter_q12;
            entry_met = on_evidence >= threshold && (single || dual);
            if (single || dual) {
                reasons |= RF_V27_REASON_ENTRY_SUPPORT;
            }
            if (dual) {
                reasons |= RF_V27_REASON_ENTRY_DUAL_SOURCE;
            }
        }

        phase = previous_phase;
        if (phase == RF_V27_MEMORY_WORKING ||
            phase == RF_V27_MEMORY_DECAYING) {
            if (normal) {
                off_llr = rf_v27_mul_q12(
                    profile->off_support_decay_q12, off_evidence) -
                          off_evidence;
                off_evidence = rf_v27_mul_q12(
                    profile->off_support_decay_q12, off_evidence);
                phase = RF_V27_MEMORY_WORKING;
                reasons |= RF_V27_REASON_OFF_RECOVERED;
            } else if (weak) {
                off_llr = rf_v27_mul_q12(profile->off_miss_llr_q12,
                                         profile->off_weak_scale_q12);
                off_evidence = rf_v27_sat_add(off_evidence, off_llr);
                phase = RF_V27_MEMORY_DECAYING;
                reasons |= RF_V27_REASON_OFF_WEAK_MISS;
            } else {
                off_llr = profile->off_miss_llr_q12;
                off_evidence = rf_v27_sat_add(off_evidence, off_llr);
                phase = RF_V27_MEMORY_DECAYING;
                reasons |= RF_V27_REASON_OFF_FULL_MISS;
            }
            {
                uint8_t recent =
                    state->rounds_since_strong <= profile->strong_memory_rounds;
                uint16_t exit_rounds = recent
                                           ? profile->recent_strong_exit_miss_rounds
                                           : profile->exit_miss_rounds;
                int32_t exit_threshold = recent
                                              ? profile->off_recent_strong_q12
                                              : profile->off_exit_q12;
                if (phase == RF_V27_MEMORY_DECAYING &&
                    state->working_age_rounds >= profile->minimum_working_rounds &&
                    state->consecutive_empty_rounds >= exit_rounds &&
                    off_evidence >= exit_threshold) {
                    phase = RF_V27_MEMORY_NO_MEMORY;
                }
            }
        } else if (entry_met != 0u) {
            phase = RF_V27_MEMORY_WORKING;
            state->working_age_rounds = 0u;
            state->candidate_age_rounds = 0u;
            state->consecutive_empty_rounds = 0u;
            off_evidence = 0;
            off_llr = 0;
        } else if (observation->quality != RF_V27_QUALITY_NONE ||
                   on_evidence > 205 /* 0.05 Q12 */) {
            phase = RF_V27_MEMORY_CANDIDATE;
        } else {
            phase = RF_V27_MEMORY_NO_MEMORY;
        }

        if (phase == RF_V27_MEMORY_CANDIDATE &&
            entry_met != 0u) {
            phase = RF_V27_MEMORY_WORKING;
            state->working_age_rounds = 0u;
            state->candidate_age_rounds = 0u;
            state->consecutive_empty_rounds = 0u;
            off_evidence = 0;
            off_llr = 0;
        } else if (phase == RF_V27_MEMORY_CANDIDATE &&
                   state->consecutive_empty_rounds >=
                       profile->candidate_timeout_rounds &&
                   state->support_count == 0u &&
                   on_evidence < profile->support_llr_q12) {
            phase = RF_V27_MEMORY_NO_MEMORY;
        }
        if (phase == RF_V27_MEMORY_NO_MEMORY) {
            on_evidence = 0;
            off_evidence = 0;
            state->support_history_bits = 0u;
            state->strong_history_bits = 0u;
            state->control_history_bits = 0u;
            state->video_history_bits = 0u;
            state->model_agreement_history_bits = 0u;
            state->support_count = 0u;
            state->strong_count = 0u;
            state->consecutive_empty_rounds = 0u;
            state->candidate_age_rounds = 0u;
            state->working_age_rounds = 0u;
        }

        current = (phase == RF_V27_MEMORY_WORKING ||
                   phase == RF_V27_MEMORY_DECAYING)
                      ? RF_V27_ACTIVITY_WORKING
                      : RF_V27_ACTIVITY_NO_RF_OBSERVED;
        if (previous_state != current) {
            reasons |= rf_v27_transition_reason(previous_state, current);
            state->last_transition_time_us = message->round_end_time_us;
        }
        if (support != 0u && observation->center_mask != 0u) {
            state->last_positive_center_mask = observation->center_mask;
        }
        state->memory_phase = phase;
        state->activity_state = current;
        state->on_evidence_q12 = on_evidence;
        state->off_evidence_q12 = off_evidence;
        state->last_on_llr_q12 = on_increment;
        state->last_off_llr_q12 = off_llr;
        state->last_round_quality = observation->quality;
        state->last_source_mask = observation->source_mask;
        if (observation->accepted_count > 1u) {
            reasons |= RF_V27_REASON_DUPLICATES_CAPPED;
        }
        state->last_reason_flags = reasons;
    }
    fusion->output_generation += 1u;
    fusion->last_output_round_index = message->round_index;
    return RF_V27_APPLY_OUTPUT_READY;
}

int rf_v27_activity_fusion_get(
    const rf_v27_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id,
    rf_v27_activity_view_t *view)
{
    const rf_v13_object_state_t *base;
    const rf_v27_object_state_t *state;
    if (fusion == NULL || view == NULL || object_id < RF_V13_OBJECT_DJI ||
        object_id >= RF_V13_OBJECT_COUNT) {
        return 0;
    }
    base = &fusion->evidence.objects[object_id];
    state = &fusion->objects[object_id];
    memset(view, 0, sizeof(*view));
    view->last_positive_time_us = base->last_positive_time_us;
    view->last_transition_time_us = state->last_transition_time_us;
    view->on_evidence_q12 = state->on_evidence_q12;
    view->off_evidence_q12 = state->off_evidence_q12;
    view->last_on_llr_q12 = state->last_on_llr_q12;
    view->last_off_llr_q12 = state->last_off_llr_q12;
    view->last_reason_flags = state->last_reason_flags;
    view->last_invalid_reason_flags = base->last_invalid_reason_flags;
    view->last_round_index = base->last_round_index;
    view->working_age_rounds = state->working_age_rounds;
    view->consecutive_empty_rounds = state->consecutive_empty_rounds;
    view->candidate_age_rounds = state->candidate_age_rounds;
    view->rounds_since_strong = state->rounds_since_strong;
    view->activity_state = state->activity_state;
    view->memory_phase = state->memory_phase;
    view->last_positive_source_class = base->last_positive_source_class;
    view->last_positive_band_mask = base->last_positive_band_mask;
    view->last_positive_center_slot = base->last_positive_center_slot;
    view->last_round_complete = base->last_round_complete;
    view->last_round_valid = base->last_round_valid;
    view->support_history_bits = state->support_history_bits;
    view->support_count = state->support_count;
    view->strong_history_bits = state->strong_history_bits;
    view->strong_count = state->strong_count;
    view->last_round_quality = state->last_round_quality;
    view->last_positive_center_mask = state->last_positive_center_mask;
    view->model_agreement_history_bits =
        state->model_agreement_history_bits;
    return 1;
}

uint32_t rf_v27_activity_fusion_output_generation(
    const rf_v27_activity_fusion_t *fusion)
{
    return fusion == NULL ? 0u : fusion->output_generation;
}
