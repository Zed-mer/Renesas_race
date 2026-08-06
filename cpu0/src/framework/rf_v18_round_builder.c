#include "rf_v18_round_builder.h"

#include <string.h>

static const uint64_t rf_v18_center_hz[RF_V12_CENTER_COUNT] = {
    RF_V12_CENTER_2420_HZ,
    RF_V12_CENTER_2464_HZ,
    RF_V12_CENTER_5760_HZ,
    RF_V12_CENTER_5816_HZ,
};

static uint32_t rf_v18_invalid_flags_from_tile(uint8_t flags)
{
    uint32_t result = RF_V13_INVALID_NONE;
    if ((flags & RF_V12_TILE_CRC_ERROR) != 0u) {
        result |= RF_V13_INVALID_CRC;
    }
    if ((flags & RF_V12_TILE_PACKET_GAP) != 0u) {
        result |= RF_V13_INVALID_IQ_GAP;
    }
    if ((flags & RF_V12_TILE_RETUNE_UNLOCKED) != 0u) {
        result |= RF_V13_INVALID_RETUNE_UNLOCKED;
    }
    if ((flags & RF_V12_TILE_RING_OVERFLOW) != 0u) {
        result |= RF_V13_INVALID_RING_OVERFLOW;
    }
    if ((flags & (RF_V12_TILE_ADC_SATURATION |
                  RF_V12_TILE_CAPTURE_TIMEOUT)) != 0u) {
        result |= RF_V13_INVALID_CAPTURE;
    }
    if ((flags & RF_V12_TILE_BACKGROUND_RESET) != 0u) {
        result |= RF_V13_INVALID_BACKGROUND_NOT_READY;
    }
    if ((flags & RF_V12_TILE_RESULT_TRUNCATED) != 0u) {
        result |= RF_V13_INVALID_RESULT_TRUNCATED;
    }
    return result;
}

static int rf_v18_tile_header_is_valid(const rf_v12_tile_payload_t *tile)
{
    return tile != NULL && tile->magic == RF_V12_ABI_MAGIC &&
           tile->abi_version_major == RF_V12_ABI_VERSION_MAJOR &&
           tile->abi_version_minor == RF_V12_ABI_VERSION_MINOR &&
           tile->sample_rate_hz == RF_V12_SAMPLE_RATE_HZ &&
           tile->tile_samples == RF_V12_TILE_SAMPLES &&
           tile->event_count <= RF_V12_MAX_BOXES_PER_TILE &&
           tile->center_index < RF_V12_CENTER_COUNT &&
           tile->capture_center_frequency_hz ==
               rf_v18_center_hz[tile->center_index] &&
           tile->capture_start_time_us <= tile->capture_end_time_us;
}

void rf_v18_round_builder_init(rf_v18_round_builder_t *builder)
{
    if (builder != NULL) {
        memset(builder, 0, sizeof(*builder));
    }
}

int rf_v18_round_builder_begin(
    rf_v18_round_builder_t *builder,
    uint32_t message_sequence,
    uint32_t round_index,
    uint64_t round_start_time_us)
{
    if (builder == NULL || builder->active != 0u) {
        return 0;
    }
    memset(builder, 0, sizeof(*builder));
    rf_v13_cpu0_round_message_init(
        &builder->message,
        message_sequence,
        round_index,
        round_start_time_us,
        round_start_time_us);
    builder->active = 1u;
    return 1;
}

