#include "iq_ring.h"

#include <string.h>

typedef struct st_iq_ring_slot
{
    uint32_t length;
    uint32_t sequence;
    uint32_t flags;
    uint32_t reserved;
    uint32_t format;
    uint32_t sample_index_low;
    uint32_t sample_index_high;
    uint32_t session_id;
    uint8_t payload[IQ_RING_PAYLOAD_BYTES];
} iq_ring_slot_t;

typedef struct st_iq_ring_control
{
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t pushed;
    volatile uint32_t popped;
    volatile uint32_t full_drops;
    volatile uint32_t oversize_drops;
    volatile uint32_t high_watermark;
} iq_ring_control_t;

static iq_ring_slot_t g_iq_ring[IQ_RING_SLOT_COUNT]
    __attribute__((section(".sdram_noinit"), aligned(32), used));
static iq_ring_control_t g_iq_ring_control
    __attribute__((section(".ram_nocache"), aligned(32)));

typedef char iq_ring_storage_size_must_match_layout[
    (sizeof(g_iq_ring) == RA8P1_CPU0_IQ_RING_BYTES) ? 1 : -1];

static void iq_ring_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

void iq_ring_init(void)
{
    memset((void *) &g_iq_ring_control, 0, sizeof(g_iq_ring_control));
    iq_ring_barrier();
}

bool iq_ring_push_copy(const uint8_t *data,
                       uint32_t length,
                       uint32_t sequence,
                       uint32_t flags,
                       uint64_t sample_index,
                       uint32_t format,
                       uint32_t session_id)
{
    uint32_t head;
    uint32_t tail;
    uint32_t queued;
    iq_ring_slot_t *slot;

    if ((data == NULL) || (length > IQ_RING_PAYLOAD_BYTES))
    {
        g_iq_ring_control.oversize_drops++;
        return false;
    }

    head = g_iq_ring_control.head;
    tail = g_iq_ring_control.tail;
    queued = head - tail;
    if (queued >= IQ_RING_SLOT_COUNT)
    {
        g_iq_ring_control.full_drops++;
        return false;
    }

    slot = &g_iq_ring[head & (IQ_RING_SLOT_COUNT - 1U)];
    slot->sequence = sequence;
    slot->flags = flags;
    slot->reserved = 0U;
    slot->format = format;
    slot->sample_index_low = (uint32_t) sample_index;
    slot->sample_index_high = (uint32_t) (sample_index >> 32U);
    slot->session_id = session_id;
    slot->length = length;
    memcpy(slot->payload, data, length);
    iq_ring_barrier();
    g_iq_ring_control.head = head + 1U;
    g_iq_ring_control.pushed++;
    queued++;
    if (queued > g_iq_ring_control.high_watermark)
    {
        g_iq_ring_control.high_watermark = queued;
    }
    return true;
}

bool iq_ring_pop_begin(iq_ring_view_t *view)
{
    uint32_t tail;
    iq_ring_slot_t *slot;

    if (view == NULL)
    {
        return false;
    }
    tail = g_iq_ring_control.tail;
    if (tail == g_iq_ring_control.head)
    {
        return false;
    }
    iq_ring_barrier();
    slot = &g_iq_ring[tail & (IQ_RING_SLOT_COUNT - 1U)];
    view->data = slot->payload;
    view->length = slot->length;
    view->sequence = slot->sequence;
    view->flags = slot->flags;
    view->sample_index = ((uint64_t) slot->sample_index_high << 32U) | slot->sample_index_low;
    view->format = slot->format;
    view->session_id = slot->session_id;
    return true;
}

void iq_ring_pop_end(void)
{
    iq_ring_barrier();
    g_iq_ring_control.tail++;
    g_iq_ring_control.popped++;
}

void iq_ring_stats_get(iq_ring_stats_t *stats)
{
    if (stats != NULL)
    {
        stats->pushed = g_iq_ring_control.pushed;
        stats->popped = g_iq_ring_control.popped;
        stats->full_drops = g_iq_ring_control.full_drops;
        stats->oversize_drops = g_iq_ring_control.oversize_drops;
        stats->high_watermark = g_iq_ring_control.high_watermark;
        stats->queued = g_iq_ring_control.head - g_iq_ring_control.tail;
    }
}
