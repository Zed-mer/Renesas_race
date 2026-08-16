#include "rf_v13_round_builder.h"

#include <string.h>

#include "ipc_bridge.h"
#include "rf_v13_activity_fusion.h"
#include "rf_v18_round_builder.h"
#include "rf_v18_source_gate.h"

_Static_assert(RF_V13_DISPLAY_IDENTITY_COUNT == RF_V12_CENTER_COUNT,
               "display identity count must match the four RF centers");

typedef struct st_rf_v18_round_service
{
    rf_v18_round_builder_t builder;
    rf_v13_round_builder_stats_t stats;
    uint32_t next_message_sequence;
} rf_v18_round_service_t;

static rf_v18_round_service_t g_round_service;

static uint32_t rf_v18_round_next_sequence(void)
{
    uint32_t next = g_round_service.next_message_sequence + 1U;
    if (next == 0U)
    {
        next = 1U;
    }
    g_round_service.next_message_sequence = next;
    return next;
}

static bool rf_v18_tile_header_valid(const rf_v12_tile_payload_t *tile)
{
    static const uint64_t center_hz[RF_V12_CENTER_COUNT] =
    {
        RF_V12_CENTER_2420_HZ,
        RF_V12_CENTER_2464_HZ,
        RF_V12_CENTER_5760_HZ,
        RF_V12_CENTER_5816_HZ
    };

    return (tile != NULL) &&
           (tile->magic == RF_V12_ABI_MAGIC) &&
           (tile->abi_version_major == RF_V12_ABI_VERSION_MAJOR) &&
           (tile->abi_version_minor == RF_V12_ABI_VERSION_MINOR) &&
           (tile->sample_rate_hz == RF_V12_SAMPLE_RATE_HZ) &&
           (tile->tile_samples == RF_V12_TILE_SAMPLES) &&
           (tile->event_count <= RF_V12_MAX_BOXES_PER_TILE) &&
           (tile->center_index < RF_V12_CENTER_COUNT) &&
           (tile->capture_center_frequency_hz ==
            center_hz[tile->center_index]) &&
           (tile->capture_start_time_us <= tile->capture_end_time_us);
}

static bool rf_v18_tile_events_valid(const rf_v12_tile_payload_t *tile)
{
    uint32_t index;

    for (index = 0U; index < tile->event_count; ++index)
    {
        const rf_v12_visible_event_t *event = &tile->events[index];
        if ((event->class_id >= RF_V12_CLASS_COUNT) ||
            (event->confidence_q15 > RF_V12_CONFIDENCE_Q15_ONE) ||
            (event->frequency_low_offset_hz >=
             event->frequency_high_offset_hz) ||
            (event->visible_start_sample >= event->visible_end_sample) ||
            (event->visible_end_sample > RF_V12_TILE_SAMPLES))
        {
            return false;
        }
    }
    return true;
}

static bool rf_v18_round_publish(void)
{
    rf_v13_cpu0_round_message_t message;
    bool published;

    if (g_round_service.builder.active == 0U)
    {
        return false;
    }
    if (!rf_v18_round_builder_finish(&g_round_service.builder, &message))
    {
        return false;
    }
    if (message.invalid_reason_flags != RF_V13_INVALID_NONE)
    {
        g_round_service.stats.invalid_rounds++;
    }

    published = ipc_bridge_cpu0_activity_publish(&message);
    if (published)
    {
        g_round_service.stats.published_rounds++;
    }
    else
    {
        g_round_service.stats.mailbox_drops++;
    }
    g_round_service.stats.last_message_sequence = message.message_sequence;
    g_round_service.stats.last_round_index = message.round_index;
    return published;
}

static bool rf_v18_round_begin(const rf_v12_tile_payload_t *tile)
{
    return rf_v18_round_builder_begin(
               &g_round_service.builder,
               rf_v18_round_next_sequence(),
               tile->round_index,
               tile->capture_start_time_us) != 0;
}

