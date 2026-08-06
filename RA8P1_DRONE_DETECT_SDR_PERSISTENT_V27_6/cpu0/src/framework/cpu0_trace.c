#include "cpu0_trace.h"

#include <stddef.h>
#include <string.h>

#include "hal_data.h"

volatile cpu0_trace_ring_t g_cpu0_trace_ring
    __attribute__((section(".sdram_noinit_nocache"), aligned(32), used));

static uint32_t g_trace_sequence;
static uint32_t g_active_session_id;
static uint32_t g_active_record_index;

static void trace_barrier(void)
{
    __asm volatile ("dmb" ::: "memory");
}

static uint32_t trace_next_sequence(void)
{
    uint32_t sequence = (g_trace_sequence + 2U) & ~1U;
    if (sequence == 0U)
    {
        sequence = 2U;
    }
    g_trace_sequence = sequence;
    return sequence;
}

static bool trace_record_snapshot(uint32_t index, cpu0_trace_record_t *record)
{
    const volatile cpu0_trace_record_t *source;
    uint32_t begin;
    uint32_t end;

    if ((record == NULL) || (index >= CPU0_TRACE_CAPACITY))
    {
        return false;
    }
    source = &g_cpu0_trace_ring.records[index];
    begin = source->begin_sequence;
    if ((begin == 0U) || ((begin & 1U) != 0U))
    {
        return false;
    }
    trace_barrier();
    memcpy(record, (const void *)source, sizeof(*record));
    trace_barrier();
    end = source->end_sequence;
    return (begin == end) && (begin == source->begin_sequence) &&
           (record->begin_sequence == begin) &&
           (record->end_sequence == end) && (record->session_id != 0U);
}

static void trace_record_store(uint32_t index,
                               const cpu0_trace_record_t *record)
{
    volatile cpu0_trace_record_t *destination;
    uint32_t sequence;

    if ((record == NULL) || (index >= CPU0_TRACE_CAPACITY))
    {
        return;
    }
    destination = &g_cpu0_trace_ring.records[index];
    sequence = trace_next_sequence();
    destination->begin_sequence = sequence | 1U;
    trace_barrier();
    memcpy((void *)&destination->request_id,
           (const void *)&record->request_id,
           offsetof(cpu0_trace_record_t, end_sequence) -
               offsetof(cpu0_trace_record_t, request_id));
    trace_barrier();
    destination->end_sequence = sequence;
    destination->begin_sequence = sequence;
    trace_barrier();
    g_cpu0_trace_ring.control.latest_sequence = sequence;
}

static bool trace_find(uint32_t session_id,
                       uint32_t window_index,
                       uint32_t *index,
                       cpu0_trace_record_t *record)
{
    uint32_t i;
    if ((session_id == 0U) || (index == NULL) || (record == NULL))
    {
        return false;
    }
    if ((session_id == g_active_session_id) &&
        trace_record_snapshot(g_active_record_index, record) &&
        (record->session_id == session_id) &&
        (record->window_index == window_index))
    {
        *index = g_active_record_index;
        return true;
    }
    for (i = 0U; i < CPU0_TRACE_CAPACITY; ++i)
    {
        if (trace_record_snapshot(i, record) &&
            (record->session_id == session_id) &&
            (record->window_index == window_index))
        {
            *index = i;
            return true;
        }
    }
    return false;
}

void cpu0_trace_init(void)
{
    uint32_t boot_count = 1U;
    if ((g_cpu0_trace_ring.control.magic == CPU0_TRACE_MAGIC) &&
        (g_cpu0_trace_ring.control.version == CPU0_TRACE_VERSION) &&
        (g_cpu0_trace_ring.control.record_bytes == CPU0_TRACE_RECORD_BYTES))
    {
        boot_count = g_cpu0_trace_ring.control.boot_count + 1U;
        if (boot_count == 0U)
        {
            boot_count = 1U;
        }
    }
    memset((void *)&g_cpu0_trace_ring, 0, sizeof(g_cpu0_trace_ring));
    g_trace_sequence = 0U;
    g_active_session_id = 0U;
    g_active_record_index = 0U;
    g_cpu0_trace_ring.control.magic = CPU0_TRACE_MAGIC;
    g_cpu0_trace_ring.control.version = CPU0_TRACE_VERSION;
    g_cpu0_trace_ring.control.record_bytes = CPU0_TRACE_RECORD_BYTES;
    g_cpu0_trace_ring.control.capacity = CPU0_TRACE_CAPACITY;
    g_cpu0_trace_ring.control.cpu_cycle_hz = CPU0_TRACE_CPU_CYCLE_HZ;
    g_cpu0_trace_ring.control.boot_count = boot_count;
    trace_barrier();
}

