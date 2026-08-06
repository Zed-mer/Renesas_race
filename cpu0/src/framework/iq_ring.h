#ifndef IQ_RING_H
#define IQ_RING_H

#include <stdbool.h>
#include <stdint.h>
#include "resource_layout.h"

#define IQ_RING_SLOT_COUNT    (4096U)
#define IQ_RING_PAYLOAD_BYTES (1536U)

typedef struct st_iq_ring_view
{
    const uint8_t *data;
    uint32_t length;
    uint32_t sequence;
    uint32_t flags;
    uint64_t sample_index;
    uint32_t format;
    uint32_t session_id;
} iq_ring_view_t;

typedef struct st_iq_ring_stats
{
    uint32_t pushed;
    uint32_t popped;
    uint32_t full_drops;
    uint32_t oversize_drops;
    uint32_t high_watermark;
    uint32_t queued;
} iq_ring_stats_t;

void iq_ring_init(void);
bool iq_ring_push_copy(const uint8_t *data,
                       uint32_t length,
                       uint32_t sequence,
                       uint32_t flags,
                       uint64_t sample_index,
                       uint32_t format,
                       uint32_t session_id);
bool iq_ring_pop_begin(iq_ring_view_t *view);
void iq_ring_pop_end(void);
void iq_ring_stats_get(iq_ring_stats_t *stats);

typedef char iq_ring_slot_count_must_be_power_of_two[
    ((IQ_RING_SLOT_COUNT & (IQ_RING_SLOT_COUNT - 1U)) == 0U) ? 1 : -1];

#endif