static void rf_v18_build_event_aux(
    rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    rf_v18_event_aux_t event_aux[RF_V12_MAX_BOXES_PER_TILE])
{
    uint32_t index;

    memset(event_aux, 0,
           sizeof(rf_v18_event_aux_t) * RF_V12_MAX_BOXES_PER_TILE);
    for (index = 0U; index < tile->event_count; ++index)
    {
        event_aux[index].period_bonus_q12 = 0;
        event_aux[index].roi_decision =
            (state_roi_decision != NULL) ?
            state_roi_decision[index] : RF_V13_ROI_UNKNOWN;
        event_aux[index].quality_tier =
            (state_quality_tier != NULL) ?
            state_quality_tier[index] : RF_V18_QUALITY_NONE;

        if (state_confidence_q15 != NULL)
        {
            tile->events[index].confidence_q15 = state_confidence_q15[index];
        }
    }
}

static void rf_v18_round_record_display_identity(
    rf_v18_round_builder_t *builder,
    const rf_v12_tile_payload_t *tile,
    uint32_t display_session_id,
    uint32_t display_window_sequence)
{
    uint8_t slot_bit;

    if ((builder == NULL) || (tile == NULL) ||
        (tile->center_index >= RF_V13_DISPLAY_IDENTITY_COUNT))
    {
        return;
    }
    slot_bit = (uint8_t)(1U << tile->center_index);
    if (display_session_id == 0U)
    {
        builder->message.display_identity_conflict_mask |= slot_bit;
        return;
    }
    if ((builder->message.display_identity_mask & slot_bit) != 0U)
    {
        if ((builder->message.display_session_id[tile->center_index] !=
             display_session_id) ||
            (builder->message.display_window_sequence[tile->center_index] !=
             display_window_sequence))
        {
            builder->message.display_identity_conflict_mask |= slot_bit;
        }
        return;
    }

    builder->message.display_session_id[tile->center_index] =
        display_session_id;
    builder->message.display_window_sequence[tile->center_index] =
        display_window_sequence;
    builder->message.display_identity_mask |= slot_bit;
}

static uint32_t rf_v27_event_iou_q15(
    const rf_v12_visible_event_t *legacy,
    const rf_v27_absolute_aux_evidence_t *absolute)
{
    int64_t frequency_low;
    int64_t frequency_high;
    uint32_t time_low;
    uint32_t time_high;
    uint64_t intersection;
    uint64_t legacy_area;
    uint64_t absolute_area;
    uint64_t union_area;

    frequency_low =
        (legacy->frequency_low_offset_hz >
         absolute->frequency_low_offset_hz) ?
        legacy->frequency_low_offset_hz :
        absolute->frequency_low_offset_hz;
    frequency_high =
        (legacy->frequency_high_offset_hz <
         absolute->frequency_high_offset_hz) ?
        legacy->frequency_high_offset_hz :
        absolute->frequency_high_offset_hz;
    time_low = (legacy->visible_start_sample >
                absolute->visible_start_sample) ?
               legacy->visible_start_sample :
               absolute->visible_start_sample;
    time_high = (legacy->visible_end_sample <
                 absolute->visible_end_sample) ?
                legacy->visible_end_sample :
                absolute->visible_end_sample;
    if ((frequency_high <= frequency_low) || (time_high <= time_low))
    {
        return 0U;
    }

    intersection = (uint64_t)(frequency_high - frequency_low) *
                   (uint64_t)(time_high - time_low);
    legacy_area =
        (uint64_t)(legacy->frequency_high_offset_hz -
                   legacy->frequency_low_offset_hz) *
        (uint64_t)(legacy->visible_end_sample -
                   legacy->visible_start_sample);
    absolute_area =
        (uint64_t)(absolute->frequency_high_offset_hz -
                   absolute->frequency_low_offset_hz) *
        (uint64_t)(absolute->visible_end_sample -
                   absolute->visible_start_sample);
    union_area = legacy_area + absolute_area - intersection;
    return (union_area == 0U) ? 0U :
        (uint32_t)((intersection * RF_V12_CONFIDENCE_Q15_ONE) /
                   union_area);
}

static uint64_t rf_v27_abs_i64(int64_t value)
{
    return (uint64_t)((value < 0) ? -value : value);
}

