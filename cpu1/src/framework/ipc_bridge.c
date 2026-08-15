#include "ipc_bridge.h"

#include "hal_data.h"
#include <string.h>
#include "display_stream.h"
#include "ipc_mailbox.h"
#include "latency_telemetry.h"
#include "activity_mailbox.h"

static uint32_t g_telemetry_sequence;
static uint32_t g_command_sequence;
static uint32_t g_cpu1_boot_epoch;
static uint32_t g_observed_cpu0_boot_epoch;
static uint32_t g_command_published_cpu0_epoch;
static uint32_t g_command_retry_ticks;
static uint32_t g_command_retry_count;
static bool g_cpu0_ready;
static bool g_command_pending;
static bool g_command_acked;
static bool g_command_has_published;
static bool g_have_last_command;
static ra8p1_ui_command_t g_last_command;
static uint32_t g_display_sequence;
static uint32_t g_display_session_id;
static uint32_t g_display_cpu0_boot_epoch;
static bool g_display_session_changed;
static uint32_t g_tile_sequence;
static uint32_t g_visible_pending_session;
static uint32_t g_visible_pending_sequence;
static uint32_t g_visible_pending_window;
static uint32_t g_activity_sequence;
static uint32_t g_activity_cpu0_epoch;
static bool g_panel_shutdown_ack;

#define CPU1_COMMAND_RETRY_SERVICE_CALLS (50U)

static void ipc_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static uint32_t ipc_next_epoch(uint32_t epoch)
{
    epoch++;
    return (epoch == 0U) ? 1U : epoch;
}

static void ipc_cpu1_handshake_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)&RA8P1_IPC_HANDSHAKE->cpu1,
                            (int32_t)sizeof(RA8P1_IPC_HANDSHAKE->cpu1));
#endif
    __DSB();
}

static void ipc_cpu1_activity_state_clean(void)
{
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr(
        (volatile void *)&RA8P1_ACTIVITY_CONTROL->cpu1,
        (int32_t)sizeof(RA8P1_ACTIVITY_CONTROL->cpu1));
#endif
    __DSB();
}

static void ipc_cpu1_activity_state_publish(void)
{
    volatile ra8p1_activity_cpu1_state_t *state =
        &RA8P1_ACTIVITY_CONTROL->cpu1;
    state->magic = RA8P1_ACTIVITY_CONTROL_MAGIC;
    state->version = RA8P1_ACTIVITY_CONTROL_VERSION;
    state->size = (uint16_t)sizeof(*state);
    state->boot_epoch = g_cpu1_boot_epoch;
    ipc_barrier();
    uint32_t flags = RA8P1_ACTIVITY_CPU1_FLAG_ONLINE;
    if (g_panel_shutdown_ack)
    {
        flags |= RA8P1_ACTIVITY_CPU1_FLAG_PANEL_SHUTDOWN_ACK;
    }
    state->flags = flags;
    ipc_barrier();
    ipc_cpu1_activity_state_clean();
}

static void ipc_cpu1_state_publish(void)
{
    volatile ra8p1_ipc_cpu1_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu1;
    uint32_t flags = RA8P1_IPC_CPU1_FLAG_ONLINE;
    if (g_command_pending)
    {
        flags |= RA8P1_IPC_CPU1_FLAG_COMMAND_PENDING;
    }
    if (g_command_acked)
    {
        flags |= RA8P1_IPC_CPU1_FLAG_COMMAND_ACKED;
    }
    state->magic = RA8P1_IPC_HANDSHAKE_MAGIC;
    state->version = RA8P1_IPC_HANDSHAKE_VERSION;
    state->size = (uint16_t)sizeof(*state);
    state->boot_epoch = g_cpu1_boot_epoch;
    state->observed_cpu0_epoch = g_observed_cpu0_boot_epoch;
    state->command_retry_count = g_command_retry_count;
    ipc_barrier();
    state->flags = flags;
    ipc_barrier();
    ipc_cpu1_handshake_clean();
}

