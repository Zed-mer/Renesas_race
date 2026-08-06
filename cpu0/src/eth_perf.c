/*
 * UDP Ethernet performance service. Socket TX uses port 5001; the high-rate
 * RX sink uses lwIP raw UDP on port 5002 to avoid the socket receive mailbox.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include "hal_data.h"
#include "eth_perf.h"

#define ETH_PERF_THREAD_STACK_SIZE 8192U
#define ETH_PERF_THREAD_PRIORITY   11
#define ETH_PERF_THREAD_TICK       5
#define ETH_PERF_MAX_PAYLOAD       1472U

static struct rt_thread s_perf_thread;
static rt_uint8_t s_perf_thread_stack[ETH_PERF_THREAD_STACK_SIZE];
static rt_bool_t s_perf_thread_started = RT_FALSE;
static rt_uint8_t s_tx_payload[ETH_PERF_MAX_PAYLOAD];
static rt_uint8_t s_rx_payload[ETH_PERF_MAX_PAYLOAD + 64U];

static rt_uint32_t eth_perf_elapsed_ms(rt_tick_t start_tick)
{
    rt_tick_t elapsed_ticks = rt_tick_get() - start_tick;
    return (rt_uint32_t)(((rt_uint64_t)elapsed_ticks * 1000U) / RT_TICK_PER_SECOND);
}

static const char *eth_perf_speed_name(rt_uint32_t speed)
{
    switch (speed)
    {
        case ETHER_PHY_LINK_SPEED_10H:   return "10-half";
        case ETHER_PHY_LINK_SPEED_10F:   return "10-full";
        case ETHER_PHY_LINK_SPEED_100H:  return "100-half";
        case ETHER_PHY_LINK_SPEED_100F:  return "100-full";
        case ETHER_PHY_LINK_SPEED_1000H: return "1000-half";
        case ETHER_PHY_LINK_SPEED_1000F: return "1000-full";
        default:                         return "no-link";
    }
}

static const char *eth_perf_skip_space(const char *text)
{
    while ((*text == ' ') || (*text == '\t'))
    {
        text++;
    }
    return text;
}

static rt_uint32_t eth_perf_parse_u32(const char **text, rt_uint32_t fallback)
{
    char *end = RT_NULL;
    unsigned long value;

    *text = eth_perf_skip_space(*text);
    value = strtoul(*text, &end, 0);
    if (end == *text)
    {
        return fallback;
    }

    *text = end;
    return (rt_uint32_t)value;
}

static void eth_perf_send_text(int socket_fd,
                               const struct sockaddr_in *peer,
                               socklen_t peer_len,
                               const char *text,
                               rt_uint32_t repeats)
{
    rt_uint32_t i;
    rt_size_t length = rt_strlen(text);

    for (i = 0U; i < repeats; i++)
    {
        (void)sendto(socket_fd, text, length, 0, (const struct sockaddr *)peer, peer_len);
        if (repeats > 1U)
        {
            rt_thread_mdelay(10);
        }
    }
}

static void eth_perf_send_info(int socket_fd,
                               const struct sockaddr_in *peer,
                               socklen_t peer_len)
{
    char report[192];
    uint32_t speed = ETHER_PHY_LINK_SPEED_NO_LINK;
    uint32_t local_pause = 0U;
    uint32_t partner_pause = 0U;
    fsp_err_t result;

    result = g_rmac_phy0.p_api->linkPartnerAbilityGet(g_rmac_phy0.p_ctrl,
                                                       &speed,
                                                       &local_pause,
                                                       &partner_pause);
    rt_snprintf(report,
                sizeof(report),
                "PERF INFO result=%d speed=%s speed_enum=%lu dcache=%u pbuf=%u tx_port=%u raw_rx_port=%u",
                (int)result,
                eth_perf_speed_name(speed),
                (unsigned long)speed,
                (unsigned int)BSP_CFG_DCACHE_ENABLED,
                (unsigned int)RT_LWIP_PBUF_NUM,
                (unsigned int)ETH_PERF_SOCKET_PORT,
                (unsigned int)ETH_PERF_RAW_RX_PORT);
    eth_perf_send_text(socket_fd, peer, peer_len, report, 1U);
}

static void eth_perf_run_tx(int socket_fd,
                            const struct sockaddr_in *peer,
                            socklen_t peer_len,
                            const char *args)
{
    char report[192];
    rt_uint32_t duration_ms = eth_perf_parse_u32(&args, 3000U);
    rt_uint32_t payload_size = eth_perf_parse_u32(&args, ETH_PERF_MAX_PAYLOAD);
    rt_uint32_t packets = 0U;
    rt_uint32_t errors = 0U;
    rt_uint32_t bytes = 0U;
    rt_tick_t start_tick;
    rt_uint32_t elapsed_ms;

    if (duration_ms < 100U)
    {
        duration_ms = 100U;
    }
    if (duration_ms > 60000U)
    {
        duration_ms = 60000U;
    }
    if (payload_size < 64U)
    {
        payload_size = 64U;
    }
    if (payload_size > ETH_PERF_MAX_PAYLOAD)
    {
        payload_size = ETH_PERF_MAX_PAYLOAD;
    }

    rt_memset(s_tx_payload, 0xA5, payload_size);
    eth_perf_send_text(socket_fd, peer, peer_len, "PERF TXREADY", 1U);

    start_tick = rt_tick_get();
    do
    {
        int sent;
        rt_uint32_t magic = ETH_PERF_PACKET_MAGIC;

        rt_memcpy(&s_tx_payload[0], &magic, sizeof(magic));
        rt_memcpy(&s_tx_payload[4], &packets, sizeof(packets));
        sent = sendto(socket_fd,
                      s_tx_payload,
                      payload_size,
                      0,
                      (const struct sockaddr *)peer,
                      peer_len);
        if (sent == (int)payload_size)
        {
            packets++;
            bytes += payload_size;
        }
        else
        {
            errors++;
        }
    } while (eth_perf_elapsed_ms(start_tick) < duration_ms);

    elapsed_ms = eth_perf_elapsed_ms(start_tick);
    rt_snprintf(report,
                sizeof(report),
                "PERF TXDONE packets=%lu bytes=%lu elapsed_ms=%lu errors=%lu",
                (unsigned long)packets,
                (unsigned long)bytes,
                (unsigned long)elapsed_ms,
                (unsigned long)errors);
    eth_perf_send_text(socket_fd, peer, peer_len, report, ETH_PERF_REPORT_REPEATS);
}

static void eth_perf_thread_entry(void *parameter)
{
    int socket_fd;
    struct sockaddr_in local_addr;
    rt_bool_t rx_active = RT_FALSE;
    rt_uint32_t rx_bytes = 0U;
    rt_uint32_t rx_packets = 0U;
    rt_tick_t rx_start_tick = 0U;

    RT_UNUSED(parameter);

    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        rt_kprintf("eth_perf: socket failed\n");
        return;
    }

    rt_memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(ETH_PERF_SOCKET_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, (const struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        rt_kprintf("eth_perf: bind port %u failed\n", (unsigned int)ETH_PERF_SOCKET_PORT);
        closesocket(socket_fd);
        return;
    }

    rt_kprintf("eth_perf: UDP socket port %u ready\n", (unsigned int)ETH_PERF_SOCKET_PORT);
    while (1)
    {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int received;

        received = recvfrom(socket_fd,
                            s_rx_payload,
                            sizeof(s_rx_payload) - 1U,
                            0,
                            (struct sockaddr *)&peer,
                            &peer_len);
        if (received <= 0)
        {
            continue;
        }

        if ((received >= 5) && (rt_memcmp(s_rx_payload, "PERF ", 5U) == 0))
        {
            const char *command;
            char report[192];

            s_rx_payload[received] = '\0';
            command = (const char *)&s_rx_payload[5];
            if (rt_strncmp(command, "INFO", 4U) == 0)
            {
                eth_perf_send_info(socket_fd, &peer, peer_len);
            }
            else if (rt_strncmp(command, "TX", 2U) == 0)
            {
                eth_perf_run_tx(socket_fd, &peer, peer_len, command + 2U);
            }
            else if (rt_strncmp(command, "RXSTART", 7U) == 0)
            {
                rx_packets = 0U;
                rx_bytes = 0U;
                rx_start_tick = rt_tick_get();
                rx_active = RT_TRUE;
                eth_perf_send_text(socket_fd, &peer, peer_len, "PERF RXREADY", 1U);
            }
            else if (rt_strncmp(command, "RXSTOP", 6U) == 0)
            {
                rt_uint32_t elapsed_ms = eth_perf_elapsed_ms(rx_start_tick);
                rx_active = RT_FALSE;
                rt_snprintf(report,
                            sizeof(report),
                            "PERF RXDONE packets=%lu bytes=%lu elapsed_ms=%lu",
                            (unsigned long)rx_packets,
                            (unsigned long)rx_bytes,
                            (unsigned long)elapsed_ms);
                eth_perf_send_text(socket_fd, &peer, peer_len, report, ETH_PERF_REPORT_REPEATS);
            }
            else
            {
                eth_perf_send_text(socket_fd, &peer, peer_len, "PERF ERROR unknown-command", 1U);
            }
        }
        else if (rx_active)
        {
            rx_packets++;
            rx_bytes += (rt_uint32_t)received;
        }
    }
}

void eth_perf_start(void)
{
    if (s_perf_thread_started)
    {
        return;
    }

    eth_perf_raw_service_start();

    if (rt_thread_init(&s_perf_thread,
                       "ethperf",
                       eth_perf_thread_entry,
                       RT_NULL,
                       s_perf_thread_stack,
                       sizeof(s_perf_thread_stack),
                       ETH_PERF_THREAD_PRIORITY,
                       ETH_PERF_THREAD_TICK) == RT_EOK)
    {
        s_perf_thread_started = RT_TRUE;
        rt_thread_startup(&s_perf_thread);
    }
    else
    {
        rt_kprintf("eth_perf: thread start failed\n");
    }
}