static bool rf_v27_events_match(
    const rf_v12_visible_event_t *legacy,
    const rf_v27_absolute_aux_evidence_t *absolute,
    uint32_t *iou_q15)
{
    uint32_t iou;
    uint64_t frequency_center_delta_q1;
    uint64_t time_center_delta_q1;
    uint64_t maximum_frequency_span;
    uint64_t maximum_time_span;

    if ((legacy == NULL) || (absolute == NULL) ||
        (legacy->class_id != RF_V12_CLASS_DJI_CONTROL))
    {
        return false;
    }
    iou = rf_v27_event_iou_q15(legacy, absolute);
    if (iou_q15 != NULL)
    {
        *iou_q15 = iou;
    }
    if (iou >= 11468U) /* round(0.35 * 32767) */
    {
        return true;
    }

    frequency_center_delta_q1 = rf_v27_abs_i64(
        ((int64_t)legacy->frequency_low_offset_hz +
         legacy->frequency_high_offset_hz) -
        ((int64_t)absolute->frequency_low_offset_hz +
         absolute->frequency_high_offset_hz));
    time_center_delta_q1 = rf_v27_abs_i64(
        ((int64_t)legacy->visible_start_sample +
         legacy->visible_end_sample) -
        ((int64_t)absolute->visible_start_sample +
         absolute->visible_end_sample));
    maximum_frequency_span = (uint64_t)(
        legacy->frequency_high_offset_hz -
        legacy->frequency_low_offset_hz);
    if ((uint64_t)(absolute->frequency_high_offset_hz -
                   absolute->frequency_low_offset_hz) >
        maximum_frequency_span)
    {
        maximum_frequency_span = (uint64_t)(
            absolute->frequency_high_offset_hz -
            absolute->frequency_low_offset_hz);
    }
    maximum_time_span =
        legacy->visible_end_sample - legacy->visible_start_sample;
    if ((uint64_t)(absolute->visible_end_sample -
                   absolute->visible_start_sample) > maximum_time_span)
    {
        maximum_time_span =
            absolute->visible_end_sample -
            absolute->visible_start_sample;
    }

    /* Coordinates above are doubled centres.  This is equivalent to a
     * centre distance no greater than 0.40 of the larger span. */
    return (frequency_center_delta_q1 * 5U <=
            maximum_frequency_span * 4U) &&
           (time_center_delta_q1 * 5U <= maximum_time_span * 4U);
}

static int32_t rf_v27_legacy_dji_llr_q12(uint16_t confidence_q15)
{
    if (confidence_q15 >= 29490U)
    {
        return 11186;
    }
    if (confidence_q15 >= 24575U)
    {
        return 6144;
    }
    return (confidence_q15 >= 18022U) ? 2048 : 0;
}

static void rf_v27_map_legacy_evidence(
    const rf_v12_tile_payload_t *tile,
    const rf_v18_event_aux_t event_aux[RF_V12_MAX_BOXES_PER_TILE],
    uint16_t evidence_start,
    uint16_t evidence_end,
    uint16_t evidence_index[RF_V12_MAX_BOXES_PER_TILE])
{
    uint16_t cursor = evidence_start;

    for (uint32_t event = 0U;
         event < RF_V12_MAX_BOXES_PER_TILE;
         ++event)
    {
        evidence_index[event] = UINT16_MAX;
    }
    for (uint32_t event = 0U; event < tile->event_count; ++event)
    {
        const rf_v12_visible_event_t *source = &tile->events[event];
        const rf_v13_cpu0_evidence_t *stored;
        if ((event_aux[event].quality_tier == 0U) ||
            (cursor >= evidence_end))
        {
            continue;
        }
        stored = &g_round_service.builder.message.evidence[cursor];
        if ((stored->class_id == source->class_id) &&
            (stored->center_slot == tile->center_index) &&
            (stored->confidence_q15 == source->confidence_q15) &&
            (stored->detection_time_us == tile->capture_end_time_us))
        {
            evidence_index[event] = cursor++;
        }
    }
}

static bool rf_v27_append_aux_evidence(
    rf_v13_cpu0_round_message_t *message,
    const rf_v27_absolute_aux_evidence_t *absolute,
    bool corroborated)
{
    uint16_t evidence_index;
    int result;

    if ((message == NULL) || (absolute == NULL) ||
        (message->evidence_count >= RF_V13_MAX_EVIDENCE_PER_ROUND))
    {
        return false;
    }
    evidence_index = message->evidence_count;
    result = rf_v13_cpu0_add_v12_detection(
        message,
        RF_V13_CLASS_DJI_CONTROL,
        absolute->center_slot,
        absolute->confidence_q15,
        absolute->roi_decision,
        0,
        0U,
        absolute->detection_time_us);
    if (result <= 0)
    {
        return false;
    }
    (void)rf_v27_cpu0_set_canonical_llr(
        message, evidence_index, absolute->llr_q12);
    if (corroborated)
    {
        (void)rf_v27_cpu0_set_model_corroborated(message, evidence_index);
    }
    return true;
}