static bool ipc_cpu1_cpu0_state_read(ra8p1_ipc_cpu0_state_t *state)
{
    volatile ra8p1_ipc_cpu0_state_t *source = &RA8P1_IPC_HANDSHAKE->cpu0;
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
           (state->ready_sequence == state->boot_epoch) &&
           ((state->flags & RA8P1_IPC_CPU0_FLAG_READY) != 0U);
}

static void ipc_cpu1_command_publish(void)
{
    volatile ra8p1_command_mailbox_t *mailbox = RA8P1_COMMAND_MAILBOX;
    volatile ra8p1_ipc_cpu1_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu1;
    uint32_t sequence = (g_command_sequence + 2U) & ~1U;
    if (sequence == 0U)
    {
        sequence = 2U;
    }
    if (g_command_has_published)
    {
        g_command_retry_count++;
    }
    mailbox->begin_sequence = sequence | 1U;
    ipc_barrier();
    memcpy((void *)&mailbox->payload, &g_last_command, sizeof(g_last_command));
    ipc_barrier();
    mailbox->end_sequence = sequence;
    mailbox->begin_sequence = sequence;
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((volatile void *)mailbox,
                            (int32_t)sizeof(*mailbox));
#endif
    __DSB();
    g_command_sequence = sequence;
    g_command_has_published = true;
    g_command_published_cpu0_epoch = g_observed_cpu0_boot_epoch;
    g_command_retry_ticks = 0U;
    state->published_command_sequence = g_last_command.sequence;
    state->published_mailbox_sequence = sequence;
    ipc_barrier();
    ipc_cpu1_state_publish();
}

static bool ipc_cpu1_latency_record_read(uint32_t index,
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
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *) source,
                                  (int32_t) sizeof(*source));
#endif
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
    return (begin == end) && (begin == final_begin) &&
           (record->begin_sequence == begin) &&
           (record->end_sequence == end) &&
           (record->session_id != 0U);
}

static void ipc_cpu1_latency_ack(uint32_t session_id, uint32_t window_index)
{
    ra8p1_latency_record_t record;
    uint32_t index;
    for (index = 0U; index < RA8P1_LATENCY_SLOT_COUNT; ++index)
    {
        volatile ra8p1_latency_record_t *destination = &RA8P1_LATENCY_RECORDS[index];
        if (!ipc_cpu1_latency_record_read(index, &record) ||
            (record.session_id != session_id) ||
            ((record.window_index_flags & RA8P1_LATENCY_WINDOW_INDEX_MASK) != window_index) ||
            ((record.window_index_flags &
              RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID) != 0U))
        {
            continue;
        }
        /* This aligned word is the only CPU1-owned field in the record. CPU0
         * accepts it only when it equals the still-current even seqlock. */
        destination->cpu1_visible_cpu0_cycles = record.begin_sequence;
        ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
        SCB_CleanDCache_by_Addr((volatile void *) destination,
                                (int32_t) sizeof(*destination));
#endif
        ipc_barrier();
        return;
    }
}

void ipc_bridge_cpu1_init(void)
{
    volatile ra8p1_ipc_cpu1_state_t *state = &RA8P1_IPC_HANDSHAKE->cpu1;
    ra8p1_ipc_cpu1_state_t previous;

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
    state->flags = 0U;
    ipc_barrier();
    ipc_cpu1_handshake_clean();

    g_telemetry_sequence = 0U;
    g_command_sequence = 0U;
    g_cpu1_boot_epoch = ipc_next_epoch(previous.boot_epoch);
    g_observed_cpu0_boot_epoch = 0U;
    g_command_published_cpu0_epoch = 0U;
    g_command_retry_ticks = 0U;
    g_command_retry_count = 0U;
    g_cpu0_ready = false;
    g_command_pending = false;
    g_command_acked = false;
    g_command_has_published = false;
    g_have_last_command = false;
    memset(&g_last_command, 0, sizeof(g_last_command));
    g_display_sequence = 0U;
    g_display_session_id = 0U;
    g_display_cpu0_boot_epoch = 0U;
    g_display_session_changed = false;
    g_tile_sequence = 0U;
    g_visible_pending_session = 0U;
    g_visible_pending_sequence = 0U;
    g_visible_pending_window = 0U;
    g_activity_sequence = 0U;
    g_activity_cpu0_epoch = 0U;
    g_panel_shutdown_ack = false;
    state->published_command_sequence = 0U;
    state->published_mailbox_sequence = 0U;
    ipc_cpu1_state_publish();
    {
        volatile ra8p1_activity_cpu1_state_t *activity =
            &RA8P1_ACTIVITY_CONTROL->cpu1;
        activity->observed_cpu0_epoch = 0U;
        activity->acknowledged_message_sequence = 0U;
        activity->protocol_errors = 0U;
        activity->reserved = 0U;
        ipc_cpu1_activity_state_publish();
    }
}

