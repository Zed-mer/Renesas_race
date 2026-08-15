#include "ipc_bridge.h"

#include "hal_data.h"
#include <stdbool.h>
#include <string.h>
#include "display_stream.h"
#include "display_tile.h"
#include "ipc_mailbox.h"
#include "analysis_contract.h"
#include "activity_mailbox.h"
#include "latency_telemetry.h"
#include "cpu0_trace.h"

static ra8p1_system_telemetry_t g_telemetry_shadow;
static uint32_t g_publish_sequence;
static uint32_t g_command_sequence;
static uint32_t g_command_logical_sequence;
static uint32_t g_cpu0_boot_epoch;
static uint32_t g_cpu1_boot_epoch;
static uint32_t g_display_sequence;
static uint32_t g_tile_sequence;
static uint32_t g_display_session_id;
static uint32_t g_activity_sequence;

#define CPU0_LATENCY_MAX_WINDOWS (RA8P1_ANALYSIS_TILE_COUNT)

typedef struct st_cpu0_latency_window_state
{
    uint32_t first_packet_cycles;
    uint8_t first_packet_valid;
} cpu0_latency_window_state_t;

static struct
{
    uint32_t session_id;
    uint32_t window_count;
    uint32_t stride_samples;
    uint32_t next_ingress_window;
    uint64_t next_ingress_start_sample;
    uint32_t record_sequence;
    uint32_t pending_cpu1_acks;
    uint32_t active;
    cpu0_latency_window_state_t windows[CPU0_LATENCY_MAX_WINDOWS];
    ra8p1_latency_control_t control;
} g_cpu0_latency;

static void ipc_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static uint32_t ipc_next_epoch(uint32_t epoch)
{
    epoch++;
    return (epoch == 0U) ? 1U : epoch;
}

static void ipc_cpu0_handshake_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)&RA8P1_IPC_HANDSHAKE->cpu0,
                            (int32_t)sizeof(RA8P1_IPC_HANDSHAKE->cpu0));
#endif
    __DSB();
}

static void ipc_cpu0_activity_state_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr(
        (volatile void *)&RA8P1_ACTIVITY_CONTROL->cpu0,
        (int32_t)sizeof(RA8P1_ACTIVITY_CONTROL->cpu0));
#endif
    __DSB();
}

static bool ipc_cpu0_activity_cpu1_state_read(
    ra8p1_activity_cpu1_state_t *state)
{
    volatile ra8p1_activity_cpu1_state_t *source =
        &RA8P1_ACTIVITY_CONTROL->cpu1;
    if (state == NULL)
    {
        return false;
    }
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)source,
                                 (int32_t)sizeof(*source));
#endif
    ipc_barrier();
    memcpy(state, (const void *)source, sizeof(*state));
    ipc_barrier();
    return (state->magic == RA8P1_ACTIVITY_CONTROL_MAGIC) &&
           (state->version == RA8P1_ACTIVITY_CONTROL_VERSION) &&
           (state->size == sizeof(*state)) &&
           (state->boot_epoch != 0U) &&
           ((state->flags & RA8P1_ACTIVITY_CPU1_FLAG_ONLINE) != 0U);
}

static void ipc_cpu0_activity_init(void)
{
    volatile ra8p1_activity_cpu0_state_t *state =
        &RA8P1_ACTIVITY_CONTROL->cpu0;

    g_activity_sequence = 0U;
    memset((void *)RA8P1_ACTIVITY_MESSAGE, 0,
           sizeof(*RA8P1_ACTIVITY_MESSAGE));
    state->flags = 0U;
    ipc_barrier();
    state->magic = RA8P1_ACTIVITY_CONTROL_MAGIC;
    state->version = RA8P1_ACTIVITY_CONTROL_VERSION;
    state->size = (uint16_t)sizeof(*state);
    state->boot_epoch = g_cpu0_boot_epoch;
    state->begin_sequence = 0U;
    state->end_sequence = 0U;
    state->message_sequence = 0U;
    state->publish_drops = 0U;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)RA8P1_ACTIVITY_MESSAGE,
                            (int32_t)sizeof(*RA8P1_ACTIVITY_MESSAGE));
#endif
    state->flags = RA8P1_ACTIVITY_CPU0_FLAG_READY;
    ipc_barrier();
    ipc_cpu0_activity_state_clean();
}

static void ipc_cpu0_handshake_begin(void)
{
    volatile ra8p1_ipc_cpu0_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu0;
    ra8p1_ipc_cpu0_state_t previous;

#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)RA8P1_IPC_HANDSHAKE,
                                 (int32_t)sizeof(*RA8P1_IPC_HANDSHAKE));
