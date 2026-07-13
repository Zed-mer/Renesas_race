#ifndef ETH_IQ_FAST_H
#define ETH_IQ_FAST_H

#include <stdint.h>

#define ETH_IQ_FAST_MAGIC 0x5149504BUL
#define ETH_IQ_FAST_PORT  5003U

typedef struct st_eth_iq_fast_stats
{
    uint32_t magic;
    uint32_t schema_version;
    uint32_t active;
    uint32_t packets;
    uint32_t payload_bytes;
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid;
    uint32_t first_tick;
    uint32_t last_tick;
    uint32_t elapsed_ms;
    uint32_t mbps_x1000;
    uint32_t next_sequence;
    uint32_t data_checksum;
    uint32_t last_sender_us;
    uint32_t flags;
} eth_iq_fast_stats_t;

extern volatile eth_iq_fast_stats_t g_eth_iq_fast_stats;

int eth_iq_fast_consume(const uint8_t *frame, uint32_t frame_length);

#endif