bool ipc_bridge_cpu1_activity_poll(
    rf_v13_cpu0_round_message_t *message,
    bool *cpu0_epoch_changed)
{
    volatile ra8p1_activity_cpu0_state_t *source =
        &RA8P1_ACTIVITY_CONTROL->cpu0;
    volatile ra8p1_activity_cpu1_state_t *ack =
        &RA8P1_ACTIVITY_CONTROL->cpu1;
    ra8p1_activity_cpu0_state_t first;
    ra8p1_activity_cpu0_state_t final;
    rf_v13_cpu0_round_message_t candidate;

    if (cpu0_epoch_changed != NULL)
    {
        *cpu0_epoch_changed = false;
    }
    if (message == NULL)
    {
        return false;
    }

#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)source,
                                 (int32_t)sizeof(*source));
#endif
    ipc_barrier();
    memcpy(&first, (const void *)source, sizeof(first));
    if ((first.magic != RA8P1_ACTIVITY_CONTROL_MAGIC) ||
        (first.version != RA8P1_ACTIVITY_CONTROL_VERSION) ||
        (first.size != sizeof(first)) ||
        (first.boot_epoch == 0U) ||
        ((first.flags & RA8P1_ACTIVITY_CPU0_FLAG_READY) == 0U))
    {
        return false;
    }

    if (first.boot_epoch != g_activity_cpu0_epoch)
    {
        g_activity_cpu0_epoch = first.boot_epoch;
        g_activity_sequence = 0U;
        g_panel_shutdown_ack = false;
        ack->observed_cpu0_epoch = first.boot_epoch;
        ack->acknowledged_message_sequence = 0U;
        ipc_barrier();
        ipc_cpu1_activity_state_publish();
        if (cpu0_epoch_changed != NULL)
        {
            *cpu0_epoch_changed = true;
        }
    }

    if ((first.begin_sequence == 0U) ||
        ((first.begin_sequence & 1U) != 0U) ||
        (first.begin_sequence == g_activity_sequence) ||
        (first.begin_sequence != first.end_sequence) ||
        (first.message_sequence == 0U))
    {
        return false;
    }

#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)RA8P1_ACTIVITY_MESSAGE,
                                 (int32_t)sizeof(*RA8P1_ACTIVITY_MESSAGE));
#endif
    ipc_barrier();
    memcpy(&candidate, (const void *)RA8P1_ACTIVITY_MESSAGE,
           sizeof(candidate));
    ipc_barrier();
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)source,
                                 (int32_t)sizeof(*source));
#endif
    ipc_barrier();
    memcpy(&final, (const void *)source, sizeof(final));

    if ((first.begin_sequence != final.begin_sequence) ||
        (first.end_sequence != final.end_sequence) ||
        (first.message_sequence != final.message_sequence) ||
        (candidate.magic != RF_V13_ACTIVITY_MAGIC) ||
        (candidate.abi_major != RF_V13_ACTIVITY_ABI_MAJOR) ||
        (candidate.abi_minor != RF_V13_ACTIVITY_ABI_MINOR) ||
        (candidate.message_bytes != RF_V13_CPU0_MESSAGE_BYTES) ||
        (candidate.message_sequence != first.message_sequence))
    {
        ack->protocol_errors++;
        ipc_barrier();
        ipc_cpu1_activity_state_publish();
        return false;
    }

    *message = candidate;
    g_activity_sequence = first.begin_sequence;
    ack->acknowledged_message_sequence = candidate.message_sequence;
    ipc_barrier();
    ipc_cpu1_activity_state_publish();
    return true;
}