#endif
    ipc_barrier();
    memcpy(&previous, (const void *)state, sizeof(previous));
    if ((previous.magic != RA8P1_IPC_HANDSHAKE_MAGIC) ||
        (previous.version != RA8P1_IPC_HANDSHAKE_VERSION) ||
        (previous.size != sizeof(previous)))
    {
        previous.boot_epoch = 0U;
    }

    g_cpu0_boot_epoch = ipc_next_epoch(previous.boot_epoch);
    state->flags = 0U;
    ipc_barrier();
    state->magic = RA8P1_IPC_HANDSHAKE_MAGIC;
    state->version = RA8P1_IPC_HANDSHAKE_VERSION;
    state->size = (uint16_t)sizeof(*state);
    state->boot_epoch = g_cpu0_boot_epoch;
    state->ready_sequence = 0U;
    state->acknowledged_cpu1_epoch = 0U;
    state->acknowledged_command_sequence = 0U;
    state->acknowledged_mailbox_sequence = 0U;
    ipc_barrier();
    ipc_cpu0_handshake_clean();
}

static void ipc_cpu0_handshake_ready(void)
{
    volatile ra8p1_ipc_cpu0_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu0;
    state->ready_sequence = g_cpu0_boot_epoch;
    ipc_barrier();
    state->flags = RA8P1_IPC_CPU0_FLAG_READY;
    ipc_barrier();
    ipc_cpu0_handshake_clean();
}

static bool ipc_cpu0_cpu1_state_read(ra8p1_ipc_cpu1_state_t *state)
{
    volatile ra8p1_ipc_cpu1_state_t *source = &RA8P1_IPC_HANDSHAKE->cpu1;
    if (state == NULL)
    {
        return false;
    }
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)source,
                                 (int32_t)sizeof(*source));
#endif
    ipc_barrier();
    memcpy(state, (const void *)source, sizeof(*state));
    ipc_barrier();
    return (state->magic == RA8P1_IPC_HANDSHAKE_MAGIC) &&
           (state->version == RA8P1_IPC_HANDSHAKE_VERSION) &&
           (state->size == sizeof(*state)) &&
           (state->boot_epoch != 0U) &&
           ((state->flags & RA8P1_IPC_CPU1_FLAG_ONLINE) != 0U);
}

static void ipc_cpu0_command_ack(uint32_t cpu1_epoch,
                                 uint32_t command_sequence,
                                 uint32_t mailbox_sequence)
{
    volatile ra8p1_ipc_cpu0_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu0;
    state->acknowledged_cpu1_epoch = cpu1_epoch;
    state->acknowledged_command_sequence = command_sequence;
    state->acknowledged_mailbox_sequence = mailbox_sequence;
    ipc_barrier();
    ipc_cpu0_handshake_clean();
}

static bool ipc_cpu0_dwt_enabled(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

static bool ipc_cpu0_dwt_enable(void)
{
    if (ipc_cpu0_dwt_enabled())
    {
        return true;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0UL) = 0xC5ACCE55UL;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    return ipc_cpu0_dwt_enabled();
}

bool ipc_bridge_cpu0_latency_cycle_now(uint32_t *cycles)
{
    if ((cycles == NULL) || !ipc_cpu0_dwt_enable())
    {
        return false;
    }
    __DSB();
    *cycles = DWT->CYCCNT;
    __ISB();
    return true;
}

static uint32_t ipc_cpu0_latency_next_sequence(void)
{
    uint32_t sequence = (g_cpu0_latency.record_sequence + 2U) & ~1U;
    if (sequence == 0U)
    {
        sequence = 2U;
    }
    g_cpu0_latency.record_sequence = sequence;
    return sequence;
}

static void ipc_cpu0_latency_clean_control(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) RA8P1_LATENCY_CONTROL,
                            (int32_t) sizeof(ra8p1_latency_control_t));
#endif
}

static void ipc_cpu0_latency_clean_record(volatile ra8p1_latency_record_t *record)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) record,
                            (int32_t) sizeof(ra8p1_latency_record_t));
#else
    (void) record;
#endif
}

static void ipc_cpu0_latency_invalidate_record(volatile ra8p1_latency_record_t *record)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *) record,
                                  (int32_t) sizeof(ra8p1_latency_record_t));
#else
    (void) record;
#endif
}

static bool ipc_cpu0_latency_record_read(uint32_t index,
                                         ra8p1_latency_record_t *record)
{
    volatile ra8p1_latency_record_t *source;
    uint32_t begin;
    uint32_t end;
    uint32_t final_begin;

    if ((record == NULL) || (index >= RA8P1_LATENCY_SLOT_COUNT))
    {
        return false;
    }
    source = &RA8P1_LATENCY_RECORDS[index];
    ipc_cpu0_latency_invalidate_record(source);
    ipc_barrier();
    begin = source->begin_sequence;
    if ((begin == 0U) || ((begin & 1U) != 0U))
    {
        return false;
    }
    memcpy(record, (const void *) source, sizeof(*record));
    ipc_barrier();
    end = source->end_sequence;
    final_begin = source->begin_sequence;
    if ((begin != end) || (begin != final_begin) ||
        (record->begin_sequence != begin) || (record->end_sequence != end) ||
        (record->session_id == 0U) ||
        ((record->window_index_flags & RA8P1_LATENCY_WINDOW_INDEX_MASK) >=
         CPU0_LATENCY_MAX_WINDOWS))
    {
        return false;
    }
    return true;
}

