#include "rf_v25_activity_fusion.h"

#include <limits.h>
#include <string.h>

#define RF_V25_CLASS_SLOT_COUNT 4u

typedef struct rf_v25_round_object {
    int32_t on_llr_q12;
    uint8_t credible;
    uint8_t strong;
    uint8_t source_mask;
    uint8_t center_mask;
    uint8_t duplicate_count;
} rf_v25_round_object_t;

static uint16_t rf_v25_sat_inc_u16(uint16_t value)
{
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1u);
}

static uint16_t rf_v25_sat_sub_u16(uint16_t value, uint16_t decrement)
{
    return value > decrement ? (uint16_t)(value - decrement) : 0u;
}

static uint8_t rf_v25_popcount_u8(uint8_t value)
{
    uint8_t count = 0u;
    while (value != 0u) {
        count = (uint8_t)(count + (value & 1u));
        value >>= 1;
    }
    return count;
}

static int32_t rf_v25_q12_multiply(int32_t left, int32_t right)
{
    int64_t product = (int64_t)left * (int64_t)right;
    if (product >= 0) {
        return (int32_t)((product + INT64_C(2048)) >> 12);
    }
    return (int32_t)(-(((-product) + INT64_C(2048)) >> 12));
}

static int32_t rf_v25_sat_add_i32(int32_t left, int32_t right)
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

static int32_t rf_v25_clamp(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static int rf_v25_profile_is_valid(const rf_v25_device_profile_t *profile)
{
    if (profile == NULL || profile->on_hit_leak_q12 < 0 ||
        profile->on_hit_leak_q12 > RF_V13_Q12_ONE ||
        profile->on_miss_decay_q12 < 0 ||
        profile->on_miss_decay_q12 > RF_V13_Q12_ONE ||
        profile->weak_on_scale_q12 < 0 ||
        profile->weak_on_scale_q12 > RF_V13_Q12_ONE ||
        profile->weak_miss_scale_q12 < 0 ||
        profile->weak_miss_scale_q12 > RF_V13_Q12_ONE ||
        profile->normal_off_decay_q12 < 0 ||
        profile->normal_off_decay_q12 > RF_V13_Q12_ONE ||
        profile->multi_hit_scale_q12 < 0 ||
        profile->multi_hit_scale_q12 > RF_V13_Q12_ONE ||
        profile->on_enter_q12 <= 0 || profile->on_dual_enter_q12 <= 0 ||
        profile->on_cap_q12 < profile->on_enter_q12 ||
        profile->on_cap_q12 < profile->on_dual_enter_q12 ||
        profile->off_miss_llr_q12 <= 0 || profile->off_exit_q12 <= 0 ||
        profile->off_dual_exit_q12 <= 0 ||
        profile->off_cap_q12 < profile->off_exit_q12 ||
        profile->off_cap_q12 < profile->off_dual_exit_q12 ||
        profile->support_llr_q12 <= 0 ||
        profile->strong_llr_q12 < profile->support_llr_q12 ||
        profile->support_window_rounds == 0u ||
        profile->support_window_rounds > 8u ||
        profile->single_support_rounds == 0u ||
        profile->single_support_rounds > profile->support_window_rounds ||
        profile->single_strong_rounds > profile->single_support_rounds ||
        profile->dual_support_rounds == 0u ||
        profile->dual_support_rounds > profile->support_window_rounds ||
        profile->dual_strong_rounds > profile->dual_support_rounds ||
        profile->uncertain_exit_rounds == 0u ||
        profile->working_min_hold_rounds == 0u ||
        profile->exit_miss_rounds < 2u ||
        profile->dual_exit_miss_rounds < 2u) {
        return 0;
    }
    return 1;
}

static int rf_v25_config_is_valid(const rf_v25_activity_config_t *config)
{
    uint8_t object_id;
    if (config == NULL || config->t12_hop_bonus_q12 < 0 ||
        config->t12_hop_bonus_q12 >
            config->evidence.maximum_period_bonus_q12) {
        return 0;
    }
    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        if (!rf_v25_profile_is_valid(&config->profiles[object_id])) {
            return 0;
        }
    }
    return 1;
}

static uint8_t rf_v25_class_object(uint8_t class_id)
{
    static const uint8_t object_for_class[RF_V13_CLASS_COUNT] = {
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_AT9S,
        RF_V13_OBJECT_T12,
        RF_V13_OBJECT_XIAOBAWANG
    };
    return class_id < RF_V13_CLASS_COUNT ? object_for_class[class_id]
                                         : UINT8_MAX;
}