bool ipc_bridge_cpu1_poll(ra8p1_system_telemetry_t *telemetry)
{
    volatile ra8p1_telemetry_mailbox_t *mailbox = RA8P1_TELEMETRY_MAILBOX;
    uint32_t begin;
    uint32_t end;
    uint32_t final_begin;
    ra8p1_system_telemetry_t candidate;

    if (telemetry == NULL)
    {
        return false;
    }
    ipc_barrier();
    begin = mailbox->begin_sequence;
    if (((begin & 1U) != 0U) || (begin == 0U) || (begin == g_telemetry_sequence))
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
        (candidate.size != sizeof(candidate)))
    {
        return false;
    }
    *telemetry = candidate;
    g_telemetry_sequence = begin;
    return true;
}

bool ipc_bridge_cpu1_display_poll(ra8p1_display_frame_t *frame)
{
    volatile ra8p1_display_stream_control_t *control = RA8P1_DISPLAY_STREAM_CONTROL;
    ra8p1_display_frame_t oldest;
    uint32_t oldest_sequence = 0U;
    uint32_t session_id;
    bool found = false;
    uint32_t index;

    if (frame == NULL)
    {
        return false;
    }

    /* SDR sessions share one monotonic result queue. A CPU0 reboot is the
     * only event that restarts the producer sequence, so reset the consumer
     * from the handshake epoch rather than from each capture session. */
    if (g_observed_cpu0_boot_epoch != g_display_cpu0_boot_epoch)
    {
        g_display_cpu0_boot_epoch = g_observed_cpu0_boot_epoch;
        g_display_sequence = 0U;
        g_display_session_id = 0U;
        g_tile_sequence = 0U;
        g_visible_pending_session = 0U;
        g_visible_pending_sequence = 0U;
        g_visible_pending_window = 0U;
    }

    ipc_barrier();
    session_id = control->session_id;
    if ((session_id == 0U) ||
        (control->magic != RA8P1_DISPLAY_STREAM_MAGIC) ||
        (control->version != RA8P1_DISPLAY_STREAM_VERSION) ||
        (control->size != sizeof(*control)))
    {
        return false;
    }
    if (session_id != g_display_session_id)
    {
        g_display_session_id = session_id;
        g_display_session_changed = true;
        g_tile_sequence = 0U;
    }

    for (index = 0U; index < RA8P1_DISPLAY_STREAM_SLOT_COUNT; index++)
    {
        volatile ra8p1_display_stream_slot_t *slot = &RA8P1_DISPLAY_STREAM_SLOTS[index];
        ra8p1_display_frame_t candidate;
        uint32_t begin;
        uint32_t end;
        uint32_t final_begin;

        ipc_barrier();
        begin = slot->begin_sequence;
        if (((begin & 1U) != 0U) || (begin == 0U) ||
            ((int32_t) (begin - g_display_sequence) <= 0))
        {
            continue;
        }

        memcpy(&candidate, (const void *) &slot->payload, sizeof(candidate));
        ipc_barrier();
        end = slot->end_sequence;
        final_begin = slot->begin_sequence;
        if ((begin != end) || (begin != final_begin) ||
            (candidate.sequence != begin) ||
            (candidate.session_id == 0U) ||
            (candidate.magic != RA8P1_DISPLAY_STREAM_MAGIC) ||
            (candidate.version != RA8P1_DISPLAY_STREAM_VERSION) ||
            (candidate.size != sizeof(candidate)))
        {
            continue;
        }

        if (!found || ((int32_t) (begin - oldest_sequence) < 0))
        {
            oldest = candidate;
            oldest_sequence = begin;
            found = true;
        }
    }

    if (!found)
    {
        return false;
    }

    *frame = oldest;
    g_display_sequence = oldest_sequence;
    g_visible_pending_session = oldest.session_id;
    g_visible_pending_sequence = oldest.sequence;
    g_visible_pending_window = oldest.analysis.window_sequence;
    return true;
}

