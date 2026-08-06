#ifndef ETH_IQ_FAST_H
#define ETH_IQ_FAST_H

#include <stdbool.h>
#include <stdint.h>

#define ETH_IQ_FAST_MAGIC 0x5149504BUL
#define ETH_IQ_FAST_PORT  5003U
/* Sparse SDRC replies use a separate bounded queue so an IQ burst cannot
 * delay CAPTURE_COMPLETE/CREDIT_ACCEPTED behind lwIP socket scheduling. */
#define ETH_IQ_FAST_CONTROL_PORT       5004U
#define ETH_IQ_FAST_CONTROL_WIRE_BYTES 164U
#define ETH_IQ_FAST_STATS_SCHEMA_VERSION 6U

#define ETH_IQ_FAST_CRC_TIMING_END_VALID      (1U << 0)
#define ETH_IQ_FAST_CRC_TIMING_COMPLETE_VALID (1U << 1)

typedef struct st_eth_iq_fast_stats
{
    uint32_t magic;
    uint32_t schema_version;
    uint32_t active;
    uint32_t packets;
    uint64_t payload_bytes;
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid;
    uint32_t first_tick;
    uint32_t last_tick;
    uint32_t elapsed_ms;
    uint32_t mbps_x1000;
    uint32_t next_sequence;
    uint32_t data_checksum;
    uint32_t session_id;
    uint32_t flags;
    /* Optional WINDOW_CRC evidence. These fields are appended so the v3
     * statistics offsets remain stable for existing samplers. */
    uint32_t crc32c;
    uint32_t expected_crc32c;
    uint32_t crc_errors;
    uint32_t crc_flags;
    uint32_t crc_backend;
    uint32_t crc_updates;
    uint64_t crc_cycles_total;
    uint32_t crc_cycles_max;
    uint32_t crc_hw_self_test;
    /* CPU0 DWT timestamps for the END datagram and the point at which the
     * consumer has incorporated the final IQ payload into the window CRC. */
    uint32_t end_packet_cpu0_cycles;
    uint32_t crc_complete_cpu0_cycles;
    uint32_t crc_after_end_cycles;
    uint32_t crc_timing_flags;
} eth_iq_fast_stats_t;

typedef char eth_iq_fast_stats_size_must_be_128[
    (sizeof(eth_iq_fast_stats_t) == 128U) ? 1 : -1];

typedef struct st_eth_iq_fast_control_datagram
{
    uint32_t source_address;
    uint16_t source_port;
    uint16_t length;
    uint8_t wire[ETH_IQ_FAST_CONTROL_WIRE_BYTES];
} eth_iq_fast_control_datagram_t;

extern volatile eth_iq_fast_stats_t g_eth_iq_fast_stats;

int eth_iq_fast_consume(const uint8_t *frame, uint32_t frame_length);

/* The RMAC receive path only enqueues a validated-length SDRC datagram. The
 * RF worker remains the sole parser and state-machine owner. */
void eth_iq_fast_control_init(void);
bool eth_iq_fast_control_pop(eth_iq_fast_control_datagram_t *datagram);

/* Update the optional window CRC from the copied ring payload.  This runs in
 * the RF consumer thread so the RMAC descriptor can be released immediately
 * after iq_ring_push_copy() returns. */
void eth_iq_fast_crc_consume(uint32_t session_id,
                             const uint8_t *data,
                             uint32_t length);

/* Return ring-drop deltas relative to the current IQSC session START.  The
 * ring counters themselves are lifetime diagnostics; ACK status is
 * intentionally session-scoped so a prior failed attempt can be retried. */
void eth_iq_fast_session_ring_drops(uint32_t *full_drops,
                                    uint32_t *oversize_drops);

#endif