static void ipc_cpu0_latency_record_write(uint32_t index,
                                          const ra8p1_latency_record_t *payload)
{
    volatile ra8p1_latency_record_t *destination;
    uint32_t sequence;

    if ((payload == NULL) || (index >= RA8P1_LATENCY_SLOT_COUNT))
    {
        return;
    }
    destination = &RA8P1_LATENCY_RECORDS[index];
    sequence = ipc_cpu0_latency_next_sequence();
    destination->begin_sequence = sequence | 1U;
    ipc_barrier();
    destination->session_id = payload->session_id;
    destination->window_index_flags = payload->window_index_flags;
    destination->first_packet_cpu0_cycles = payload->first_packet_cpu0_cycles;
    destination->window_complete_cpu0_cycles = payload->window_complete_cpu0_cycles;
    destination->npu_publish_cpu0_cycles = payload->npu_publish_cpu0_cycles;
    destination->cpu1_visible_cpu0_cycles = payload->cpu1_visible_cpu0_cycles;
    ipc_barrier();
    destination->end_sequence = sequence;
    destination->begin_sequence = sequence;
    ipc_barrier();
    ipc_cpu0_latency_clean_record(destination);
}

static void ipc_cpu0_latency_control_write(void)
{
    volatile ra8p1_latency_control_t *destination = RA8P1_LATENCY_CONTROL;
    destination->magic = g_cpu0_latency.control.magic;
    destination->version = g_cpu0_latency.control.version;
    destination->size = g_cpu0_latency.control.size;
    destination->cpu0_cycle_hz = g_cpu0_latency.control.cpu0_cycle_hz;
    destination->published_windows = g_cpu0_latency.control.published_windows;
    destination->overwritten_windows = g_cpu0_latency.control.overwritten_windows;
    destination->overwritten_unacked_windows = g_cpu0_latency.control.overwritten_unacked_windows;
    destination->cpu1_visible_windows = g_cpu0_latency.control.cpu1_visible_windows;
    destination->latest_sequence = g_cpu0_latency.control.latest_sequence;
    ipc_barrier();
    ipc_cpu0_latency_clean_control();
}

static uint32_t ipc_cpu0_latency_window_count(uint64_t total_samples,
                                              uint32_t window_samples,
                                              uint32_t stride_samples)
{
    uint64_t count;
    if ((window_samples == 0U) || (stride_samples == 0U) ||
        (total_samples < window_samples))
    {
        return 0U;
    }
    count = 1U + ((total_samples - window_samples) / stride_samples);
    return (count > CPU0_LATENCY_MAX_WINDOWS) ? CPU0_LATENCY_MAX_WINDOWS :
           (uint32_t) count;
}

void ipc_bridge_cpu0_latency_session_begin(uint32_t session_id,
                                           uint64_t total_samples,
                                           uint32_t window_samples,
                                           uint32_t stride_samples)
{
    volatile ra8p1_latency_control_t *control = RA8P1_LATENCY_CONTROL;
    (void) ipc_cpu0_dwt_enable();

    if (session_id == 0U)
    {
        memset(&g_cpu0_latency, 0, sizeof(g_cpu0_latency));
        g_cpu0_latency.control.magic = RA8P1_LATENCY_MAGIC;
        g_cpu0_latency.control.version = RA8P1_LATENCY_VERSION;
        g_cpu0_latency.control.size = RA8P1_LATENCY_CONTROL_BYTES;
        g_cpu0_latency.control.cpu0_cycle_hz = RA8P1_LATENCY_CPU0_CYCLE_HZ;
        ipc_barrier();
        memset((void *) control, 0, RA8P1_IPC_LATENCY_BYTES);
        ipc_barrier();
        ipc_cpu0_latency_control_write();
#if (__DCACHE_PRESENT == 1U)
        SCB_CleanDCache_by_Addr((volatile void *) control,
                                (int32_t) RA8P1_IPC_LATENCY_BYTES);
#endif
        return;
    }

    /* Preserve the cross-session record sequence, pending CPU1 ACKs, and
     * cumulative diagnostics. Only the ingress state belongs to this window. */
    ipc_bridge_cpu0_latency_poll();
    g_cpu0_latency.session_id = session_id;
    g_cpu0_latency.stride_samples = stride_samples;
    g_cpu0_latency.window_count = ipc_cpu0_latency_window_count(total_samples,
                                                                 window_samples,
                                                                 stride_samples);
    g_cpu0_latency.next_ingress_window = 0U;
    g_cpu0_latency.next_ingress_start_sample = 0U;
    g_cpu0_latency.active = (g_cpu0_latency.window_count != 0U);
    memset(g_cpu0_latency.windows, 0, sizeof(g_cpu0_latency.windows));
}

