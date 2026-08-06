/* High-rate UDP sink that runs directly in the lwIP tcpip thread. */

#include <rtthread.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include "eth_perf.h"
#include "eth_iq_fast.h"
#include "framework/iq_protocol.h"
#include "framework/iq_ring.h"

typedef struct st_eth_perf_raw_rx
{
    struct udp_pcb *pcb;
    rt_bool_t active;
    rt_bool_t sequence_valid;
    rt_tick_t start_tick;
    rt_uint32_t bytes;
    rt_uint32_t packets;
    rt_uint32_t next_sequence;
    rt_uint32_t sequence_gaps;
    rt_uint32_t out_of_order;
    rt_uint32_t invalid_packets;
    rt_uint32_t elapsed_ms;
} eth_perf_raw_rx_t;

static eth_perf_raw_rx_t s_raw_rx;

static rt_uint32_t eth_perf_raw_elapsed_ms(rt_tick_t start_tick)
{
    rt_tick_t elapsed_ticks = rt_tick_get() - start_tick;
    return (rt_uint32_t)(((rt_uint64_t)elapsed_ticks * 1000U) / RT_TICK_PER_SECOND);
}

static void eth_perf_raw_send_text(struct udp_pcb *pcb,
                                   const ip_addr_t *peer,
                                   u16_t peer_port,
                                   const char *text,
                                   rt_uint32_t repeats)
{
    struct pbuf *response;
    u16_t length = (u16_t)rt_strlen(text);
    rt_uint32_t i;

    response = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
    if (response == RT_NULL)
    {
        return;
    }
    if (pbuf_take(response, text, length) != ERR_OK)
    {
        pbuf_free(response);
        return;
    }

    for (i = 0U; i < repeats; i++)
    {
        (void)udp_sendto(pcb, response, peer, peer_port);
    }
    pbuf_free(response);
}

static void eth_perf_raw_send_iq_ack(struct udp_pcb *pcb,
                                     const ip_addr_t *peer,
                                     u16_t peer_port,
                                     const ra8p1_iq_ack_request_t *request)
{
    ra8p1_iq_ack_response_t response;
    eth_iq_fast_stats_t iq_stats;
    iq_ring_stats_t ring_stats;
    struct pbuf *payload;
    rt_uint32_t ring_free;
    rt_uint32_t session_ring_full_drops = 0U;
    rt_uint32_t session_ring_oversize_drops = 0U;
    rt_uint32_t ack_flags = 0U;
    rt_uint32_t status = RA8P1_IQ_ACK_STATUS_OK;

    rt_memset(&response, 0, sizeof(response));
    rt_memset(&iq_stats, 0, sizeof(iq_stats));
    rt_memcpy(&iq_stats, (const void *)&g_eth_iq_fast_stats, sizeof(iq_stats));
    iq_ring_stats_get(&ring_stats);
    eth_iq_fast_session_ring_drops(&session_ring_full_drops,
                                   &session_ring_oversize_drops);
    ring_free = (ring_stats.queued < IQ_RING_SLOT_COUNT) ?
                (IQ_RING_SLOT_COUNT - ring_stats.queued) : 0U;

    response.magic = RA8P1_IQ_ACK_RESPONSE_MAGIC;
    response.version = RA8P1_IQ_ACK_VERSION;
    response.size = (uint16_t)sizeof(response);
    response.request_id = (request != RT_NULL) ? request->request_id : 0U;
    response.session_id = iq_stats.session_id;
    response.packets = iq_stats.packets;
    response.payload_bytes = iq_stats.payload_bytes;
    response.sequence_gaps = iq_stats.sequence_gaps;
    response.reordered = iq_stats.reordered;
    response.invalid = iq_stats.invalid;
    response.ring_full_drops = session_ring_full_drops;
    response.ring_oversize_drops = session_ring_oversize_drops;
    response.ring_free = ring_free;
    response.crc32c = iq_stats.crc32c;
    response.expected_crc32c = iq_stats.expected_crc32c;
    response.crc_errors = iq_stats.crc_errors;

    if ((request == RT_NULL) ||
        (request->magic != RA8P1_IQ_ACK_REQUEST_MAGIC) ||
        (request->version != RA8P1_IQ_ACK_VERSION) ||
        (request->size != sizeof(*request)))
    {
        status = RA8P1_IQ_ACK_STATUS_INVALID_REQUEST;
    }
    else if ((iq_stats.magic != ETH_IQ_FAST_MAGIC) ||
             (iq_stats.session_id == 0U))
    {
        status = RA8P1_IQ_ACK_STATUS_NO_SESSION;
    }
    else if ((request->session_id != 0U) &&
             (request->session_id != iq_stats.session_id))
    {
        status = RA8P1_IQ_ACK_STATUS_SESSION_MISMATCH;
    }
    else if ((iq_stats.active != 0U) || (ring_stats.queued != 0U))
    {
        status = RA8P1_IQ_ACK_STATUS_ACTIVE;
        ack_flags |= RA8P1_IQ_ACK_FLAG_ACTIVE;
    }
    else if (((iq_stats.crc_flags & RA8P1_IQ_ACK_FLAG_CRC_PRESENT) != 0U) &&
             ((iq_stats.crc_flags & RA8P1_IQ_ACK_FLAG_CRC_VALID) == 0U))
    {
        status = RA8P1_IQ_ACK_STATUS_CRC_MISMATCH;
    }
    else if ((iq_stats.sequence_gaps != 0U) ||
             (iq_stats.reordered != 0U))
    {
        status = RA8P1_IQ_ACK_STATUS_SEQUENCE_ERROR;
    }
    else if (iq_stats.invalid != 0U)
    {
        status = RA8P1_IQ_ACK_STATUS_INVALID_DATA;
    }
    else if ((session_ring_full_drops != 0U) ||
             (session_ring_oversize_drops != 0U))
    {
        status = RA8P1_IQ_ACK_STATUS_RING_DROP;
    }

    if ((iq_stats.active == 0U) && (ring_stats.queued == 0U) &&
        (iq_stats.session_id != 0U))
    {
        ack_flags |= RA8P1_IQ_ACK_FLAG_COMPLETE;
    }
    ack_flags |= iq_stats.crc_flags &
                 (RA8P1_IQ_ACK_FLAG_CRC_PRESENT |
                  RA8P1_IQ_ACK_FLAG_CRC_VALID);
    if ((iq_stats.sequence_gaps != 0U) || (iq_stats.reordered != 0U))
    {
        ack_flags |= RA8P1_IQ_ACK_FLAG_SEQUENCE_ERROR;
    }
    if ((session_ring_full_drops != 0U) ||
        (session_ring_oversize_drops != 0U))
    {
        ack_flags |= RA8P1_IQ_ACK_FLAG_RING_DROP;
    }
    response.status = status;
    response.flags = ack_flags;

    payload = pbuf_alloc(PBUF_TRANSPORT, (u16_t)sizeof(response), PBUF_RAM);
    if (payload == RT_NULL)
    {
        return;
    }
    if (pbuf_take(payload, &response, sizeof(response)) == ERR_OK)
    {
        (void)udp_sendto(pcb, payload, peer, peer_port);
    }
    pbuf_free(payload);
}

