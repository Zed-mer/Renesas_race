#ifndef CPU0_TRACE_H
#define CPU0_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "sdr_control_protocol.h"

#define CPU0_TRACE_MAGIC              (0x30435254UL) /* TRC0 */
#define CPU0_TRACE_VERSION            (3U)
#define CPU0_TRACE_CAPACITY           (128U)
#define CPU0_TRACE_CONTROL_BYTES      (32U)
#define CPU0_TRACE_RECORD_BYTES       (208U)
#define CPU0_TRACE_CPU_CYCLE_HZ       (1000000000UL)

#define CPU0_TRACE_FLAG_REQUEST_TX_VALID  (1UL << 0)
#define CPU0_TRACE_FLAG_REMOTE_TIMES_VALID (1UL << 1)
#define CPU0_TRACE_FLAG_FIRST_PACKET_VALID (1UL << 2)
#define CPU0_TRACE_FLAG_LAST_PACKET_VALID  (1UL << 3)
#define CPU0_TRACE_FLAG_CRC_VALID          (1UL << 4)
#define CPU0_TRACE_FLAG_ACK_TX_VALID       (1UL << 5)
#define CPU0_TRACE_FLAG_STFT_START_VALID   (1UL << 6)
#define CPU0_TRACE_FLAG_STFT_END_VALID     (1UL << 7)
#define CPU0_TRACE_FLAG_NPU_START_VALID    (1UL << 8)
#define CPU0_TRACE_FLAG_NPU_END_VALID      (1UL << 9)
#define CPU0_TRACE_FLAG_CPU1_VISIBLE_VALID (1UL << 10)
#define CPU0_TRACE_FLAG_RETRY              (1UL << 11)
#define CPU0_TRACE_FLAG_CAPTURE_READY_VALID (1UL << 12)
#define CPU0_TRACE_FLAG_CAPTURE_COMPLETE_VALID (1UL << 13)
#define CPU0_TRACE_FLAG_CREDIT_ACCEPTED_VALID (1UL << 14)
#define CPU0_TRACE_FLAG_IQSC_START_VALID   (1UL << 15)
#define CPU0_TRACE_FLAG_NPU_PHASES_VALID   (1UL << 16)
#define CPU0_TRACE_FLAG_POSTPROCESS_VALID  (1UL << 17)

#define CPU0_TRACE_PHASE_NPU_VALID         (1UL << 0)
#define CPU0_TRACE_PHASE_POSTPROCESS_VALID (1UL << 1)

typedef struct st_cpu0_trace_control
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_bytes;
    uint32_t capacity;
    uint32_t cpu_cycle_hz;
    uint32_t records_started;
    uint32_t records_overwritten;
    uint32_t latest_sequence;
    uint32_t boot_count;
} cpu0_trace_control_t;

typedef struct st_cpu0_trace_inference_phases
{
    uint32_t v2_input_copy_cycles;
    uint32_t v2_invoke_cycles;
    uint32_t v2_output_copy_cycles;
    uint32_t v3_input_copy_cycles;
    uint32_t v3_invoke_cycles;
    uint32_t v3_output_copy_cycles;
    uint32_t postprocess_cycles;
    uint32_t flags;
} cpu0_trace_inference_phases_t;

typedef struct st_cpu0_trace_record
{
    volatile uint32_t begin_sequence;
    uint32_t request_id;
    uint32_t session_id;
    uint32_t center_index;
    uint32_t window_index;
    uint32_t sample_count;
    uint32_t state_flags;
    uint32_t status;
    uint32_t request_tx_cycles;
    uint32_t first_packet_cycles;
    uint32_t last_packet_cycles;
    uint32_t crc_complete_cycles;
    uint32_t ack_tx_cycles;
    uint32_t stft_start_cycles;
    uint32_t stft_end_cycles;
    uint32_t npu_start_cycles;
    uint32_t npu_end_cycles;
    uint32_t cpu1_visible_cycles;
    uint64_t agent_request_rx_us;
    uint64_t tune_start_us;
    uint64_t tune_complete_us;
    uint64_t capture_start_us;
    uint64_t capture_complete_us;
    uint32_t actual_payload_mbps_x1000;
    uint32_t window_crc32c;
    uint32_t crc_cycles;
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid_packets;
    uint32_t ring_full_drops;
    uint32_t ring_oversize_drops;
    uint32_t ring_high_watermark;
    uint32_t ring_free;
    uint32_t cpu0_load_permille;
    uint32_t capture_ready_cycles;
    uint32_t capture_complete_cycles;
    uint32_t credit_accepted_cycles;
    uint32_t iqsc_start_cycles;
    uint32_t v2_input_copy_cycles;
    uint32_t v2_invoke_cycles;
    uint32_t v2_output_copy_cycles;
    uint32_t v3_input_copy_cycles;
    uint32_t v3_invoke_cycles;
    uint32_t v3_output_copy_cycles;
    uint32_t postprocess_cycles;
    volatile uint32_t end_sequence;
} cpu0_trace_record_t;

typedef struct st_cpu0_trace_ring
{
    cpu0_trace_control_t control;
    cpu0_trace_record_t records[CPU0_TRACE_CAPACITY];
} cpu0_trace_ring_t;

extern volatile cpu0_trace_ring_t g_cpu0_trace_ring;

void cpu0_trace_init(void);
bool cpu0_trace_cycle_now(uint32_t *cycles);
void cpu0_trace_control_tx(const ra8p1_sdr_control_message_t *message,
                           uint32_t cycles);
void cpu0_trace_control_rx(const ra8p1_sdr_control_message_t *message,
                           uint32_t cycles,
                           bool cycles_valid);
void cpu0_trace_iqsc_start(uint32_t session_id, uint32_t cycles);
void cpu0_trace_ingress(uint32_t session_id,
                        uint32_t first_packet_cycles,
                        uint32_t last_packet_cycles);
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
                    uint32_t ring_free);
void cpu0_trace_analysis(uint32_t session_id,
                         uint32_t window_index,
                         uint32_t stft_start_cycles,
                         uint32_t stft_end_cycles,
                         uint32_t npu_start_cycles,
                         uint32_t npu_end_cycles,
                         uint32_t cpu0_load_permille,
                         const cpu0_trace_inference_phases_t *phases);
void cpu0_trace_cpu1_visible(uint32_t session_id,
                             uint32_t window_index,
                             uint32_t visible_cycles);

typedef char cpu0_trace_control_size_must_be_32[
    (sizeof(cpu0_trace_control_t) == CPU0_TRACE_CONTROL_BYTES) ? 1 : -1];
typedef char cpu0_trace_inference_phases_size_must_be_32[
    (sizeof(cpu0_trace_inference_phases_t) == 32U) ? 1 : -1];
typedef char cpu0_trace_record_size_must_be_208[
    (sizeof(cpu0_trace_record_t) == CPU0_TRACE_RECORD_BYTES) ? 1 : -1];
typedef char cpu0_trace_capacity_must_be_power_of_two[
    ((CPU0_TRACE_CAPACITY & (CPU0_TRACE_CAPACITY - 1U)) == 0U) ? 1 : -1];

#endif