bool cpu0_trace_cycle_now(uint32_t *cycles)
{
    if (cycles == NULL)
    {
        return false;
    }
    if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
        ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U))
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        *((volatile uint32_t *)0xE0001FB0UL) = 0xC5ACCE55UL;
        if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
        {
            return false;
        }
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        __DSB();
        __ISB();
    }
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        return false;
    }
    __DSB();
    *cycles = DWT->CYCCNT;
    __ISB();
    return true;
}

void cpu0_trace_control_tx(const ra8p1_sdr_control_message_t *message,
                           uint32_t cycles)
{
    cpu0_trace_record_t record;
    uint32_t index;

    if ((message == NULL) || (message->session_id == 0U))
    {
        return;
    }
    if (message->command == RA8P1_SDR_CONTROL_CAPTURE_REQ)
    {
        if (trace_find(message->session_id, 0U, &index, &record))
        {
            record.state_flags |= CPU0_TRACE_FLAG_RETRY;
            record.status = message->status;
            trace_record_store(index, &record);
            return;
        }
        memset(&record, 0, sizeof(record));
        index = g_cpu0_trace_ring.control.records_started &
                (CPU0_TRACE_CAPACITY - 1U);
        if (g_cpu0_trace_ring.control.records_started >= CPU0_TRACE_CAPACITY)
        {
            g_cpu0_trace_ring.control.records_overwritten++;
        }
        record.request_id = message->request_id;
        record.session_id = message->session_id;
        record.center_index = message->center_index;
        record.sample_count = message->sample_count;
        record.state_flags = CPU0_TRACE_FLAG_REQUEST_TX_VALID;
        record.request_tx_cycles = cycles;
        trace_record_store(index, &record);
        g_cpu0_trace_ring.control.records_started++;
        g_active_session_id = message->session_id;
        g_active_record_index = index;
    }
    else if ((message->command == RA8P1_SDR_CONTROL_WINDOW_ACK) &&
             trace_find(message->session_id, 0U, &index, &record))
    {
        if ((record.state_flags & CPU0_TRACE_FLAG_ACK_TX_VALID) == 0U)
        {
            /* Bind latency to the first WINDOW_ACK. Later idempotent retries
             * are reliability evidence and must not inflate result->ACK. */
            record.ack_tx_cycles = cycles;
            record.state_flags |= CPU0_TRACE_FLAG_ACK_TX_VALID;
        }
        else
        {
            record.state_flags |= CPU0_TRACE_FLAG_RETRY;
        }
        record.status = message->status;
        trace_record_store(index, &record);
    }
}

void cpu0_trace_control_rx(const ra8p1_sdr_control_message_t *message,
                           uint32_t cycles,
                           bool cycles_valid)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if ((message == NULL) ||
        !trace_find(message->session_id, 0U, &index, &record))
    {
        return;
    }
    /* ACCEPTED/STARTED/CREDIT responses legitimately carry only a prefix of
     * the remote timeline.  Preserve earlier nonzero evidence instead of
     * letting a later sparse response erase it. */
    if (message->agent_request_rx_us != 0U)
        record.agent_request_rx_us = message->agent_request_rx_us;
    if (message->tune_start_us != 0U)
        record.tune_start_us = message->tune_start_us;
    if (message->tune_complete_us != 0U)
        record.tune_complete_us = message->tune_complete_us;
    if (message->capture_start_us != 0U)
        record.capture_start_us = message->capture_start_us;
    if (message->capture_complete_us != 0U)
        record.capture_complete_us = message->capture_complete_us;
    if (message->actual_payload_mbps_x1000 != 0U)
        record.actual_payload_mbps_x1000 =
            message->actual_payload_mbps_x1000;
    if (message->window_crc32c != 0U)
        record.window_crc32c = message->window_crc32c;
    record.status = message->status;
    if ((record.agent_request_rx_us != 0U) &&
        (record.capture_start_us != 0U) &&
        (record.capture_complete_us >= record.capture_start_us))
    {
        record.state_flags |= CPU0_TRACE_FLAG_REMOTE_TIMES_VALID;
    }
    switch (message->command)
    {
        case RA8P1_SDR_CONTROL_CAPTURE_READY:
            if (cycles_valid &&
                ((record.state_flags &
                  CPU0_TRACE_FLAG_CAPTURE_READY_VALID) == 0U))
            {
                record.capture_ready_cycles = cycles;
                record.state_flags |= CPU0_TRACE_FLAG_CAPTURE_READY_VALID;
            }
            break;
        case RA8P1_SDR_CONTROL_CAPTURE_COMPLETE:
            if (cycles_valid &&
                ((record.state_flags &
                  CPU0_TRACE_FLAG_CAPTURE_COMPLETE_VALID) == 0U))
            {
                record.capture_complete_cycles = cycles;
                record.state_flags |=
                    CPU0_TRACE_FLAG_CAPTURE_COMPLETE_VALID;
            }
            break;
        case RA8P1_SDR_CONTROL_CREDIT_ACCEPTED:
            if (cycles_valid &&
                ((record.state_flags &
                  CPU0_TRACE_FLAG_CREDIT_ACCEPTED_VALID) == 0U))
            {
                record.credit_accepted_cycles = cycles;
                record.state_flags |=
                    CPU0_TRACE_FLAG_CREDIT_ACCEPTED_VALID;
            }
            break;
        default:
            break;
    }
    trace_record_store(index, &record);
}

