/* Fixed-port IQ UDP sink used to measure the RMAC path without lwIP overhead. */

#include <rtthread.h>
#include <string.h>
#include "eth_iq_fast.h"

#define ETH_HEADER_SIZE       14U
#define IPV4_MIN_HEADER_SIZE  20U
#define UDP_HEADER_SIZE       8U
#define IQ_HEADER_SIZE        32U
#define IQ_STATS_UPDATE_MASK  0xFFU

volatile eth_iq_fast_stats_t g_eth_iq_fast_stats
    __attribute__((section(".ram_nocache"), aligned(32))) =
{
    .magic = ETH_IQ_FAST_MAGIC,
    .schema_version = 1U
};

static uint16_t iq_read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t iq_read_le32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static void iq_reset_stats(uint32_t tick)
{
    memset((void *)&g_eth_iq_fast_stats, 0, sizeof(g_eth_iq_fast_stats));
    g_eth_iq_fast_stats.magic = ETH_IQ_FAST_MAGIC;
    g_eth_iq_fast_stats.schema_version = 1U;
    g_eth_iq_fast_stats.active = 1U;
    g_eth_iq_fast_stats.first_tick = tick;
    g_eth_iq_fast_stats.last_tick = tick;
}

int eth_iq_fast_consume(const uint8_t *frame, uint32_t frame_length)
{
    uint32_t ip_header_size;
    uint32_t udp_offset;
    uint32_t udp_length;
    uint32_t iq_length;
    uint32_t sequence;
    uint32_t data_length;
    uint32_t tick;
    const uint8_t *iq_header;
    const uint8_t *iq_data;

    if ((frame_length < (ETH_HEADER_SIZE + IPV4_MIN_HEADER_SIZE + UDP_HEADER_SIZE + IQ_HEADER_SIZE)) ||
        (frame[12] != 0x08U) || (frame[13] != 0x00U) ||
        ((frame[14] >> 4) != 4U) || (frame[23] != 17U))
    {
        return 0;
    }

    ip_header_size = (uint32_t)(frame[14] & 0x0FU) * 4U;
    if ((ip_header_size < IPV4_MIN_HEADER_SIZE) ||
        (frame_length < (ETH_HEADER_SIZE + ip_header_size + UDP_HEADER_SIZE + IQ_HEADER_SIZE)))
    {
        return 0;
    }

    udp_offset = ETH_HEADER_SIZE + ip_header_size;
    if (iq_read_be16(&frame[udp_offset + 2U]) != ETH_IQ_FAST_PORT)
    {
        return 0;
    }

    udp_length = iq_read_be16(&frame[udp_offset + 4U]);
    if ((udp_length < (UDP_HEADER_SIZE + IQ_HEADER_SIZE)) ||
        ((udp_offset + udp_length) > frame_length))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }

    iq_header = &frame[udp_offset + UDP_HEADER_SIZE];
    iq_length = udp_length - UDP_HEADER_SIZE;
    if (iq_read_le32(&iq_header[0]) != ETH_IQ_FAST_MAGIC)
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }

    sequence = iq_read_le32(&iq_header[4]);
    data_length = iq_read_le32(&iq_header[8]);
    if ((data_length > (iq_length - IQ_HEADER_SIZE)) || ((data_length & 3U) != 0U))
    {
        g_eth_iq_fast_stats.invalid++;
        return 1;
    }

    if ((sequence == 0U) || (g_eth_iq_fast_stats.active == 0U))
    {
        tick = (uint32_t)rt_tick_get();
        iq_reset_stats(tick);
    }
    else if (sequence == g_eth_iq_fast_stats.next_sequence)
    {
        /* Expected packet. */
    }
    else if ((uint32_t)(sequence - g_eth_iq_fast_stats.next_sequence) < 0x80000000UL)
    {
        g_eth_iq_fast_stats.sequence_gaps += sequence - g_eth_iq_fast_stats.next_sequence;
    }
    else
    {
        g_eth_iq_fast_stats.reordered++;
    }

    iq_data = &iq_header[IQ_HEADER_SIZE];
    g_eth_iq_fast_stats.packets++;
    g_eth_iq_fast_stats.payload_bytes += data_length;
    g_eth_iq_fast_stats.next_sequence = sequence + 1U;
    if ((g_eth_iq_fast_stats.packets & IQ_STATS_UPDATE_MASK) == 0U)
    {
        tick = (uint32_t)rt_tick_get();
        g_eth_iq_fast_stats.last_tick = tick;
        g_eth_iq_fast_stats.elapsed_ms =
            (uint32_t)(((uint64_t)(tick - g_eth_iq_fast_stats.first_tick) * 1000U) / RT_TICK_PER_SECOND);
        if (g_eth_iq_fast_stats.elapsed_ms > 0U)
        {
            g_eth_iq_fast_stats.mbps_x1000 =
                (uint32_t)(((uint64_t)g_eth_iq_fast_stats.payload_bytes * 8U) /
                           g_eth_iq_fast_stats.elapsed_ms);
        }
    }
    if (data_length >= sizeof(uint32_t))
    {
        g_eth_iq_fast_stats.data_checksum =
            ((g_eth_iq_fast_stats.data_checksum << 1) |
             (g_eth_iq_fast_stats.data_checksum >> 31)) ^ iq_read_le32(iq_data);
    }
    g_eth_iq_fast_stats.last_sender_us = iq_read_le32(&iq_header[24]);
    g_eth_iq_fast_stats.flags = iq_read_le32(&iq_header[12]);

    return 1;
}