static int rf_v25_class_allowed(uint8_t class_id, uint8_t center_slot)
{
    if (class_id >= RF_V13_CLASS_COUNT || center_slot >= 4u) {
        return 0;
    }
    return class_id == RF_V13_CLASS_DJI_CONTROL ||
                   class_id == RF_V13_CLASS_DJI_VIDEO || center_slot < 2u;
}

static int32_t rf_v25_event_llr(
    const rf_v13_cpu0_evidence_t *item,
    const rf_v25_activity_config_t *config)
{
    int32_t llr;
    int32_t bonus;
    int32_t roi_scale;
    if (item == NULL || item->roi_decision == RF_V13_ROI_FAIL ||
        !rf_v25_class_allowed(item->class_id, item->center_slot)) {
        return 0;
    }
    llr = rf_v13_lookup_llr_q12(
        &config->evidence, item->class_id, item->confidence_q15);
    if (llr == INT32_MIN) {
        return 0;
    }
    bonus = item->period_bonus_q12;
    if (bonus > config->evidence.maximum_period_bonus_q12) {
        bonus = config->evidence.maximum_period_bonus_q12;
    }
    roi_scale = item->roi_decision == RF_V13_ROI_PASS
                    ? RF_V13_Q12_ONE
                    : config->evidence.unknown_roi_scale_q12;
    llr = rf_v25_q12_multiply(rf_v25_sat_add_i32(llr, bonus), roi_scale);
    return llr > 0 ? llr : 0;
}

static void rf_v25_collect_round(
    const rf_v13_cpu0_round_message_t *message,
    const rf_v25_activity_config_t *config,
    rf_v25_round_object_t round_objects[RF_V13_OBJECT_COUNT])
{
    int32_t best[RF_V13_CLASS_COUNT][RF_V25_CLASS_SLOT_COUNT];
    uint8_t strong[RF_V13_CLASS_COUNT][RF_V25_CLASS_SLOT_COUNT];
    uint8_t seen[RF_V13_CLASS_COUNT][RF_V25_CLASS_SLOT_COUNT];
    uint16_t index;
    uint8_t class_id;
    uint8_t slot;
    uint8_t object_id;

    memset(best, 0, sizeof(best));
    memset(strong, 0, sizeof(strong));
    memset(seen, 0, sizeof(seen));
    memset(round_objects, 0, sizeof(*round_objects) * RF_V13_OBJECT_COUNT);
    for (index = 0u; index < message->evidence_count; ++index) {
        const rf_v13_cpu0_evidence_t *item = &message->evidence[index];
        int32_t llr = rf_v25_event_llr(item, config);
        if (llr <= 0 || item->class_id >= RF_V13_CLASS_COUNT ||
            item->center_slot >= RF_V25_CLASS_SLOT_COUNT) {
            continue;
        }
        class_id = item->class_id;
        slot = item->center_slot;
        object_id = rf_v25_class_object(class_id);
        round_objects[object_id].duplicate_count = (uint8_t)(
            round_objects[object_id].duplicate_count == UINT8_MAX
                ? UINT8_MAX
                : round_objects[object_id].duplicate_count + 1u);
        if (seen[class_id][slot] == 0u || llr > best[class_id][slot]) {
            seen[class_id][slot] = 1u;
            best[class_id][slot] = llr;
            strong[class_id][slot] =
                (item->evidence_flags & RF_V25_EVIDENCE_STRONG_TEXTURE) != 0u
                    ? 1u
                    : 0u;
        }
    }

    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        const rf_v25_device_profile_t *profile = &config->profiles[object_id];
        int32_t maximum = 0;
        int32_t extras = 0;
        int32_t bonus;
        uint8_t dji_strong = 0u;
        for (class_id = 0u; class_id < RF_V13_CLASS_COUNT; ++class_id) {
            if (rf_v25_class_object(class_id) != object_id) {
                continue;
            }
            for (slot = 0u; slot < RF_V25_CLASS_SLOT_COUNT; ++slot) {
                int32_t value;
                if (seen[class_id][slot] == 0u) {
                    continue;
                }
                value = best[class_id][slot];
                round_objects[object_id].source_mask |=
                    (uint8_t)(1u << class_id);
                round_objects[object_id].center_mask |=
                    (uint8_t)(1u << slot);
                if (object_id == RF_V13_OBJECT_DJI &&
                    strong[class_id][slot] != 0u &&
                    value >= profile->strong_llr_q12) {
                    dji_strong = 1u;
                }
                if (value > maximum) {
                    extras = rf_v25_sat_add_i32(extras, maximum);
                    maximum = value;
                } else {
                    extras = rf_v25_sat_add_i32(extras, value);
                }
            }
        }
        if (maximum <= 0) {
            continue;
        }
        bonus = rf_v25_q12_multiply(profile->multi_hit_scale_q12, extras);
        if (bonus > profile->multi_hit_bonus_cap_q12) {
            bonus = profile->multi_hit_bonus_cap_q12;
        }
        round_objects[object_id].on_llr_q12 = rf_v25_clamp(
            rf_v25_sat_add_i32(maximum, bonus), 0, profile->on_cap_q12);
        round_objects[object_id].credible = 1u;
        round_objects[object_id].strong =
            round_objects[object_id].on_llr_q12 >= profile->strong_llr_q12 &&
                    (object_id != RF_V13_OBJECT_DJI || dji_strong != 0u)
                ? 1u
                : 0u;
    }
}