void cpu0_trace_iqsc_start(uint32_t session_id, uint32_t cycles)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if (!trace_find(session_id, 0U, &index, &record))
    {
        return;
    }
    if ((record.state_flags & CPU0_TRACE_FLAG_IQSC_START_VALID) == 0U)
    {
        record.iqsc_start_cycles = cycles;
        record.state_flags |= CPU0_TRACE_FLAG_IQSC_START_VALID;
        trace_record_store(index, &record);
    }
}

void cpu0_trace_ingress(uint32_t session_id,
                        uint32_t first_packet_cycles,
                        uint32_t last_packet_cycles)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if (!trace_find(session_id, 0U, &index, &record))
    {
        return;
    }
    if (first_packet_cycles != 0U)
    {
        record.first_packet_cycles = first_packet_cycles;
        record.state_flags |= CPU0_TRACE_FLAG_FIRST_PACKET_VALID;
    }
    if (last_packet_cycles != 0U)
    {
        record.last_packet_cycles = last_packet_cycles;
        record.state_flags |= CPU0_TRACE_FLAG_LAST_PACKET_VALID;
    }
    trace_record_store(index, &record);
}

void cpu0_trace_crc(uint32_t session_id,
                    uint32_t complete_cycles,
                    uint32_t crc_cycles,
                    uint32_t crc32c,
                    bool crc_valid,
                    uint32_t actual_payload_mbps_x1000,
                    uint32_t sequence_gaps,
                    uint32_t reordered,
                    uint32_t invalid_packets,
                    uint32_t ring_full_drops,
                    uint32_t ring_oversize_drops,
                    uint32_t ring_high_watermark,
                    uint32_t ring_free)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if (!trace_find(session_id, 0U, &index, &record))
    {
        return;
    }
    record.crc_complete_cycles = complete_cycles;
    record.crc_cycles = crc_cycles;
    record.window_crc32c = crc32c;
    record.actual_payload_mbps_x1000 = actual_payload_mbps_x1000;
    record.sequence_gaps = sequence_gaps;
    record.reordered = reordered;
    record.invalid_packets = invalid_packets;
    record.ring_full_drops = ring_full_drops;
    record.ring_oversize_drops = ring_oversize_drops;
    record.ring_high_watermark = ring_high_watermark;
    record.ring_free = ring_free;
    if (crc_valid)
    {
        record.state_flags |= CPU0_TRACE_FLAG_CRC_VALID;
    }
    else
    {
        record.state_flags &= ~CPU0_TRACE_FLAG_CRC_VALID;
        record.status = RA8P1_SDR_CONTROL_STATUS_IQ_CRC;
    }
    trace_record_store(index, &record);
}