static void rf_v27_merge_absolute_aux(
    const rf_v12_tile_payload_t *tile,
    const rf_v18_event_aux_t event_aux[RF_V12_MAX_BOXES_PER_TILE],
    uint16_t evidence_start,
    const rf_v27_absolute_aux_evidence_t *absolute,
    size_t absolute_count)
{
    rf_v13_cpu0_round_message_t *message =
        &g_round_service.builder.message;
    uint16_t evidence_index[RF_V12_MAX_BOXES_PER_TILE];
    bool legacy_matched[RF_V12_MAX_BOXES_PER_TILE] = {false};

    if ((absolute == NULL) || (absolute_count == 0U))
    {
        return;
    }
    if (absolute_count > RF_V27_ABSOLUTE_AUX_MAX_PEAKS)
    {
        absolute_count = RF_V27_ABSOLUTE_AUX_MAX_PEAKS;
    }
    rf_v27_map_legacy_evidence(
        tile, event_aux, evidence_start, message->evidence_count,
        evidence_index);

    for (size_t candidate = 0U; candidate < absolute_count; ++candidate)
    {
        uint32_t best_iou_q15 = 0U;
        uint32_t best_event = UINT32_MAX;
        const rf_v27_absolute_aux_evidence_t *item = &absolute[candidate];

        if ((item->center_slot != tile->center_index) ||
            (item->confidence_q15 > RF_V12_CONFIDENCE_Q15_ONE) ||
            (item->llr_q12 < 0) ||
            (item->roi_decision > RF_V13_ROI_FAIL) ||
            (item->frequency_low_offset_hz >=
             item->frequency_high_offset_hz) ||
            (item->visible_start_sample >= item->visible_end_sample) ||
            (item->visible_end_sample > RF_V12_TILE_SAMPLES))
        {
            continue;
        }
        for (uint32_t event = 0U; event < tile->event_count; ++event)
        {
            uint32_t iou_q15 = 0U;
            if (!legacy_matched[event] &&
                rf_v27_events_match(&tile->events[event], item, &iou_q15) &&
                ((best_event == UINT32_MAX) || (iou_q15 > best_iou_q15)))
            {
                best_iou_q15 = iou_q15;
                best_event = event;
            }
        }

        if (best_event != UINT32_MAX)
        {
            const uint16_t index = evidence_index[best_event];
            legacy_matched[best_event] = true;
            if (index != UINT16_MAX)
            {
                rf_v13_cpu0_evidence_t *stored = &message->evidence[index];
                const int32_t legacy_llr = rf_v27_legacy_dji_llr_q12(
                    stored->confidence_q15);
                stored->roi_decision = RF_V13_ROI_PASS;
                if (item->llr_q12 > legacy_llr)
                {
                    (void)rf_v27_cpu0_set_canonical_llr(
                        message, index, item->llr_q12);
                }
                (void)rf_v27_cpu0_set_model_corroborated(message, index);
                continue;
            }
            (void)rf_v27_append_aux_evidence(message, item, true);
            continue;
        }
        (void)rf_v27_append_aux_evidence(message, item, false);
    }
}