uint32_t ipc_bridge_cpu0_latency_ingress_prepare(uint32_t session_id,
                                                 uint64_t sample_index,
                                                 uint32_t complex_samples,
                                                 uint32_t *ingress_cycles)
{
    uint64_t end_sample;
    uint32_t window_mask = 0U;
    uint32_t index;

    if ((g_cpu0_latency.active == 0U) || (session_id != g_cpu0_latency.session_id) ||
        (complex_samples == 0U) || (ingress_cycles == NULL) ||
        (sample_index > (UINT64_MAX - (uint64_t) complex_samples)))
    {
        return 0U;
    }
    end_sample = sample_index + (uint64_t) complex_samples;
    while ((g_cpu0_latency.next_ingress_window < g_cpu0_latency.window_count) &&
           (g_cpu0_latency.next_ingress_start_sample < sample_index))
    {
        g_cpu0_latency.next_ingress_window++;
        g_cpu0_latency.next_ingress_start_sample += g_cpu0_latency.stride_samples;
    }
    while ((g_cpu0_latency.next_ingress_window < g_cpu0_latency.window_count) &&
           (g_cpu0_latency.next_ingress_start_sample < end_sample))
    {
        index = g_cpu0_latency.next_ingress_window;
        if (g_cpu0_latency.windows[index].first_packet_valid == 0U)
        {
            window_mask |= 1UL << index;
        }
        g_cpu0_latency.next_ingress_window++;
        g_cpu0_latency.next_ingress_start_sample += g_cpu0_latency.stride_samples;
    }
    if ((window_mask == 0U) ||
        !ipc_bridge_cpu0_latency_cycle_now(ingress_cycles))
    {
        return 0U;
    }
    return window_mask;
}

void ipc_bridge_cpu0_latency_ingress_commit(uint32_t session_id,
                                            uint32_t window_mask,
                                            uint32_t ingress_cycles)
{
    uint32_t index;
    if ((g_cpu0_latency.active == 0U) ||
        (session_id != g_cpu0_latency.session_id) || (window_mask == 0U))
    {
        return;
    }
    for (index = 0U; index < g_cpu0_latency.window_count; ++index)
    {
        if (((window_mask & (1UL << index)) != 0U) &&
            (g_cpu0_latency.windows[index].first_packet_valid == 0U))
        {
            g_cpu0_latency.windows[index].first_packet_cycles = ingress_cycles;
            ipc_barrier();
            g_cpu0_latency.windows[index].first_packet_valid = 1U;
        }
    }
}

void ipc_bridge_cpu0_latency_window_complete(uint32_t session_id,
                                             uint32_t window_index,
                                             uint32_t complete_cycles,
                                             bool timing_valid)
{
    ra8p1_latency_record_t old;
    ra8p1_latency_record_t record;
    uint32_t slot_index;
    uint32_t flags;

    if ((g_cpu0_latency.active == 0U) ||
        (session_id != g_cpu0_latency.session_id) ||
        (window_index >= g_cpu0_latency.window_count))
    {
        return;
    }
    /* Convert acknowledgements that arrived while this window was being
     * processed before deciding whether the four-slot ring will overwrite. */
    ipc_bridge_cpu0_latency_poll();
    slot_index = g_cpu0_latency.control.published_windows &
                 (RA8P1_LATENCY_SLOT_COUNT - 1U);
    if (ipc_cpu0_latency_record_read(slot_index, &old))
    {
        g_cpu0_latency.control.overwritten_windows++;
        if ((old.window_index_flags & RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID) == 0U)
        {
            /* A pending token proves CPU1 copied the frame even if CPU0 has
             * not had a chance to turn it into a timestamp yet. */
            if (old.cpu1_visible_cpu0_cycles == old.begin_sequence)
            {
                g_cpu0_latency.control.cpu1_visible_windows++;
            }
            else
            {
                g_cpu0_latency.control.overwritten_unacked_windows++;
            }
            if (g_cpu0_latency.pending_cpu1_acks != 0U)
            {
                g_cpu0_latency.pending_cpu1_acks--;
            }
        }
    }
    memset(&record, 0, sizeof(record));
    record.session_id = session_id;
    flags = window_index & RA8P1_LATENCY_WINDOW_INDEX_MASK;
    if (g_cpu0_latency.windows[window_index].first_packet_valid != 0U)
    {
        flags |= RA8P1_LATENCY_FLAG_FIRST_PACKET_VALID;
        record.first_packet_cpu0_cycles =
            g_cpu0_latency.windows[window_index].first_packet_cycles;
    }
    record.window_index_flags = flags;
    if (timing_valid)
    {
        record.window_index_flags |= RA8P1_LATENCY_FLAG_WINDOW_COMPLETE_VALID;
        record.window_complete_cpu0_cycles = complete_cycles;
    }
    ipc_cpu0_latency_record_write(slot_index, &record);
    g_cpu0_latency.control.published_windows++;
    g_cpu0_latency.control.latest_sequence = g_cpu0_latency.record_sequence;
    ipc_cpu0_latency_control_write();
}