rf_v18_round_add_result_t rf_v18_round_builder_add_tile(
    rf_v18_round_builder_t *builder,
    const rf_v12_tile_payload_t *tile,
    const rf_v18_event_aux_t *event_aux,
    size_t event_aux_count)
{
    uint8_t slot;
    uint8_t slot_bit;
    uint16_t event_index;
    int tile_valid;
    if (builder == NULL || tile == NULL) {
        return RF_V18_ROUND_BAD_ARGUMENT;
    }
    if (builder->active == 0u) {
        return RF_V18_ROUND_BAD_STATE;
    }
    if (!rf_v18_tile_header_is_valid(tile)) {
        builder->message.invalid_reason_flags |=
            RF_V13_INVALID_V12_ABI | RF_V13_INVALID_MALFORMED_EVIDENCE;
        return RF_V18_ROUND_BAD_TILE_HEADER;
    }
    if (tile->round_index != builder->message.round_index) {
        builder->message.invalid_reason_flags |=
            RF_V13_INVALID_INCOMPLETE | RF_V13_INVALID_TILE_SEQUENCE;
        return RF_V18_ROUND_WRONG_ROUND;
    }
    slot = tile->center_index;
    slot_bit = (uint8_t)(1u << slot);
    if ((builder->seen_slot_mask & slot_bit) != 0u) {
        builder->message.invalid_reason_flags |= RF_V13_INVALID_TILE_SEQUENCE;
        return RF_V18_ROUND_DUPLICATE_SLOT;
    }
    if (builder->tile_count == 0u) {
        builder->message.first_v12_tile_sequence = tile->sequence;
        builder->message.round_start_time_us = tile->capture_start_time_us;
    } else {
        if (tile->sequence != builder->last_tile_sequence + 1u) {
            builder->message.invalid_reason_flags |=
                RF_V13_INVALID_TILE_SEQUENCE;
        }
        if (tile->capture_start_time_us <
            builder->message.round_start_time_us) {
            builder->message.round_start_time_us =
                tile->capture_start_time_us;
        }
    }
    if (tile->capture_end_time_us > builder->message.round_end_time_us) {
        builder->message.round_end_time_us = tile->capture_end_time_us;
    }
    builder->last_tile_sequence = tile->sequence;
    builder->message.last_v12_tile_sequence = tile->sequence;
    builder->tile_count = (uint8_t)(builder->tile_count + 1u);
    builder->seen_slot_mask |= slot_bit;
    builder->header_valid_slot_mask |= slot_bit;
    builder->message.observed_slot_mask = builder->seen_slot_mask;

    tile_valid = tile->tile_validity == RF_V12_TILE_VALID && tile->flags == 0u;
    if (tile->tile_validity == RF_V12_TILE_BACKGROUND_NOT_READY) {
        builder->message.invalid_reason_flags |=
            RF_V13_INVALID_BACKGROUND_NOT_READY;
    } else if (tile->tile_validity != RF_V12_TILE_VALID) {
        builder->message.invalid_reason_flags |= RF_V13_INVALID_CAPTURE;
    }
    builder->message.invalid_reason_flags |=
        rf_v18_invalid_flags_from_tile(tile->flags);
    if (tile_valid) {
        builder->message.valid_slot_mask |= slot_bit;
    }
    if (event_aux_count < tile->event_count ||
        (tile->event_count > 0u && event_aux == NULL)) {
        builder->message.invalid_reason_flags |=
            RF_V13_INVALID_MALFORMED_EVIDENCE;
        return RF_V18_ROUND_TILE_ACCEPTED_INVALID;
    }
    if (!tile_valid) {
        return RF_V18_ROUND_TILE_ACCEPTED_INVALID;
    }
    for (event_index = 0u; event_index < tile->event_count; ++event_index) {
        const rf_v12_visible_event_t *event = &tile->events[event_index];
        const rf_v18_event_aux_t *aux = &event_aux[event_index];
        uint16_t evidence_before = builder->message.evidence_count;
        int result;
        if (event->class_id >= RF_V12_CLASS_COUNT ||
            event->confidence_q15 > RF_V12_CONFIDENCE_Q15_ONE ||
            aux->roi_decision > RF_V13_ROI_FAIL ||
            aux->quality_tier > 2u || aux->period_bonus_q12 < 0) {
            builder->message.invalid_reason_flags |=
                RF_V13_INVALID_MALFORMED_EVIDENCE;
            continue;
        }
        if (aux->quality_tier == 0u) {
            continue;
        }
        result = rf_v13_cpu0_add_v12_detection(
            &builder->message,
            event->class_id,
            slot,
            event->confidence_q15,
            aux->roi_decision,
            aux->period_bonus_q12,
            event->flags,
            tile->capture_end_time_us);
        if (result < 0) {
            builder->message.invalid_reason_flags |=
                RF_V13_INVALID_RESULT_TRUNCATED;
        } else if (result > 0 && aux->quality_tier >= 2u &&
                   builder->message.evidence_count == evidence_before + 1u) {
            builder->message.evidence[evidence_before].evidence_flags |=
                RF_V18_EVIDENCE_STRONG_TEXTURE;
        }
    }
    return builder->message.invalid_reason_flags == RF_V13_INVALID_NONE
               ? RF_V18_ROUND_TILE_ACCEPTED
               : RF_V18_ROUND_TILE_ACCEPTED_INVALID;
}

int rf_v18_round_builder_finish(
    rf_v18_round_builder_t *builder,
    rf_v13_cpu0_round_message_t *output)
{
    if (builder == NULL || output == NULL || builder->active == 0u) {
        return 0;
    }
    if (builder->seen_slot_mask == RF_V13_CENTER_SLOT_MASK &&
        builder->tile_count == RF_V12_CENTER_COUNT) {
        builder->message.round_flags |= RF_V13_ROUND_COMPLETE;
    } else {
        builder->message.invalid_reason_flags |= RF_V13_INVALID_INCOMPLETE;
    }
    if (builder->header_valid_slot_mask == RF_V13_CENTER_SLOT_MASK &&
        builder->tile_count == RF_V12_CENTER_COUNT) {
        builder->message.round_flags |= RF_V13_ROUND_V12_TILES_VALIDATED;
    }
    if ((builder->message.round_flags & RF_V13_ROUND_EVIDENCE_TRUNCATED) !=
        0u) {
        builder->message.invalid_reason_flags |=
            RF_V13_INVALID_RESULT_TRUNCATED;
    }
    memcpy(output, &builder->message, sizeof(*output));
    builder->active = 0u;
    return 1;
}