bool ipc_bridge_cpu1_display_visible(const ra8p1_display_frame_t *frame)
{
    uint64_t center_hz;
    if ((frame == NULL) ||
        (frame->magic != RA8P1_DISPLAY_STREAM_MAGIC) ||
        (frame->version != RA8P1_DISPLAY_STREAM_VERSION) ||
        (frame->size != sizeof(*frame)) ||
        (frame->session_id != g_visible_pending_session) ||
        (frame->sequence != g_visible_pending_sequence) ||
        (frame->analysis.window_sequence != g_visible_pending_window) ||
        (frame->analysis.center_index >= RA8P1_CENTER_COUNT))
    {
        return false;
    }
    center_hz = ((uint64_t)frame->analysis.center_frequency_high << 32U) |
                frame->analysis.center_frequency_low;
    if (center_hz != ra8p1_center_frequency_hz(frame->analysis.center_index))
    {
        return false;
    }
    ipc_cpu1_latency_ack(frame->session_id, frame->analysis.window_sequence);
    g_visible_pending_session = 0U;
    g_visible_pending_sequence = 0U;
    g_visible_pending_window = 0U;
    return true;
}

bool ipc_bridge_cpu1_display_session_changed(void)
{
    bool changed = g_display_session_changed;
    g_display_session_changed = false;
    return changed;
}

static bool ipc_bridge_cpu1_display_tile_copy(
    volatile ra8p1_display_tile_slot_t *slot,
    uint32_t sequence_floor,
    uint32_t exact_sequence,
    ra8p1_display_tile_payload_t *candidate)
{
    ra8p1_display_tile_payload_t snapshot;
    uint32_t begin;
    uint32_t end;
    uint32_t final_begin;

    if ((slot == NULL) || (candidate == NULL))
    {
        return false;
    }

    ipc_barrier();
    begin = slot->begin_sequence;
    if (((begin & 1U) != 0U) || (begin == 0U) ||
        ((int32_t)(begin - sequence_floor) <= 0) ||
        ((exact_sequence != 0U) && (begin != exact_sequence)))
    {
        return false;
    }
    memcpy(&snapshot, (const void *)&slot->payload, sizeof(snapshot));
    ipc_barrier();
    end = slot->end_sequence;
    final_begin = slot->begin_sequence;
    if ((begin != end) || (begin != final_begin) ||
        (snapshot.sequence != begin) ||
        (snapshot.session_id != g_display_session_id) ||
        (snapshot.magic != RA8P1_DISPLAY_TILE_MAGIC) ||
        (snapshot.version != RA8P1_DISPLAY_TILE_VERSION) ||
        (snapshot.size != sizeof(snapshot)) ||
        (snapshot.width_height != ((RA8P1_DISPLAY_TILE_WIDTH << 16U) |
                                   RA8P1_DISPLAY_TILE_HEIGHT)) ||
        (snapshot.center_index >= RA8P1_CENTER_COUNT) ||
        (snapshot.novel_time_start >= RA8P1_DISPLAY_TILE_HEIGHT) ||
        (snapshot.novel_time_count != 1U))
    {
        return false;
    }

    *candidate = snapshot;
    return true;
}