static bool ipc_cpu0_latency_update_npu(uint32_t source_session_id,
                                        uint32_t published_session_id,
                                        uint32_t window_index,
                                        uint32_t publish_cycles,
                                        bool timing_valid)
{
    ra8p1_latency_record_t record;
    uint32_t slot_index;
    if (g_cpu0_latency.active == 0U)
    {
        return false;
    }
    for (slot_index = 0U; slot_index < RA8P1_LATENCY_SLOT_COUNT; ++slot_index)
    {
        if (!ipc_cpu0_latency_record_read(slot_index, &record) ||
            (record.session_id != source_session_id) ||
            ((record.window_index_flags & RA8P1_LATENCY_WINDOW_INDEX_MASK) != window_index))
        {
            continue;
        }
        record.session_id = published_session_id;
        if (timing_valid)
        {
            record.npu_publish_cpu0_cycles = publish_cycles;
            record.window_index_flags |= RA8P1_LATENCY_FLAG_NPU_PUBLISH_VALID;
        }
        ipc_cpu0_latency_record_write(slot_index, &record);
        g_cpu0_latency.pending_cpu1_acks++;
        g_cpu0_latency.control.latest_sequence = g_cpu0_latency.record_sequence;
        ipc_cpu0_latency_control_write();
        return true;
    }
    return false;
}

void ipc_bridge_cpu0_latency_poll(void)
{
    ra8p1_latency_record_t record;
    uint32_t visible_cycles;
    uint32_t slot_index;
    uint32_t publish_sequence;
    if (g_cpu0_latency.pending_cpu1_acks == 0U)
    {
        return;
    }
    for (slot_index = 0U; slot_index < RA8P1_LATENCY_SLOT_COUNT; ++slot_index)
    {
        if (!ipc_cpu0_latency_record_read(slot_index, &record) ||
            ((record.window_index_flags & RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID) != 0U))
        {
            continue;
        }
        if (record.cpu1_visible_cpu0_cycles != record.begin_sequence)
        {
            continue;
        }
        if (!ipc_bridge_cpu0_latency_cycle_now(&visible_cycles))
        {
            continue;
        }
        publish_sequence = record.begin_sequence;
        record.cpu1_visible_cpu0_cycles = visible_cycles;
        record.window_index_flags |= RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID;
        ipc_cpu0_latency_record_write(slot_index, &record);
        cpu0_trace_cpu1_visible(record.session_id,
                                record.window_index_flags &
                                    RA8P1_LATENCY_WINDOW_INDEX_MASK,
                                visible_cycles);
        if (g_cpu0_latency.pending_cpu1_acks != 0U)
        {
            g_cpu0_latency.pending_cpu1_acks--;
        }
        /* A concurrent producer cannot change this slot while this function
         * runs, but keep the sequence read explicit for host diagnostics. */
        if (publish_sequence != 0U)
        {
            g_cpu0_latency.control.cpu1_visible_windows++;
            g_cpu0_latency.control.latest_sequence = g_cpu0_latency.record_sequence;
            ipc_cpu0_latency_control_write();
        }
    }
}

bool ipc_bridge_cpu0_latency_result_visible(uint32_t session_id,
                                            uint32_t window_index)
{
    ra8p1_latency_record_t record;
    uint32_t slot_index;
    if ((session_id == 0U) ||
        (window_index >= CPU0_LATENCY_MAX_WINDOWS))
    {
        return false;
    }
    ipc_bridge_cpu0_latency_poll();
    for (slot_index = 0U; slot_index < RA8P1_LATENCY_SLOT_COUNT; ++slot_index)
    {
        if (ipc_cpu0_latency_record_read(slot_index, &record) &&
            (record.session_id == session_id) &&
            ((record.window_index_flags & RA8P1_LATENCY_WINDOW_INDEX_MASK) ==
             window_index) &&
            ((record.window_index_flags &
              RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID) != 0U))
        {
            return true;
        }
    }
    return false;
}

void ipc_bridge_cpu0_init(void)
{
    volatile ra8p1_display_stream_control_t *display_control = RA8P1_DISPLAY_STREAM_CONTROL;
    uint32_t previous_session;

#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *) display_control,
                                (int32_t) sizeof(*display_control));