static uint32_t rf_v25_transition_reason(uint8_t previous, uint8_t current)
{
    if (previous == current) {
        return 0u;
    }
    if (current == RF_V25_ACTIVITY_WORKING) {
        return RF_V25_REASON_ENTERED_WORKING;
    }
    if (previous == RF_V25_ACTIVITY_WORKING) {
        return RF_V25_REASON_EXITED_WORKING;
    }
    if (current == RF_V25_ACTIVITY_UNCERTAIN) {
        return RF_V25_REASON_ENTERED_UNCERTAIN;
    }
    return RF_V25_REASON_RETURNED_NO_RF;
}

void rf_v25_activity_fusion_init(rf_v25_activity_fusion_t *fusion)
{
    if (fusion == NULL) {
        return;
    }
    memset(fusion, 0, sizeof(*fusion));
    rf_v13_activity_fusion_init(&fusion->evidence);
}

rf_v25_apply_result_t rf_v25_activity_fusion_apply_round(
    rf_v25_activity_fusion_t *fusion,
    const rf_v13_cpu0_round_message_t *message,
    const rf_v25_activity_config_t *config)
{
    rf_v13_apply_result_t base_result;
    rf_v25_round_object_t round_objects[RF_V13_OBJECT_COUNT];
    uint8_t object_id;

    if (fusion == NULL || message == NULL || !rf_v25_config_is_valid(config)) {
        return RF_V25_APPLY_BAD_ARGUMENT;
    }
    base_result = rf_v13_activity_fusion_apply_round(
        &fusion->evidence, message, &config->evidence);
    if (base_result == RF_V13_APPLY_HELD_INVALID_ROUND) {
        for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
            fusion->objects[object_id].last_reason_flags =
                RF_V25_REASON_INVALID_ROUND_FROZEN;
        }
        return RF_V25_APPLY_HELD_INVALID_NO_OUTPUT;
    }
    if (base_result == RF_V13_APPLY_IGNORED_DUPLICATE) {
        return RF_V25_APPLY_IGNORED_DUPLICATE;
    }
    if (base_result == RF_V13_APPLY_IGNORED_STALE) {
        return RF_V25_APPLY_IGNORED_STALE;
    }
    if (base_result == RF_V13_APPLY_BAD_ARGUMENT) {
        return RF_V25_APPLY_BAD_ARGUMENT;
    }
    if (base_result != RF_V13_APPLY_OK) {
        return RF_V25_APPLY_BAD_MESSAGE;
    }

    rf_v25_collect_round(message, config, round_objects);
    for (object_id = 0u; object_id < RF_V13_OBJECT_COUNT; ++object_id) {
        const rf_v25_device_profile_t *profile = &config->profiles[object_id];
        const rf_v25_round_object_t *round = &round_objects[object_id];
        rf_v25_object_state_t *state = &fusion->objects[object_id];
        uint8_t previous = state->activity_state;
        uint8_t history_mask = profile->support_window_rounds == 8u
                                   ? UINT8_C(0xff)
                                   : (uint8_t)(
                                         (1u << profile->support_window_rounds) -
                                         1u);
        uint8_t quality;
        uint8_t support;
        uint8_t strong_support;
        uint8_t control;
        uint8_t video;
        uint8_t same_round_dual;
        uint8_t dji_dual_recent;
        uint8_t single_entry_met;
        uint8_t dual_entry_met;
        uint8_t entry_met;
        uint8_t current;
        uint32_t transition;
        int32_t on_increment = round->on_llr_q12;
        int32_t on_evidence;
        int32_t off_evidence = 0;
        int32_t off_llr = 0;
        int32_t entry_threshold;
        int32_t exit_threshold;
        uint16_t active_exit_misses;
        uint8_t dji_dual_profile = state->dji_dual_profile;
        uint32_t reasons = RF_V25_REASON_ROUND_OUTPUT_READY;

        if (round->credible == 0u || round->on_llr_q12 <= 0) {
            quality = RF_V25_QUALITY_NONE;
        } else if (round->strong != 0u) {
            quality = RF_V25_QUALITY_STRONG;
        } else if (round->on_llr_q12 >= profile->support_llr_q12) {
            quality = RF_V25_QUALITY_NORMAL;
        } else {
            quality = RF_V25_QUALITY_WEAK;
        }
        support = quality == RF_V25_QUALITY_NORMAL ||
                          quality == RF_V25_QUALITY_STRONG
                      ? 1u
                      : 0u;
        strong_support = quality == RF_V25_QUALITY_STRONG ? 1u : 0u;
        control = object_id == RF_V13_OBJECT_DJI && support != 0u &&
                          (round->source_mask &
                           (1u << RF_V13_CLASS_DJI_CONTROL)) != 0u
                      ? 1u
                      : 0u;
        video = object_id == RF_V13_OBJECT_DJI && support != 0u &&
                        (round->source_mask &
                         (1u << RF_V13_CLASS_DJI_VIDEO)) != 0u
                    ? 1u
                    : 0u;
        same_round_dual = control != 0u && video != 0u ? 1u : 0u;
        state->support_history_bits = (uint8_t)(
            ((state->support_history_bits << 1) | support) & history_mask);
        state->strong_history_bits = (uint8_t)(
            ((state->strong_history_bits << 1) | strong_support) & history_mask);
        state->control_history_bits = (uint8_t)(
            ((state->control_history_bits << 1) | control) & history_mask);
        state->video_history_bits = (uint8_t)(
            ((state->video_history_bits << 1) | video) & history_mask);
        state->dual_source_history_bits = (uint8_t)(
            ((state->dual_source_history_bits << 1) | same_round_dual) &
            history_mask);
        state->support_count = rf_v25_popcount_u8(state->support_history_bits);
        state->strong_count = rf_v25_popcount_u8(state->strong_history_bits);
        state->dual_source_count =
            rf_v25_popcount_u8(state->dual_source_history_bits);
        dji_dual_recent = object_id == RF_V13_OBJECT_DJI &&
                                  state->control_history_bits != 0u &&
                                  state->video_history_bits != 0u
                              ? 1u
                              : 0u;
        if (dji_dual_recent != 0u) {
            reasons |= RF_V25_REASON_DJI_DUAL_RECENT;
        }
        if (support != 0u) {
            reasons |= RF_V25_REASON_ON_SUPPORT;
            if (object_id == RF_V13_OBJECT_T12 &&
                state->last_positive_center_mask != 0u &&
                round->center_mask != 0u &&
                (state->last_positive_center_mask & round->center_mask) == 0u) {
                on_increment = rf_v25_sat_add_i32(
                    on_increment, config->t12_hop_bonus_q12);
                reasons |= RF_V25_REASON_T12_HOP_BONUS;
            }
            on_evidence = rf_v25_sat_add_i32(
                rf_v25_q12_multiply(
                    profile->on_hit_leak_q12, state->on_evidence_q12),
                on_increment);
        } else {
            if (quality == RF_V25_QUALITY_WEAK) {
                reasons |= RF_V25_REASON_ON_WEAK;
            }
            on_evidence = rf_v25_sat_add_i32(
                rf_v25_q12_multiply(
                    profile->on_miss_decay_q12, state->on_evidence_q12),
                rf_v25_q12_multiply(
                    profile->weak_on_scale_q12, round->on_llr_q12));
        }
        on_evidence = rf_v25_clamp(on_evidence, 0, profile->on_cap_q12);

        if (strong_support != 0u) {
            state->consecutive_miss_rounds = 0u;
        } else if (support != 0u) {
            state->consecutive_miss_rounds = rf_v25_sat_sub_u16(
                state->consecutive_miss_rounds, 1u);
        } else {
            state->consecutive_miss_rounds =
                rf_v25_sat_inc_u16(state->consecutive_miss_rounds);
        }
        state->working_age_rounds =
            previous == RF_V25_ACTIVITY_WORKING
                ? rf_v25_sat_inc_u16(state->working_age_rounds)
                : 0u;

        if (previous == RF_V25_ACTIVITY_WORKING) {
            if (strong_support != 0u) {
                off_evidence = 0;
                off_llr = -state->off_evidence_q12;
                reasons |= RF_V25_REASON_OFF_STRONG_RESET;
            } else if (support != 0u) {
                off_evidence = rf_v25_q12_multiply(
                    profile->normal_off_decay_q12,
                    state->off_evidence_q12);
                off_llr = off_evidence - state->off_evidence_q12;
                reasons |= RF_V25_REASON_OFF_NORMAL_RECOVERY;
            } else if (quality == RF_V25_QUALITY_WEAK) {
                off_llr = rf_v25_q12_multiply(
                    profile->off_miss_llr_q12,
                    profile->weak_miss_scale_q12);
                off_evidence = rf_v25_sat_add_i32(
                    state->off_evidence_q12, off_llr);
                reasons |= RF_V25_REASON_OFF_WEAK_MISS;
            } else {
                off_llr = profile->off_miss_llr_q12;
                off_evidence = rf_v25_sat_add_i32(
                    state->off_evidence_q12, off_llr);
                reasons |= RF_V25_REASON_OFF_FULL_MISS;
            }
            off_evidence = rf_v25_clamp(off_evidence, 0, profile->off_cap_q12);
            dji_dual_profile = dji_dual_profile != 0u ||
                                       dji_dual_recent != 0u
                                   ? 1u
                                   : 0u;
        }

        single_entry_met =
            state->support_count >= profile->single_support_rounds &&
                    state->strong_count >= profile->single_strong_rounds
                ? 1u
                : 0u;
        dual_entry_met = object_id == RF_V13_OBJECT_DJI &&
                                 dji_dual_recent != 0u &&
                                 state->support_count >=
                                     profile->dual_support_rounds &&
                                 state->strong_count >=
                                     profile->dual_strong_rounds
                             ? 1u
                             : 0u;
        if (single_entry_met != 0u) {
            reasons |= RF_V25_REASON_ENTRY_SINGLE_WINDOW;
        }
        if (dual_entry_met != 0u) {
            reasons |= RF_V25_REASON_ENTRY_DUAL_WINDOW;
        }
        entry_threshold = dual_entry_met != 0u ? profile->on_dual_enter_q12
                                                : profile->on_enter_q12;
        entry_met = support != 0u && on_evidence >= entry_threshold &&
                            (single_entry_met != 0u || dual_entry_met != 0u)
                        ? 1u
                        : 0u;

        if (previous == RF_V25_ACTIVITY_WORKING) {
            active_exit_misses = object_id == RF_V13_OBJECT_DJI &&
                                         dji_dual_profile != 0u
                                     ? profile->dual_exit_miss_rounds
                                     : profile->exit_miss_rounds;
            exit_threshold = object_id == RF_V13_OBJECT_DJI &&
                                     dji_dual_profile != 0u
                                 ? profile->off_dual_exit_q12
                                 : profile->off_exit_q12;
            current = state->working_age_rounds >=
                              profile->working_min_hold_rounds &&
                              state->consecutive_miss_rounds >=
                                  active_exit_misses &&
                              off_evidence >= exit_threshold
                          ? RF_V25_ACTIVITY_NO_RF_OBSERVED
                          : RF_V25_ACTIVITY_WORKING;
        } else if (entry_met != 0u) {
            current = RF_V25_ACTIVITY_WORKING;
            state->working_age_rounds = 0u;
            state->consecutive_miss_rounds = 0u;
            off_evidence = 0;
            off_llr = 0;
            dji_dual_profile = dual_entry_met;
            active_exit_misses = dji_dual_profile != 0u
                                     ? profile->dual_exit_miss_rounds
                                     : profile->exit_miss_rounds;
        } else {
            active_exit_misses = profile->exit_miss_rounds;
            if (previous == RF_V25_ACTIVITY_NO_RF_OBSERVED) {
                current = support != 0u ? RF_V25_ACTIVITY_UNCERTAIN
                                        : RF_V25_ACTIVITY_NO_RF_OBSERVED;
            } else {
                current = state->consecutive_miss_rounds >=
                                  profile->uncertain_exit_rounds &&
                                  state->support_count == 0u &&
                                  on_evidence < profile->support_llr_q12
                              ? RF_V25_ACTIVITY_NO_RF_OBSERVED
                              : RF_V25_ACTIVITY_UNCERTAIN;
            }
            state->working_age_rounds = 0u;
            off_evidence = 0;
            off_llr = 0;
            dji_dual_profile = 0u;
        }

        transition = rf_v25_transition_reason(previous, current);
        reasons |= transition;
        if (previous == RF_V25_ACTIVITY_WORKING &&
            current == RF_V25_ACTIVITY_NO_RF_OBSERVED) {
            on_evidence = 0;
            off_evidence = 0;
            state->support_history_bits = 0u;
            state->strong_history_bits = 0u;
            state->control_history_bits = 0u;
            state->video_history_bits = 0u;
            state->dual_source_history_bits = 0u;
            state->support_count = 0u;
            state->strong_count = 0u;
            state->dual_source_count = 0u;
            state->consecutive_miss_rounds = 0u;
            state->working_age_rounds = 0u;
            dji_dual_profile = 0u;
        }
        if (transition != 0u) {
            state->last_transition_time_us = message->round_end_time_us;
        }
        if (support != 0u && round->center_mask != 0u) {
            state->last_positive_center_mask = round->center_mask;
        }
        state->activity_state = current;
        state->on_evidence_q12 = on_evidence;
        state->off_evidence_q12 = off_evidence;
        state->last_on_llr_q12 = on_increment;
        state->last_off_llr_q12 = off_llr;
        state->active_exit_miss_rounds = active_exit_misses;
        state->dji_dual_profile = dji_dual_profile;
        state->last_round_had_entry_support = support;
        state->last_round_had_strong_support = strong_support;
        state->last_round_had_dual_source = same_round_dual;
        state->last_round_quality = quality;
        if (round->duplicate_count > 1u) {
            reasons |= RF_V25_REASON_DUPLICATES_CAPPED;
        }
        state->last_reason_flags = reasons;
    }
    fusion->output_generation += 1u;
    fusion->last_output_round_index = message->round_index;
    return RF_V25_APPLY_OUTPUT_READY;
}