static bool rf_v18_round_builder_submit_internal(
    const rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    uint32_t display_session_id,
    uint32_t display_window_sequence,
    bool display_identity_valid,
    const rf_v27_absolute_aux_evidence_t *v27_aux,
    size_t v27_aux_count)
{
    rf_v12_tile_payload_t state_tile;
    rf_v18_event_aux_t event_aux[RF_V12_MAX_BOXES_PER_TILE];
    rf_v18_round_add_result_t add_result;
    uint16_t evidence_start;
    bool events_valid;
    bool published = false;

    g_round_service.stats.submitted_tiles++;
    if (!rf_v18_tile_header_valid(tile))
    {
        g_round_service.stats.malformed_tiles++;
        if (g_round_service.builder.active != 0U)
        {
            g_round_service.builder.message.invalid_reason_flags |=
                RF_V13_INVALID_V12_ABI |
                RF_V13_INVALID_MALFORMED_EVIDENCE;
        }
        return false;
    }

    if ((g_round_service.builder.active != 0U) &&
        (tile->round_index != g_round_service.builder.message.round_index))
    {
        published = rf_v18_round_publish();
    }
    if ((g_round_service.builder.active == 0U) &&
        !rf_v18_round_begin(tile))
    {
        g_round_service.stats.malformed_tiles++;
        return published;
    }

    if (display_identity_valid)
    {
        rf_v18_round_record_display_identity(
            &g_round_service.builder,
            tile,
            display_session_id,
            display_window_sequence);
    }

    state_tile = *tile;
    rf_v18_build_event_aux(
        &state_tile,
        state_confidence_q15,
        state_roi_decision,
        state_quality_tier,
        event_aux);
    events_valid = rf_v18_tile_events_valid(&state_tile);
    if (!events_valid)
    {
        g_round_service.builder.message.invalid_reason_flags |=
            RF_V13_INVALID_MALFORMED_EVIDENCE;
    }
    if ((g_round_service.builder.tile_count != 0U) &&
        (state_tile.capture_start_time_us <
         g_round_service.builder.message.round_end_time_us))
    {
        g_round_service.builder.message.invalid_reason_flags |=
            RF_V13_INVALID_TIMESTAMP;
    }

    evidence_start = g_round_service.builder.message.evidence_count;
    add_result = rf_v18_round_builder_add_tile(
        &g_round_service.builder,
        &state_tile,
        event_aux,
        state_tile.event_count);
    if ((add_result == RF_V18_ROUND_BAD_ARGUMENT) ||
        (add_result == RF_V18_ROUND_BAD_STATE) ||
        (add_result == RF_V18_ROUND_BAD_TILE_HEADER))
    {
        g_round_service.stats.malformed_tiles++;
    }
    if (events_valid &&
        (state_tile.tile_validity == RF_V12_TILE_VALID) &&
        (state_tile.flags == 0U) &&
        ((add_result == RF_V18_ROUND_TILE_ACCEPTED) ||
         (add_result == RF_V18_ROUND_TILE_ACCEPTED_INVALID)))
    {
        rf_v27_merge_absolute_aux(
            &state_tile, event_aux, evidence_start,
            v27_aux, v27_aux_count);
    }
    if (g_round_service.builder.seen_slot_mask == RF_V13_CENTER_SLOT_MASK)
    {
        published = rf_v18_round_publish() || published;
    }
    return published;
}

void rf_v13_round_builder_init(void)
{
    memset(&g_round_service, 0, sizeof(g_round_service));
    rf_v18_round_builder_init(&g_round_service.builder);
}

void rf_v13_round_builder_reset(void)
{
    const uint32_t sequence = g_round_service.next_message_sequence;
    const rf_v13_round_builder_stats_t stats = g_round_service.stats;

    memset(&g_round_service, 0, sizeof(g_round_service));
    g_round_service.next_message_sequence = sequence;
    g_round_service.stats = stats;
    rf_v18_round_builder_init(&g_round_service.builder);
}

bool rf_v13_round_builder_submit(const rf_v12_tile_payload_t *tile)
{
    return rf_v18_round_builder_submit_internal(
        tile, NULL, NULL, NULL, 0U, 0U, false, NULL, 0U);
}

bool rf_v13_round_builder_submit_processed(
    const rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    uint32_t display_session_id,
    uint32_t display_window_sequence)
{
    return rf_v18_round_builder_submit_internal(
        tile,
        state_confidence_q15,
        state_roi_decision,
        state_quality_tier,
        display_session_id,
        display_window_sequence,
        true,
        NULL,
        0U);
}

bool rf_v13_round_builder_submit_processed_with_v27_aux(
    const rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    const rf_v27_absolute_aux_evidence_t *v27_aux,
    size_t v27_aux_count,
    uint32_t display_session_id,
    uint32_t display_window_sequence)
{
    return rf_v18_round_builder_submit_internal(
        tile,
        state_confidence_q15,
        state_roi_decision,
        state_quality_tier,
        display_session_id,
        display_window_sequence,
        true,
        v27_aux,
        v27_aux_count);
}

void rf_v13_round_builder_flush(void)
{
    (void)rf_v18_round_publish();
}

void rf_v13_round_builder_stats_get(rf_v13_round_builder_stats_t *stats)
{
    if (stats != NULL)
    {
        *stats = g_round_service.stats;
    }
}
