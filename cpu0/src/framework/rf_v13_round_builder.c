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

static bool rf_v18_round_builder_submit_internal(
    const rf_v12_tile_payload_t *tile,
    const uint16_t state_confidence_q15[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_roi_decision[RF_V12_MAX_BOXES_PER_TILE],
    const uint8_t state_quality_tier[RF_V12_MAX_BOXES_PER_TILE],
    uint32_t display_session_id,
    uint32_t display_window_sequence,
    bool display_identity_valid)
{
    rf_v12_tile_payload_t state_tile;
    rf_v18_event_aux_t event_aux[RF_V12_MAX_BOXES_PER_TILE];
    rf_v18_round_add_result_t add_result;
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
    if (!rf_v18_tile_events_valid(&state_tile))
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
        tile, NULL, NULL, NULL, 0U, 0U, false);
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
        true);
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