void cpu0_trace_analysis(uint32_t session_id,
                         uint32_t window_index,
                         uint32_t stft_start_cycles,
                         uint32_t stft_end_cycles,
                         uint32_t npu_start_cycles,
                         uint32_t npu_end_cycles,
                         uint32_t cpu0_load_permille,
                         const cpu0_trace_inference_phases_t *phases)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if (!trace_find(session_id, window_index, &index, &record))
    {
        cpu0_trace_record_t base;
        uint32_t base_index;
        if ((window_index == 0U) ||
            !trace_find(session_id, 0U, &base_index, &base))
        {
            return;
        }
        record = base;
        record.window_index = window_index;
        record.state_flags &= ~(CPU0_TRACE_FLAG_STFT_START_VALID |
                                CPU0_TRACE_FLAG_STFT_END_VALID |
                                CPU0_TRACE_FLAG_NPU_START_VALID |
                                CPU0_TRACE_FLAG_NPU_END_VALID |
                                CPU0_TRACE_FLAG_CPU1_VISIBLE_VALID |
                                CPU0_TRACE_FLAG_NPU_PHASES_VALID |
                                CPU0_TRACE_FLAG_POSTPROCESS_VALID);
        record.stft_start_cycles = 0U;
        record.stft_end_cycles = 0U;
        record.npu_start_cycles = 0U;
        record.npu_end_cycles = 0U;
        record.cpu1_visible_cycles = 0U;
        record.v2_input_copy_cycles = 0U;
        record.v2_invoke_cycles = 0U;
        record.v2_output_copy_cycles = 0U;
        record.v3_input_copy_cycles = 0U;
        record.v3_invoke_cycles = 0U;
        record.v3_output_copy_cycles = 0U;
        record.postprocess_cycles = 0U;
        index = g_cpu0_trace_ring.control.records_started &
                (CPU0_TRACE_CAPACITY - 1U);
        if (g_cpu0_trace_ring.control.records_started >= CPU0_TRACE_CAPACITY)
        {
            g_cpu0_trace_ring.control.records_overwritten++;
        }
        g_cpu0_trace_ring.control.records_started++;
    }
    record.stft_start_cycles = stft_start_cycles;
    record.stft_end_cycles = stft_end_cycles;
    record.npu_start_cycles = npu_start_cycles;
    record.npu_end_cycles = npu_end_cycles;
    record.cpu0_load_permille = cpu0_load_permille;
    record.state_flags &= ~(CPU0_TRACE_FLAG_NPU_PHASES_VALID |
                            CPU0_TRACE_FLAG_POSTPROCESS_VALID);
    record.v2_input_copy_cycles = 0U;
    record.v2_invoke_cycles = 0U;
    record.v2_output_copy_cycles = 0U;
    record.v3_input_copy_cycles = 0U;
    record.v3_invoke_cycles = 0U;
    record.v3_output_copy_cycles = 0U;
    record.postprocess_cycles = 0U;
    if (phases != NULL)
    {
        record.v2_input_copy_cycles = phases->v2_input_copy_cycles;
        record.v2_invoke_cycles = phases->v2_invoke_cycles;
        record.v2_output_copy_cycles = phases->v2_output_copy_cycles;
        record.v3_input_copy_cycles = phases->v3_input_copy_cycles;
        record.v3_invoke_cycles = phases->v3_invoke_cycles;
        record.v3_output_copy_cycles = phases->v3_output_copy_cycles;
        record.postprocess_cycles = phases->postprocess_cycles;
        if ((phases->flags & CPU0_TRACE_PHASE_NPU_VALID) != 0U)
        {
            record.state_flags |= CPU0_TRACE_FLAG_NPU_PHASES_VALID;
        }
        if ((phases->flags & CPU0_TRACE_PHASE_POSTPROCESS_VALID) != 0U)
        {
            record.state_flags |= CPU0_TRACE_FLAG_POSTPROCESS_VALID;
        }
    }
    record.state_flags |= CPU0_TRACE_FLAG_STFT_START_VALID |
                          CPU0_TRACE_FLAG_STFT_END_VALID |
                          CPU0_TRACE_FLAG_NPU_START_VALID |
                          CPU0_TRACE_FLAG_NPU_END_VALID;
    trace_record_store(index, &record);
}

void cpu0_trace_cpu1_visible(uint32_t session_id,
                             uint32_t window_index,
                             uint32_t visible_cycles)
{
    cpu0_trace_record_t record;
    uint32_t index;
    if (!trace_find(session_id, window_index, &index, &record))
    {
        return;
    }
    record.cpu1_visible_cycles = visible_cycles;
    record.state_flags |= CPU0_TRACE_FLAG_CPU1_VISIBLE_VALID;
    trace_record_store(index, &record);
}