#endif
    previous_session = display_control->session_id;

    ipc_cpu0_handshake_begin();
    ipc_cpu0_activity_init();
    memset(&g_telemetry_shadow, 0, sizeof(g_telemetry_shadow));
    g_telemetry_shadow.magic = RA8P1_SYSTEM_PROTOCOL_MAGIC;
    g_telemetry_shadow.version = RA8P1_SYSTEM_PROTOCOL_VERSION;
    g_telemetry_shadow.size = (uint16_t) sizeof(g_telemetry_shadow);
    g_publish_sequence = 0U;
    /* A running CPU1 can publish its boot request while CPU0 is still opening
     * the NPU and analysis pipeline.  Start with no consumed sequence so the
     * latest valid mailbox command is replayed after a CPU0 warm reset too. */
    g_command_sequence = 0U;
    g_command_logical_sequence = 0U;
    g_cpu1_boot_epoch = 0U;
    g_display_sequence = 0U;
    g_tile_sequence = 0U;
    ipc_bridge_cpu0_latency_session_begin(0U, 0U, 0U, 0U);
    g_display_session_id = previous_session + 1U;
    if (g_display_session_id == 0U)
    {
        g_display_session_id = 1U;
    }

    display_control->session_id = 0U;
    ipc_barrier();
    memset((void *) RA8P1_DISPLAY_STREAM_SLOTS,
           0,
           RA8P1_DISPLAY_STREAM_SLOT_COUNT * sizeof(ra8p1_display_stream_slot_t));
    memset((void *) RA8P1_DISPLAY_TILE_SLOTS, 0, RA8P1_DISPLAY_TILE_BYTES);
    display_control->magic = RA8P1_DISPLAY_STREAM_MAGIC;
    display_control->version = RA8P1_DISPLAY_STREAM_VERSION;
    display_control->size = (uint16_t) sizeof(*display_control);
    ipc_barrier();
    display_control->session_id = g_display_session_id;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) display_control,
                           (int32_t) RA8P1_DISPLAY_STREAM_BYTES);
    SCB_CleanDCache_by_Addr((volatile void *) RA8P1_DISPLAY_TILE_SLOTS,
                           (int32_t) RA8P1_DISPLAY_TILE_BYTES);
#endif
    ipc_cpu0_handshake_ready();
}

bool ipc_bridge_cpu0_activity_publish(
    const rf_v13_cpu0_round_message_t *message)
{
    volatile ra8p1_activity_cpu0_state_t *state =
        &RA8P1_ACTIVITY_CONTROL->cpu0;
    ra8p1_activity_cpu1_state_t cpu1_state;
    uint32_t sequence;

    if ((message == NULL) ||
        (message->magic != RF_V13_ACTIVITY_MAGIC) ||
        (message->abi_major != RF_V13_ACTIVITY_ABI_MAJOR) ||
        (message->abi_minor != RF_V13_ACTIVITY_ABI_MINOR) ||
        (message->message_bytes != RF_V13_CPU0_MESSAGE_BYTES) ||
        (message->message_sequence == 0U) ||
        !ipc_cpu0_activity_cpu1_state_read(&cpu1_state) ||
        (cpu1_state.observed_cpu0_epoch != g_cpu0_boot_epoch))
    {
        return false;
    }

    if ((state->message_sequence != 0U) &&
        (cpu1_state.acknowledged_message_sequence !=
         state->message_sequence))
    {
        state->publish_drops++;
        ipc_cpu0_activity_state_clean();
        return false;
    }

    sequence = (g_activity_sequence + 2U) & ~1U;
    if (sequence == 0U)
    {
        sequence = 2U;
    }

    state->begin_sequence = sequence | 1U;
    ipc_barrier();
    ipc_cpu0_activity_state_clean();
    memcpy((void *)RA8P1_ACTIVITY_MESSAGE, message, sizeof(*message));
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)RA8P1_ACTIVITY_MESSAGE,
                            (int32_t)sizeof(*RA8P1_ACTIVITY_MESSAGE));
#endif
    __DSB();
    state->end_sequence = sequence;
    state->message_sequence = message->message_sequence;
    ipc_barrier();
    state->begin_sequence = sequence;
    ipc_barrier();
    ipc_cpu0_activity_state_clean();
    g_activity_sequence = sequence;
    return true;
}

void ipc_bridge_cpu0_publish(const ra8p1_system_telemetry_t *telemetry)
{
    if (telemetry != NULL)
    {
        volatile ra8p1_telemetry_mailbox_t *mailbox = RA8P1_TELEMETRY_MAILBOX;
        uint32_t sequence = (g_publish_sequence + 2U) & ~1U;
        g_telemetry_shadow = *telemetry;
        mailbox->begin_sequence = sequence | 1U;
        ipc_barrier();
        memcpy((void *) &mailbox->payload, telemetry, sizeof(*telemetry));
        ipc_barrier();
        mailbox->end_sequence = sequence;
        mailbox->begin_sequence = sequence;
        ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
        SCB_CleanDCache_by_Addr((volatile void *) mailbox, (int32_t) sizeof(*mailbox));
#endif
        g_publish_sequence = sequence;
    }
}