static void eth_perf_raw_start(struct udp_pcb *pcb,
                               const ip_addr_t *peer,
                               u16_t peer_port)
{
    s_raw_rx.active = RT_FALSE;
    s_raw_rx.sequence_valid = RT_FALSE;
    s_raw_rx.bytes = 0U;
    s_raw_rx.packets = 0U;
    s_raw_rx.next_sequence = 0U;
    s_raw_rx.sequence_gaps = 0U;
    s_raw_rx.out_of_order = 0U;
    s_raw_rx.invalid_packets = 0U;
    s_raw_rx.elapsed_ms = 0U;
    s_raw_rx.start_tick = rt_tick_get();
    s_raw_rx.active = RT_TRUE;
    eth_perf_raw_send_text(pcb, peer, peer_port, "PERF RXREADY RAW", 1U);
}

static void eth_perf_raw_stop(struct udp_pcb *pcb,
                              const ip_addr_t *peer,
                              u16_t peer_port)
{
    char report[224];

    if (s_raw_rx.active)
    {
        s_raw_rx.elapsed_ms = eth_perf_raw_elapsed_ms(s_raw_rx.start_tick);
        s_raw_rx.active = RT_FALSE;
    }
    rt_snprintf(report,
                sizeof(report),
                "PERF RXDONE packets=%lu bytes=%lu elapsed_ms=%lu seq_lost=%lu reorder=%lu invalid=%lu raw=1",
                (unsigned long)s_raw_rx.packets,
                (unsigned long)s_raw_rx.bytes,
                (unsigned long)s_raw_rx.elapsed_ms,
                (unsigned long)s_raw_rx.sequence_gaps,
                (unsigned long)s_raw_rx.out_of_order,
                (unsigned long)s_raw_rx.invalid_packets);
    eth_perf_raw_send_text(pcb, peer, peer_port, report, ETH_PERF_REPORT_REPEATS);
}