bool ipc_bridge_cpu1_display_tile_poll(ra8p1_display_tile_payload_t *tile)
{
    ra8p1_display_tile_payload_t oldest;
    uint32_t oldest_sequence = 0U;
    bool found = false;
    uint32_t index;

    if (tile == NULL)
    {
        return false;
    }

    /* In the no-loss case the producer's sequence maps to one deterministic
     * ring slot.  Read that slot directly; a full scan is only needed after
     * overwrite, a session handover, or a partially committed slot. */
    {
        const uint32_t expected_sequence =
            (g_tile_sequence + 2U) & ~1U;
        if (expected_sequence != 0U)
        {
            const uint32_t expected_index =
                ((expected_sequence >> 1U) - 1U) &
                (RA8P1_DISPLAY_TILE_SLOT_COUNT - 1U);
            if (ipc_bridge_cpu1_display_tile_copy(
                    &RA8P1_DISPLAY_TILE_SLOTS[expected_index],
                    g_tile_sequence,
                    expected_sequence,
                    tile))
            {
                g_tile_sequence = expected_sequence;
                return true;
            }
        }
    }

    for (index = 0U; index < RA8P1_DISPLAY_TILE_SLOT_COUNT; ++index)
    {
        volatile ra8p1_display_tile_slot_t *slot = &RA8P1_DISPLAY_TILE_SLOTS[index];
        ra8p1_display_tile_payload_t candidate;
        if (!ipc_bridge_cpu1_display_tile_copy(slot,
                                               g_tile_sequence,
                                               0U,
                                               &candidate))
        {
            continue;
        }
        /* Tiles are produced independently from completed display frames;
         * the transport sequence remains monotonic and orders them. The
         * logical window index may legitimately restart at zero during a
         * same-session replay. */
        if (!found ||
            ((int32_t)(candidate.sequence - oldest_sequence) < 0))
        {
            oldest = candidate;
            oldest_sequence = candidate.sequence;
            found = true;
        }
    }

    if (!found)
    {
        return false;
    }
    *tile = oldest;
    g_tile_sequence = oldest_sequence;
    return true;
}

bool ipc_bridge_cpu1_command_send(const ra8p1_ui_command_t *command)
{
    if ((command == NULL) ||
        (command->magic != RA8P1_SYSTEM_PROTOCOL_MAGIC) ||
        (command->version != RA8P1_SYSTEM_PROTOCOL_VERSION) ||
        (command->size != sizeof(*command)) ||
        (command->sequence == 0U))
    {
        return false;
    }
    if (g_command_pending)
    {
        return (command->sequence == g_last_command.sequence) &&
               (memcmp(command, &g_last_command, sizeof(*command)) == 0);
    }
    g_last_command = *command;
    g_have_last_command = true;
    g_command_pending = true;
    g_command_acked = false;
    g_command_has_published = false;
    g_command_published_cpu0_epoch = 0U;
    g_command_retry_ticks = CPU1_COMMAND_RETRY_SERVICE_CALLS;
    g_command_retry_count = 0U;
    ipc_cpu1_state_publish();
    ipc_bridge_cpu1_command_service();
    return true;
}

void ipc_bridge_cpu1_command_service(void)
{
    ra8p1_ipc_cpu0_state_t cpu0_state;
    if (!ipc_cpu1_cpu0_state_read(&cpu0_state))
    {
        g_cpu0_ready = false;
        return;
    }
    g_cpu0_ready = true;
    if (cpu0_state.boot_epoch != g_observed_cpu0_boot_epoch)
    {
        g_observed_cpu0_boot_epoch = cpu0_state.boot_epoch;
        g_command_published_cpu0_epoch = 0U;
        g_command_retry_ticks = CPU1_COMMAND_RETRY_SERVICE_CALLS;
        if (g_have_last_command && !g_command_pending)
        {
            g_command_pending = true;
            g_command_acked = false;
            g_command_has_published = false;
            g_command_retry_count = 0U;
        }
        ipc_cpu1_state_publish();
    }
    if (!g_command_pending)
    {
        return;
    }
    if ((cpu0_state.acknowledged_cpu1_epoch == g_cpu1_boot_epoch) &&
        (cpu0_state.acknowledged_command_sequence == g_last_command.sequence))
    {
        g_command_pending = false;
        g_command_acked = true;
        ipc_cpu1_state_publish();
        return;
    }
    if ((g_command_published_cpu0_epoch != g_observed_cpu0_boot_epoch) ||
        (g_command_retry_ticks >= CPU1_COMMAND_RETRY_SERVICE_CALLS))
    {
        ipc_cpu1_command_publish();
        return;
    }
    g_command_retry_ticks++;
}

bool ipc_bridge_cpu1_cpu0_ready(void)
{
    return g_cpu0_ready;
}

bool ipc_bridge_cpu1_command_pending(void)
{
    return g_command_pending;
}