void ipc_bridge_cpu0_display_session_set(uint32_t session_id)
{
    volatile ra8p1_display_stream_control_t *display_control = RA8P1_DISPLAY_STREAM_CONTROL;
    uint32_t next_session = (session_id == 0U) ? 1U : session_id;
    bool session_changed = (next_session != g_display_session_id);

    /* Frame sequence and slots span SDR sessions so CPU1 can consume the
     * previous result after the next capture has already received credit.
     * Tile rows remain session-scoped and are restarted at each handover. */
    g_display_session_id = next_session;
    if (session_changed)
    {
        g_tile_sequence = 0U;
    }
    display_control->session_id = 0U;
    ipc_barrier();
    memset((void *) RA8P1_DISPLAY_TILE_SLOTS, 0, RA8P1_DISPLAY_TILE_BYTES);
    ipc_barrier();
    display_control->session_id = g_display_session_id;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) display_control,
                           (int32_t) sizeof(*display_control));
    SCB_CleanDCache_by_Addr((volatile void *) RA8P1_DISPLAY_TILE_SLOTS,
                           (int32_t) RA8P1_DISPLAY_TILE_BYTES);
#endif
}

void ipc_bridge_cpu0_display_tile_publish(const uint8_t *display_tile,
                                          uint32_t window_sequence,
                                          uint32_t flags)
{
    ipc_bridge_cpu0_display_tile_publish_ex(display_tile,
                                            window_sequence,
                                            flags,
                                            0U,
                                            (uint8_t)(window_sequence & 0xFFU),
                                            0U,
                                            0U,
                                            RA8P1_DISPLAY_TILE_HEIGHT);
}

static void ipc_bridge_cpu0_display_tile_publish_row(
    const uint8_t *display_row,
    uint32_t window_sequence,
    uint32_t flags,
    uint32_t center_index,
    uint8_t tile_index,
    uint8_t tile_count,
    uint8_t novel_time_start)
{
    volatile ra8p1_display_tile_slot_t *tile_slot;
    ra8p1_display_tile_payload_t tile_payload;
    uint32_t sequence;
    uint32_t slot_index;

    if (display_row == NULL)
    {
        return;
    }

    if (g_tile_sequence == 0xFFFFFFFEUL)
    {
        uint32_t next_session = g_display_session_id + 1U;
        if (next_session == 0U)
        {
            next_session = 1U;
        }
        ipc_bridge_cpu0_display_session_set(next_session);
    }

    sequence = (g_tile_sequence + 2U) & ~1U;
    slot_index = ((sequence >> 1U) - 1U) &
                 (RA8P1_DISPLAY_TILE_SLOT_COUNT - 1U);
    tile_slot = &RA8P1_DISPLAY_TILE_SLOTS[slot_index];

    memset(&tile_payload, 0, sizeof(tile_payload));
    tile_payload.magic = RA8P1_DISPLAY_TILE_MAGIC;
    tile_payload.version = RA8P1_DISPLAY_TILE_VERSION;
    tile_payload.size = (uint16_t) sizeof(tile_payload);
    tile_payload.session_id = g_display_session_id;
    tile_payload.sequence = sequence;
    tile_payload.window_sequence = window_sequence;
    tile_payload.width_height = (RA8P1_DISPLAY_TILE_WIDTH << 16U) |
                                RA8P1_DISPLAY_TILE_HEIGHT;
    tile_payload.flags = flags;
    tile_payload.center_index = (uint8_t)center_index;
    tile_payload.tile_index = tile_index;
    tile_payload.tile_count = tile_count;
    tile_payload.novel_time_start = novel_time_start;
    tile_payload.novel_time_count = 1U;
    memcpy(tile_payload.levels, display_row, RA8P1_DISPLAY_TILE_ROW_BYTES);

    tile_slot->begin_sequence = sequence | 1U;
    ipc_barrier();
    memcpy((void *) &tile_slot->payload, &tile_payload, sizeof(tile_payload));
    ipc_barrier();
    tile_slot->end_sequence = sequence;
    tile_slot->begin_sequence = sequence;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) tile_slot, (int32_t) sizeof(*tile_slot));
#endif
    g_tile_sequence = sequence;
}

void ipc_bridge_cpu0_display_tile_publish_ex(const uint8_t *display_tile,
                                             uint32_t window_sequence,
                                             uint32_t flags,
                                             uint32_t center_index,
                                             uint8_t tile_index,
                                             uint8_t tile_count,
                                             uint8_t novel_time_start,
                                             uint8_t novel_time_count)
{
    uint32_t row_offset;

    if ((display_tile == NULL) || (novel_time_count == 0U) ||
        (novel_time_start >= RA8P1_DISPLAY_TILE_HEIGHT) ||
        (((uint32_t)novel_time_start + novel_time_count) >
         RA8P1_DISPLAY_TILE_HEIGHT))
    {
        return;
    }

    for (row_offset = 0U; row_offset < novel_time_count; ++row_offset)
    {
        const uint32_t row_index = novel_time_start + row_offset;
        ipc_bridge_cpu0_display_tile_publish_row(
            &display_tile[row_index * RA8P1_DISPLAY_TILE_WIDTH],
            window_sequence,
            flags,
            center_index,
            tile_index,
            tile_count,
            (uint8_t)row_index);
    }
}