static void eth_perf_raw_receive(void *arg,
                                 struct udp_pcb *pcb,
                                 struct pbuf *packet,
                                 const ip_addr_t *peer,
                                 u16_t peer_port)
{
    rt_uint8_t header[8];
    char command[24];
    u16_t command_length;

    RT_UNUSED(arg);
    if (packet == RT_NULL)
    {
        return;
    }

    /* A binary IQACK request is deliberately distinct from the legacy ASCII
     * PERF commands and can be polled while a raw benchmark is running. */
    if (packet->tot_len >= sizeof(ra8p1_iq_ack_request_t))
    {
        ra8p1_iq_ack_request_t request;
        if ((pbuf_copy_partial(packet, &request, sizeof(request), 0U) ==
             sizeof(request)) &&
            (request.magic == RA8P1_IQ_ACK_REQUEST_MAGIC))
        {
            eth_perf_raw_send_iq_ack(pcb, peer, peer_port, &request);
            pbuf_free(packet);
            return;
        }
    }

    command_length = (packet->tot_len < (sizeof(command) - 1U))
                         ? packet->tot_len
                         : (sizeof(command) - 1U);
    if (command_length >= 5U)
    {
        (void)pbuf_copy_partial(packet, command, command_length, 0U);
        command[command_length] = '\0';
        if (rt_memcmp(command, "PERF ", 5U) == 0)
        {
            if (rt_strncmp(&command[5], "RXSTART", 7U) == 0)
            {
                eth_perf_raw_start(pcb, peer, peer_port);
            }
            else if (rt_strncmp(&command[5], "RXSTOP", 6U) == 0)
            {
                eth_perf_raw_stop(pcb, peer, peer_port);
            }
            else
            {
                eth_perf_raw_send_text(pcb, peer, peer_port, "PERF ERROR raw-command", 1U);
            }
            pbuf_free(packet);
            return;
        }
    }

    if (s_raw_rx.active)
    {
        rt_uint32_t magic;
        rt_uint32_t sequence;

        if ((packet->tot_len >= sizeof(header)) &&
            (pbuf_copy_partial(packet, header, sizeof(header), 0U) == sizeof(header)))
        {
            rt_memcpy(&magic, &header[0], sizeof(magic));
            rt_memcpy(&sequence, &header[4], sizeof(sequence));
            if (magic == ETH_PERF_PACKET_MAGIC)
            {
                if (!s_raw_rx.sequence_valid)
                {
                    s_raw_rx.sequence_valid = RT_TRUE;
                    s_raw_rx.next_sequence = sequence + 1U;
                }
                else if (sequence == s_raw_rx.next_sequence)
                {
                    s_raw_rx.next_sequence++;
                }
                else if ((rt_uint32_t)(sequence - s_raw_rx.next_sequence) < 0x80000000UL)
                {
                    s_raw_rx.sequence_gaps += sequence - s_raw_rx.next_sequence;
                    s_raw_rx.next_sequence = sequence + 1U;
                }
                else
                {
                    s_raw_rx.out_of_order++;
                }
                s_raw_rx.packets++;
                s_raw_rx.bytes += packet->tot_len;
            }
            else
            {
                s_raw_rx.invalid_packets++;
            }
        }
        else
        {
            s_raw_rx.invalid_packets++;
        }
    }
    pbuf_free(packet);
}

static void eth_perf_raw_init(void *parameter)
{
    err_t result;

    RT_UNUSED(parameter);
    s_raw_rx.pcb = udp_new();
    if (s_raw_rx.pcb == RT_NULL)
    {
        rt_kprintf("eth_perf: raw UDP allocation failed\n");
        return;
    }

    result = udp_bind(s_raw_rx.pcb, IP_ADDR_ANY, ETH_PERF_RAW_RX_PORT);
    if (result != ERR_OK)
    {
        udp_remove(s_raw_rx.pcb);
        s_raw_rx.pcb = RT_NULL;
        rt_kprintf("eth_perf: raw UDP bind port %u failed: %d\n",
                   (unsigned int)ETH_PERF_RAW_RX_PORT,
                   (int)result);
        return;
    }

    udp_recv(s_raw_rx.pcb, eth_perf_raw_receive, RT_NULL);
    rt_kprintf("eth_perf: raw UDP RX port %u ready\n", (unsigned int)ETH_PERF_RAW_RX_PORT);
}

void eth_perf_raw_service_start(void)
{
    err_t result = tcpip_callback(eth_perf_raw_init, RT_NULL);

    if (result != ERR_OK)
    {
        rt_kprintf("eth_perf: raw UDP init queue failed: %d\n", (int)result);
    }
}
