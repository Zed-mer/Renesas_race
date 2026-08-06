#ifndef RA8P1_LATENCY_TELEMETRY_H
#define RA8P1_LATENCY_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>
#include "resource_layout.h"

#define RA8P1_LATENCY_MAGIC              (0x5954414CUL) /* LATY */
#define RA8P1_LATENCY_VERSION            (1U)
#define RA8P1_LATENCY_CPU0_CYCLE_HZ      (1000000000UL)
#define RA8P1_LATENCY_CACHE_LINE_BYTES   (32U)
#define RA8P1_LATENCY_SLOT_COUNT         (4U)
#define RA8P1_LATENCY_CONTROL_BYTES      (32U)
#define RA8P1_LATENCY_RECORD_BYTES       (32U)

#define RA8P1_LATENCY_WINDOW_INDEX_MASK          (0x0000FFFFUL)
#define RA8P1_LATENCY_FLAG_FIRST_PACKET_VALID    (0x00010000UL)
#define RA8P1_LATENCY_FLAG_WINDOW_COMPLETE_VALID (0x00020000UL)
#define RA8P1_LATENCY_FLAG_NPU_PUBLISH_VALID     (0x00040000UL)
#define RA8P1_LATENCY_FLAG_CPU1_VISIBLE_UPPER_VALID (0x00080000UL)

/* CPU1 first writes the current even seqlock value into
 * cpu1_visible_cpu0_cycles. CPU0 converts that acknowledgement to a CPU0 DWT
 * timestamp under the seqlock. The resulting visible delta is therefore a
 * conservative upper bound in one clock domain, not a cross-core clock
 * subtraction. */
typedef struct st_ra8p1_latency_control
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t cpu0_cycle_hz;
    uint32_t published_windows;
    uint32_t overwritten_windows;
    uint32_t overwritten_unacked_windows;
    uint32_t cpu1_visible_windows;
    uint32_t latest_sequence;
} ra8p1_latency_control_t;

typedef struct st_ra8p1_latency_record
{
    volatile uint32_t begin_sequence;
    uint32_t session_id;
    uint32_t window_index_flags;
    uint32_t first_packet_cpu0_cycles;
    uint32_t window_complete_cpu0_cycles;
    uint32_t npu_publish_cpu0_cycles;
    volatile uint32_t cpu1_visible_cpu0_cycles;
    volatile uint32_t end_sequence;
} ra8p1_latency_record_t;

#define RA8P1_LATENCY_CONTROL \
    ((volatile ra8p1_latency_control_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_LATENCY_OFFSET))

#define RA8P1_LATENCY_RECORDS \
    ((volatile ra8p1_latency_record_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_LATENCY_OFFSET + \
      RA8P1_LATENCY_CONTROL_BYTES))

typedef char ra8p1_latency_control_size_must_be_32[
    (sizeof(ra8p1_latency_control_t) == RA8P1_LATENCY_CONTROL_BYTES) ? 1 : -1];
typedef char ra8p1_latency_record_size_must_be_32[
    (sizeof(ra8p1_latency_record_t) == RA8P1_LATENCY_RECORD_BYTES) ? 1 : -1];
typedef char ra8p1_latency_visible_word_must_be_atomic_aligned[
    ((offsetof(ra8p1_latency_record_t, cpu1_visible_cpu0_cycles) & 3U) == 0U) ? 1 : -1];
typedef char ra8p1_latency_slot_count_must_be_power_of_two[
    ((RA8P1_LATENCY_SLOT_COUNT & (RA8P1_LATENCY_SLOT_COUNT - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_latency_region_size_must_match[
    ((RA8P1_LATENCY_CONTROL_BYTES +
      (RA8P1_LATENCY_SLOT_COUNT * RA8P1_LATENCY_RECORD_BYTES)) ==
     RA8P1_IPC_LATENCY_BYTES) ? 1 : -1];
typedef char ra8p1_latency_region_must_follow_command[
    (RA8P1_IPC_LATENCY_OFFSET >=
     (RA8P1_IPC_COMMAND_OFFSET + RA8P1_IPC_COMMAND_BYTES)) ? 1 : -1];
typedef char ra8p1_latency_region_must_precede_runtime[
    ((RA8P1_IPC_LATENCY_OFFSET + RA8P1_IPC_LATENCY_BYTES) <=
     RA8P1_IPC_RUNTIME_OFFSET) ? 1 : -1];
typedef char ra8p1_latency_control_must_be_cache_line_aligned[
    (((RA8P1_SHARED_RAM_BASE + RA8P1_IPC_LATENCY_OFFSET) &
      (RA8P1_LATENCY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_latency_records_must_be_cache_line_aligned[
    ((RA8P1_LATENCY_CONTROL_BYTES &
      (RA8P1_LATENCY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];

#endif