void ipc_bridge_cpu0_display_publish(const ra8p1_display_frame_t *frame,
                                     const uint8_t *display_tile)
{
    volatile ra8p1_display_stream_slot_t *slot;
    ra8p1_display_frame_t payload;
    uint32_t sequence;
    uint32_t slot_index;
    uint32_t publish_cycles = 0U;
    bool publish_timing_valid;

    if (frame == NULL)
    {
        return;
    }

    if (g_display_sequence == 0xFFFFFFFEUL)
    {
        uint32_t next_session = g_display_session_id + 1U;
        if (next_session == 0U)
        {
            next_session = 1U;
        }
        ipc_bridge_cpu0_display_session_set(next_session);
        g_display_sequence = 0U;
    }

    sequence = (g_display_sequence + 2U) & ~1U;
    slot_index = ((sequence >> 1U) - 1U) &
                 (RA8P1_DISPLAY_STREAM_SLOT_COUNT - 1U);
    slot = &RA8P1_DISPLAY_STREAM_SLOTS[slot_index];

    payload = *frame;
    payload.magic = RA8P1_DISPLAY_STREAM_MAGIC;
    payload.version = RA8P1_DISPLAY_STREAM_VERSION;
    payload.size = (uint16_t) sizeof(payload);
    payload.session_id = g_display_session_id;
    payload.sequence = sequence;

    /* Keep the legacy argument for callers that still publish one tile per
     * completed frame.  Partial-window producers use the independent API. */
    if (display_tile != NULL)
    {
        ipc_bridge_cpu0_display_tile_publish(display_tile,
                                              payload.analysis.window_sequence,
                                              payload.flags);
    }

    slot->begin_sequence = sequence | 1U;
    ipc_barrier();
    memcpy((void *) &slot->payload, &payload, sizeof(payload));
    ipc_barrier();
    /* Timestamp the result before the display seqlock becomes readable. */
    publish_timing_valid =
        ipc_bridge_cpu0_latency_cycle_now(&publish_cycles) &&
        ((frame->analysis.timing_flags & RA8P1_DISPLAY_TIMING_E2E_VALID) != 0U);
    (void) ipc_cpu0_latency_update_npu(frame->session_id,
                                       payload.session_id,
                                       payload.analysis.window_sequence,
                                       publish_cycles,
                                       publish_timing_valid);
    ipc_barrier();
    slot->end_sequence = sequence;
    slot->begin_sequence = sequence;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *) slot, (int32_t) sizeof(*slot));
#endif
    g_display_sequence = sequence;
}

bool ipc_bridge_cpu0_command_get(ra8p1_ui_command_t *command)
{
    volatile ra8p1_command_mailbox_t *mailbox = RA8P1_COMMAND_MAILBOX;
    ra8p1_ipc_cpu1_state_t cpu1_state;
    uint32_t begin;
    uint32_t end;
    uint32_t final_begin;
    ra8p1_ui_command_t candidate;

    if (command == NULL)
    {
        return false;
    }
    if (!ipc_cpu0_cpu1_state_read(&cpu1_state))
    {
        return false;
    }
    if (cpu1_state.boot_epoch != g_cpu1_boot_epoch)
    {
        g_cpu1_boot_epoch = cpu1_state.boot_epoch;
        g_command_sequence = 0U;
        g_command_logical_sequence = 0U;
    }
    if (((cpu1_state.flags & RA8P1_IPC_CPU1_FLAG_COMMAND_PENDING) == 0U) ||
        (cpu1_state.published_command_sequence == 0U) ||
        (cpu1_state.published_mailbox_sequence == 0U))
    {
        return false;
    }
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *) mailbox, (int32_t) sizeof(*mailbox));
#endif
    ipc_barrier();
    begin = mailbox->begin_sequence;
    if (((begin & 1U) != 0U) || (begin == 0U) ||
        (begin == g_command_sequence) ||
        (begin != cpu1_state.published_mailbox_sequence))
    {
        return false;
    }
    memcpy(&candidate, (const void *) &mailbox->payload, sizeof(candidate));
    ipc_barrier();
    end = mailbox->end_sequence;
    final_begin = mailbox->begin_sequence;
    if ((begin != end) || (begin != final_begin) ||
        (candidate.magic != RA8P1_SYSTEM_PROTOCOL_MAGIC) ||
        (candidate.version != RA8P1_SYSTEM_PROTOCOL_VERSION) ||
        (candidate.size != sizeof(candidate)) ||
        (candidate.sequence == 0U) ||
        (candidate.sequence != cpu1_state.published_command_sequence))
    {
        return false;
    }
    g_command_sequence = begin;
    ipc_cpu0_command_ack(cpu1_state.boot_epoch, candidate.sequence, begin);
    if (candidate.sequence == g_command_logical_sequence)
    {
        return false;
    }
    *command = candidate;
    g_command_logical_sequence = candidate.sequence;
    return true;
}