int rf_v25_activity_fusion_get(
    const rf_v25_activity_fusion_t *fusion,
    rf_v13_object_id_t object_id,
    rf_v25_activity_view_t *view)
{
    const rf_v13_object_state_t *evidence;
    const rf_v25_object_state_t *state;
    if (fusion == NULL || view == NULL || object_id < RF_V13_OBJECT_DJI ||
        object_id >= RF_V13_OBJECT_COUNT) {
        return 0;
    }
    evidence = &fusion->evidence.objects[object_id];
    state = &fusion->objects[object_id];
    memset(view, 0, sizeof(*view));
    view->last_positive_time_us = evidence->last_positive_time_us;
    view->last_transition_time_us = state->last_transition_time_us;
    view->on_evidence_q12 = state->on_evidence_q12;
    view->off_evidence_q12 = state->off_evidence_q12;
    view->last_on_llr_q12 = state->last_on_llr_q12;
    view->last_off_llr_q12 = state->last_off_llr_q12;
    view->last_reason_flags = state->last_reason_flags;
    view->last_invalid_reason_flags = evidence->last_invalid_reason_flags;
    view->last_round_index = evidence->last_round_index;
    view->working_age_rounds = state->working_age_rounds;
    view->consecutive_miss_rounds = state->consecutive_miss_rounds;
    view->active_exit_miss_rounds = state->active_exit_miss_rounds;
    view->activity_state = state->activity_state;
    view->last_positive_source_class = evidence->last_positive_source_class;
    view->last_positive_band_mask = evidence->last_positive_band_mask;
    view->last_positive_center_slot = evidence->last_positive_center_slot;
    view->last_round_complete = evidence->last_round_complete;
    view->last_round_valid = evidence->last_round_valid;
    view->support_history_bits = state->support_history_bits;
    view->support_count = state->support_count;
    view->strong_history_bits = state->strong_history_bits;
    view->strong_count = state->strong_count;
    view->control_history_bits = state->control_history_bits;
    view->video_history_bits = state->video_history_bits;
    view->dual_source_history_bits = state->dual_source_history_bits;
    view->dual_source_count = state->dual_source_count;
    view->last_round_quality = state->last_round_quality;
    view->dji_dual_profile = state->dji_dual_profile;
    view->last_positive_center_mask = state->last_positive_center_mask;
    return 1;
}

uint32_t rf_v25_activity_fusion_output_generation(
    const rf_v25_activity_fusion_t *fusion)
{
    return fusion == NULL ? 0u : fusion->output_generation;
}