uint32_t ipc_bridge_cpu1_command_retry_count(void)
{
    return g_command_retry_count;
}

bool ipc_bridge_cpu1_panel_shutdown_requested(void)
{
    volatile ra8p1_activity_cpu0_state_t * const state =
        &RA8P1_ACTIVITY_CONTROL->cpu0;
    ra8p1_activity_cpu0_state_t snapshot;
#if (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)state,
                                 (int32_t)sizeof(*state));
#endif
    ipc_barrier();
    memcpy(&snapshot, (const void *)state, sizeof(snapshot));
    ipc_barrier();
    return (snapshot.magic == RA8P1_ACTIVITY_CONTROL_MAGIC) &&
           (snapshot.version == RA8P1_ACTIVITY_CONTROL_VERSION) &&
           (snapshot.size == sizeof(snapshot)) &&
           (snapshot.boot_epoch == g_activity_cpu0_epoch) &&
           ((snapshot.flags & RA8P1_ACTIVITY_CPU0_FLAG_READY) != 0U) &&
           ((snapshot.flags &
             RA8P1_ACTIVITY_CPU0_FLAG_PANEL_SHUTDOWN_REQUEST) != 0U);
}

void ipc_bridge_cpu1_panel_shutdown_ack(void)
{
    g_panel_shutdown_ack = true;
    ipc_cpu1_activity_state_publish();
}

void ipc_bridge_cpu1_runtime_update(uint32_t heartbeat,
                                    uint32_t display_stage,
                                    uint32_t glcdc_line_events,
                                    int32_t last_error,
                                    uint32_t glcdc_underflows,
                                    uint32_t display_running,
                                    const ra8p1_runtime_status_t *metrics)
{
    volatile ra8p1_runtime_status_t *status = RA8P1_RUNTIME_STATUS;
    uint32_t sequence = (status->end_sequence + 2U) & ~1U;
    status->begin_sequence = sequence | 1U;
    ipc_barrier();
    status->cpu1_heartbeat = heartbeat;
    status->display_stage = display_stage;
    status->glcdc_line_events = glcdc_line_events;
    status->last_error = last_error;
    status->glcdc_underflows = glcdc_underflows;
    status->display_running = display_running;
    if (metrics != NULL)
    {
        status->metrics_version = RA8P1_RUNTIME_METRICS_VERSION;
        status->lvgl_tick_ms = metrics->lvgl_tick_ms;
        status->presented_frame_count = metrics->presented_frame_count;
        status->presented_fps_millihz = metrics->presented_fps_millihz;
        status->glcdc_underflow_rate_millihz = metrics->glcdc_underflow_rate_millihz;
        status->window_rate_millihz = metrics->window_rate_millihz;
        status->inference_rate_millihz = metrics->inference_rate_millihz;
        status->tile_rate_millihz = metrics->tile_rate_millihz;
        status->content_frame_count = metrics->content_frame_count;
        status->content_fps_millihz = metrics->content_fps_millihz;
        status->waterfall_columns_generated = metrics->waterfall_columns_generated;
        status->waterfall_tiles_consumed = metrics->waterfall_tiles_consumed;
        status->waterfall_tiles_dropped = metrics->waterfall_tiles_dropped;
        status->ipc_frames_received = metrics->ipc_frames_received;
        status->ipc_tiles_received = metrics->ipc_tiles_received;
        status->ipc_tiles_missed = metrics->ipc_tiles_missed;
        status->last_session_id = metrics->last_session_id;
        status->last_frame_sequence = metrics->last_frame_sequence;
        status->last_tile_sequence = metrics->last_tile_sequence;
        status->last_command_sequence = metrics->last_command_sequence;
        status->last_command_status = metrics->last_command_status;
        status->last_command_reason = metrics->last_command_reason;
        status->last_applied_session_id = metrics->last_applied_session_id;
        status->runtime_flags = metrics->runtime_flags;
    }
    ipc_barrier();
    status->end_sequence = sequence;
    status->begin_sequence = sequence;
    ipc_barrier();
}
