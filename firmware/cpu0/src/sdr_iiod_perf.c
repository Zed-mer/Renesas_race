/*
 * Boot-time SDR iiod RX throughput probe. Results remain readable over J-Link.
 */

#include <rtthread.h>
#include <netdev_ipaddr.h>
#include <netdev.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include "hal_data.h"
#include "sdr_iiod_perf.h"

#define SDR_PERF_NETMASK           "255.255.255.0"
#define SDR_PERF_IIOD_PORT         30431U
#define SDR_PERF_OPEN_SAMPLES      131072U
#define SDR_PERF_REQUEST_BYTES     1048576U
#define SDR_PERF_WINDOW_SAMPLES    590336ULL
#define SDR_PERF_SAMPLE_BYTES      4ULL
#define SDR_PERF_TARGET_WINDOWS    5ULL
#define SDR_PERF_TARGET_BYTES      (SDR_PERF_WINDOW_SAMPLES * SDR_PERF_SAMPLE_BYTES * SDR_PERF_TARGET_WINDOWS)
#define SDR_PERF_IO_CHUNK          65536U
#define SDR_PERF_RECEIVE_BUFFER    131072
#define SDR_PERF_SOCKET_TIMEOUT_MS 10000
#define SDR_PERF_THREAD_STACK_SIZE 12288U
#define SDR_PERF_THREAD_PRIORITY   9
#define SDR_PERF_THREAD_TICK       5

enum
{
    SDR_PERF_STATE_INIT = 0,
    SDR_PERF_STATE_WAIT_LINK = 1,
    SDR_PERF_STATE_CONFIG_IP = 2,
    SDR_PERF_STATE_CONNECT = 3,
    SDR_PERF_STATE_VERSION = 4,
    SDR_PERF_STATE_OPEN = 5,
    SDR_PERF_STATE_STREAM = 6,
    SDR_PERF_STATE_DONE = 7,
    SDR_PERF_STATE_FAILED = 0x80000000UL
};

volatile sdr_iiod_perf_result_t g_sdr_iiod_perf_result
    __attribute__((section(".ram_nocache"), aligned(32)));
volatile char g_sdr_iiod_perf_report[256]
    __attribute__((section(".ram_nocache"), aligned(32)));

static struct rt_thread s_sdr_perf_thread;
static rt_uint8_t s_sdr_perf_stack[SDR_PERF_THREAD_STACK_SIZE];
/* A 64 KiB TCP receive scratch block is not latency-critical and would consume
 * half of the remaining CPU0 on-chip RAM.  Keep it in the CPU0-owned SDRAM
 * region; the socket copy makes this CPU-owned memory, not an RMAC DMA buffer. */
static rt_uint8_t s_io_buffer[SDR_PERF_IO_CHUNK]
    __attribute__((section(".sdram_noinit"), aligned(32), used));
/* iiod mixes ASCII length/mask lines and binary payload on one TCP stream.
 * Reading those lines one byte at a time turns the control parser into the
 * throughput bottleneck.  Keep a small socket-level cache so each recv call
 * amortizes lwIP/syscall overhead over a full segment. */
#define SDR_PERF_RX_CACHE_SIZE 65536U
static rt_uint8_t s_rx_cache[SDR_PERF_RX_CACHE_SIZE]
    __attribute__((section(".sdram_noinit"), aligned(32), used));
static rt_uint32_t s_rx_cache_pos;
static rt_uint32_t s_rx_cache_len;
static rt_bool_t s_sdr_perf_started = RT_FALSE;

typedef struct st_sdr_perf_candidate
{
    const char *local_ip;
    const char *gateway;
    const char *peer_ip;
} sdr_perf_candidate_t;

/* Try the project address first, then the two addresses documented by the
 * user-supplied Pluto images.  This changes only the RA8-side interface and
 * never writes the SDR configuration. */
static const sdr_perf_candidate_t s_candidates[] =
{
    {"192.168.31.20", "192.168.31.1", "192.168.31.10"},
    {"192.168.2.10",  "192.168.2.254", "192.168.2.1"},
    {"192.168.1.20",  "192.168.1.254", "192.168.1.10"},
};

static void sdr_perf_publish_result(void)
{
    uintptr_t start = (uintptr_t)&g_sdr_iiod_perf_report[0];
    uintptr_t end = (uintptr_t)&g_sdr_iiod_perf_result + sizeof(g_sdr_iiod_perf_result);

    start &= ~((uintptr_t)31U);
    end = (end + 31U) & ~((uintptr_t)31U);
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    __DSB();
}

static rt_uint32_t sdr_perf_elapsed_ms(rt_tick_t start_tick)
{
    rt_tick_t elapsed_ticks = rt_tick_get() - start_tick;
    return (rt_uint32_t)(((rt_uint64_t)elapsed_ticks * 1000U) / RT_TICK_PER_SECOND);
}

static void sdr_perf_update_report(void)
{
    const volatile sdr_iiod_perf_result_t *result = &g_sdr_iiod_perf_result;

    rt_snprintf((char *)g_sdr_iiod_perf_report,
                sizeof(g_sdr_iiod_perf_report),
                "SDRPERF v=%lu state=%lu last=%ld bytes=%lu/%lu elapsed_ms=%lu "
                "mbps_x1000=%lu reads=%lu chunks=%lu recv=%lu fills=%lu "
                "rcvbuf=%ld checksum=%08lx mask=%08lx errors=%lu peer=%s",
                (unsigned long)result->schema_version,
                (unsigned long)result->state,
                (long)result->last_error,
                (unsigned long)result->bytes_received,
                (unsigned long)result->target_bytes,
                (unsigned long)result->elapsed_ms,
                (unsigned long)result->payload_mbps_x1000,
                (unsigned long)result->read_requests,
                (unsigned long)result->read_chunks,
                (unsigned long)result->recv_calls,
                (unsigned long)result->cache_fills,
                (long)result->rcvbuf_setsockopt_result,
                (unsigned long)result->checksum,
                (unsigned long)result->stream_mask,
                (unsigned long)result->errors,
                result->peer_ip);
    sdr_perf_publish_result();
}

static void sdr_perf_fail(int error)
{
    g_sdr_iiod_perf_result.last_error = error;
    g_sdr_iiod_perf_result.errors++;
    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_FAILED;
    sdr_perf_update_report();
    rt_kprintf("%s\n", (const char *)g_sdr_iiod_perf_report);
}

static int sdr_perf_send_all(int socket_fd, const void *data, rt_size_t length)
{
    const rt_uint8_t *cursor = (const rt_uint8_t *)data;

    while (length > 0U)
    {
        int sent = send(socket_fd, cursor, length, 0);
        if (sent <= 0)
        {
            return -1;
        }
        cursor += sent;
        length -= (rt_size_t)sent;
    }

    return 0;
}

static int sdr_perf_fill_cache(int socket_fd)
{
    g_sdr_iiod_perf_result.recv_calls++;
    int received = recv(socket_fd, s_rx_cache, sizeof(s_rx_cache), 0);
    if (received <= 0)
    {
        return -1;
    }
    g_sdr_iiod_perf_result.cache_fills++;
    s_rx_cache_pos = 0U;
    s_rx_cache_len = (rt_uint32_t)received;
    return 0;
}

static int sdr_perf_recv_exact(int socket_fd, void *data, rt_size_t length)
{
    rt_uint8_t *cursor = (rt_uint8_t *)data;

    while (length > 0U)
    {
        rt_size_t available;
        rt_size_t take;
        if (s_rx_cache_pos == s_rx_cache_len)
        {
            if (sdr_perf_fill_cache(socket_fd) != 0)
            {
                return -1;
            }
        }
        available = (rt_size_t)(s_rx_cache_len - s_rx_cache_pos);
        take = (length < available) ? length : available;
        rt_memcpy(cursor, &s_rx_cache[s_rx_cache_pos], take);
        cursor += take;
        length -= take;
        s_rx_cache_pos += (rt_uint32_t)take;
    }

    return 0;
}

static int sdr_perf_recv_line(int socket_fd, char *line, rt_size_t capacity)
{
    rt_size_t used = 0U;

    if (capacity < 2U)
    {
        return -1;
    }

    while (used < (capacity - 1U))
    {
        rt_uint32_t scan;
        if (s_rx_cache_pos == s_rx_cache_len)
        {
            if (sdr_perf_fill_cache(socket_fd) != 0)
            {
                return -1;
            }
        }
        for (scan = s_rx_cache_pos; scan < s_rx_cache_len; scan++)
        {
            if (s_rx_cache[scan] == '\n')
            {
                rt_size_t take = (rt_size_t)(scan - s_rx_cache_pos);
                if (take > ((capacity - 1U) - used))
                {
                    return -1;
                }
                rt_memcpy(&line[used], &s_rx_cache[s_rx_cache_pos], take);
                used += take;
                s_rx_cache_pos = scan + 1U;
                if ((used > 0U) && (line[used - 1U] == '\r'))
                {
                    used--;
                }
                line[used] = '\0';
                return (int)used;
            }
        }
        {
            rt_size_t take = (rt_size_t)(s_rx_cache_len - s_rx_cache_pos);
            if (take > ((capacity - 1U) - used))
            {
                return -1;
            }
            rt_memcpy(&line[used], &s_rx_cache[s_rx_cache_pos], take);
            used += take;
            s_rx_cache_pos = s_rx_cache_len;
        }
    }

    line[capacity - 1U] = '\0';
    return -1;
}

static int sdr_perf_exec_integer(int socket_fd, const char *command)
{
    char line[48];
    char *end = RT_NULL;
    long value;

    if (sdr_perf_send_all(socket_fd, command, rt_strlen(command)) != 0)
    {
        return -10001;
    }
    if (sdr_perf_recv_line(socket_fd, line, sizeof(line)) < 0)
    {
        return -10002;
    }

    value = strtol(line, &end, 10);
    if ((end == line) || (*end != '\0'))
    {
        return -10003;
    }

    return (int)value;
}

static int sdr_perf_read_stream_block(int socket_fd, rt_uint32_t requested_bytes)
{
    char command[96];
    char line[48];
    rt_uint32_t checksum = g_sdr_iiod_perf_result.checksum;
    rt_uint32_t remaining = requested_bytes;
    rt_bool_t mask_read = RT_FALSE;

    rt_snprintf(command,
                sizeof(command),
                "READBUF cf-ad9361-lpc %lu\r\n",
                (unsigned long)requested_bytes);
    if (sdr_perf_send_all(socket_fd, command, rt_strlen(command)) != 0)
    {
        return -11001;
    }

    while (remaining > 0U)
    {
        char *end = RT_NULL;
        long announced;
        rt_uint32_t chunk_remaining;

        if (sdr_perf_recv_line(socket_fd, line, sizeof(line)) < 0)
        {
            return -11002;
        }
        announced = strtol(line, &end, 10);
        if ((end == line) || (*end != '\0') || (announced <= 0) || ((rt_uint32_t)announced > remaining))
        {
            return (announced < 0) ? (int)announced : -11003;
        }

        if (!mask_read)
        {
            char mask_line[9];
            if (sdr_perf_recv_exact(socket_fd, mask_line, sizeof(mask_line)) != 0)
            {
                return -11004;
            }
            if (mask_line[8] != '\n')
            {
                return -11005;
            }
            mask_line[8] = '\0';
            g_sdr_iiod_perf_result.stream_mask = (rt_uint32_t)strtoul(mask_line, RT_NULL, 16);
            mask_read = RT_TRUE;
        }

        chunk_remaining = (rt_uint32_t)announced;
        while (chunk_remaining > 0U)
        {
            rt_uint32_t to_read = (chunk_remaining > sizeof(s_io_buffer)) ? sizeof(s_io_buffer) : chunk_remaining;
            const rt_uint32_t *words;
            rt_uint32_t word_count;
            rt_uint32_t i;

            if (sdr_perf_recv_exact(socket_fd, s_io_buffer, to_read) != 0)
            {
                return -11006;
            }
            words = (const rt_uint32_t *)s_io_buffer;
            word_count = to_read / sizeof(*words);
            for (i = 0U; i < word_count; i++)
            {
                checksum = ((checksum << 1) | (checksum >> 31)) ^ words[i];
            }
            for (i = word_count * sizeof(*words); i < to_read; i++)
            {
                checksum = ((checksum << 1) | (checksum >> 31)) ^ s_io_buffer[i];
            }
            g_sdr_iiod_perf_result.bytes_received += to_read;
            g_sdr_iiod_perf_result.read_chunks++;
            chunk_remaining -= to_read;
        }

        remaining -= (rt_uint32_t)announced;
    }

    g_sdr_iiod_perf_result.checksum = checksum;
    return 0;
}

static void sdr_perf_thread_entry(void *parameter)
{
    struct netdev *netdev = RT_NULL;
    struct sockaddr_in peer;
    ip_addr_t address;
    rt_uint32_t wait_ms = 0U;
    int socket_fd = -1;
    int socket_timeout = SDR_PERF_SOCKET_TIMEOUT_MS;
    int receive_buffer = SDR_PERF_RECEIVE_BUFFER;
    char line[48];
    char open_command[96];
    char discovered_peer_ip[16];
    rt_tick_t start_tick;
    sdr_perf_candidate_t discovered_candidate;
    const sdr_perf_candidate_t *selected = RT_NULL;
    rt_uint32_t candidate_index;
    int result;

    RT_UNUSED(parameter);
    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_WAIT_LINK;
    sdr_perf_update_report();

    while (wait_ms < 30000U)
    {
        netdev = netdev_default;
        if ((netdev != RT_NULL) && netdev_is_up(netdev) && netdev_is_link_up(netdev))
        {
            break;
        }
        rt_thread_mdelay(100U);
        wait_ms += 100U;
    }
    if ((netdev == RT_NULL) || !netdev_is_link_up(netdev))
    {
        sdr_perf_fail(-12001);
        return;
    }

    g_sdr_iiod_perf_result.phy_result =
        g_rmac_phy0.p_api->linkPartnerAbilityGet(g_rmac_phy0.p_ctrl,
                                                 (uint32_t *)&g_sdr_iiod_perf_result.phy_speed,
                                                 (uint32_t *)&g_sdr_iiod_perf_result.local_pause,
                                                 (uint32_t *)&g_sdr_iiod_perf_result.partner_pause);
    rt_thread_mdelay(500U);

    if ((netdev->ops == RT_NULL) || (netdev->ops->ping == RT_NULL))
    {
        sdr_perf_fail(-12006);
        return;
    }

    for (candidate_index = 0U;
         candidate_index < (sizeof(s_candidates) / sizeof(s_candidates[0]));
         candidate_index++)
    {
        const sdr_perf_candidate_t *candidate = &s_candidates[candidate_index];
        struct netdev_ping_resp response;

        g_sdr_iiod_perf_result.state = SDR_PERF_STATE_CONFIG_IP;
        inet_aton(candidate->local_ip, &address);
        g_sdr_iiod_perf_result.ip_result = netdev_set_ipaddr(netdev, &address);
        inet_aton(SDR_PERF_NETMASK, &address);
        g_sdr_iiod_perf_result.netmask_result = netdev_set_netmask(netdev, &address);
        inet_aton(candidate->gateway, &address);
        g_sdr_iiod_perf_result.gateway_result = netdev_set_gw(netdev, &address);
        g_sdr_iiod_perf_result.candidate_attempts = candidate_index + 1U;
        if ((g_sdr_iiod_perf_result.ip_result != 0) ||
            (g_sdr_iiod_perf_result.netmask_result != 0) ||
            (g_sdr_iiod_perf_result.gateway_result != 0))
        {
            continue;
        }

        rt_thread_mdelay(100U);
        rt_memset(&response, 0, sizeof(response));
        g_sdr_iiod_perf_result.ping_result =
            netdev->ops->ping(netdev,
                              candidate->peer_ip,
                              32U,
                              500U,
                              &response,
                              RT_FALSE);
        g_sdr_iiod_perf_result.ping_time_ms = response.ticks;
        if (g_sdr_iiod_perf_result.ping_result == RT_EOK)
        {
            selected = candidate;
            g_sdr_iiod_perf_result.selected_candidate = candidate_index;
            rt_snprintf((char *)g_sdr_iiod_perf_result.local_ip,
                        sizeof(g_sdr_iiod_perf_result.local_ip),
                        "%s",
                        candidate->local_ip);
            rt_snprintf((char *)g_sdr_iiod_perf_result.peer_ip,
                        sizeof(g_sdr_iiod_perf_result.peer_ip),
                        "%s",
                        candidate->peer_ip);
            break;
        }
    }
    /* The supplied firmware instructions intentionally leave the last octet
     * user-configurable ("ifconfig eth0 192.168.1.xx").  Probe that directly
     * connected /24 with a short timeout before declaring the SDR absent. */
    if (selected == RT_NULL)
    {
        const char *scan_local_ip = "192.168.1.250";
        const char *scan_gateway = "192.168.1.254";

        inet_aton(scan_local_ip, &address);
        g_sdr_iiod_perf_result.ip_result = netdev_set_ipaddr(netdev, &address);
        inet_aton(SDR_PERF_NETMASK, &address);
        g_sdr_iiod_perf_result.netmask_result = netdev_set_netmask(netdev, &address);
        inet_aton(scan_gateway, &address);
        g_sdr_iiod_perf_result.gateway_result = netdev_set_gw(netdev, &address);
        rt_thread_mdelay(100U);

        if ((g_sdr_iiod_perf_result.ip_result == 0) &&
            (g_sdr_iiod_perf_result.netmask_result == 0) &&
            (g_sdr_iiod_perf_result.gateway_result == 0))
        {
            for (candidate_index = 1U; candidate_index < 255U; candidate_index++)
            {
                struct netdev_ping_resp response;

                if (candidate_index == 250U) continue;
                rt_snprintf(discovered_peer_ip,
                            sizeof(discovered_peer_ip),
                            "192.168.1.%lu",
                            (unsigned long)candidate_index);
                rt_memset(&response, 0, sizeof(response));
                g_sdr_iiod_perf_result.candidate_attempts++;
                g_sdr_iiod_perf_result.ping_result =
                    netdev->ops->ping(netdev,
                                      discovered_peer_ip,
                                      32U,
                                      25U,
                                      &response,
                                      RT_FALSE);
                g_sdr_iiod_perf_result.ping_time_ms = response.ticks;
                if (g_sdr_iiod_perf_result.ping_result == RT_EOK)
                {
                    discovered_candidate.local_ip = scan_local_ip;
                    discovered_candidate.gateway = scan_gateway;
                    discovered_candidate.peer_ip = discovered_peer_ip;
                    selected = &discovered_candidate;
                    g_sdr_iiod_perf_result.selected_candidate = 3U;
                    rt_snprintf((char *)g_sdr_iiod_perf_result.local_ip,
                                sizeof(g_sdr_iiod_perf_result.local_ip),
                                "%s",
                                scan_local_ip);
                    rt_snprintf((char *)g_sdr_iiod_perf_result.peer_ip,
                                sizeof(g_sdr_iiod_perf_result.peer_ip),
                                "%s",
                                discovered_peer_ip);
                    break;
                }
            }
        }
    }
    if (selected == RT_NULL)
    {
        sdr_perf_fail(-12007);
        return;
    }

    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_CONNECT;
    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0)
    {
        sdr_perf_fail(-12003);
        return;
    }
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout));
    g_sdr_iiod_perf_result.rcvbuf_setsockopt_result =
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    s_rx_cache_pos = 0U;
    s_rx_cache_len = 0U;

    rt_memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(SDR_PERF_IIOD_PORT);
    peer.sin_addr.s_addr = inet_addr(selected->peer_ip);
    g_sdr_iiod_perf_result.connect_result =
        connect(socket_fd, (const struct sockaddr *)&peer, sizeof(peer));
    if (g_sdr_iiod_perf_result.connect_result != 0)
    {
        closesocket(socket_fd);
        sdr_perf_fail(-12004);
        return;
    }

    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_VERSION;
    if ((sdr_perf_send_all(socket_fd, "VERSION\r\n", 9U) != 0) ||
        (sdr_perf_recv_line(socket_fd, line, sizeof(line)) <= 0))
    {
        closesocket(socket_fd);
        sdr_perf_fail(-12005);
        return;
    }
    rt_snprintf((char *)g_sdr_iiod_perf_result.version,
                sizeof(g_sdr_iiod_perf_result.version),
                "%s",
                line);
    g_sdr_iiod_perf_result.version_ok = 1U;

    g_sdr_iiod_perf_result.timeout_result =
        sdr_perf_exec_integer(socket_fd, "TIMEOUT 10000\r\n");
    if (g_sdr_iiod_perf_result.timeout_result < 0)
    {
        closesocket(socket_fd);
        sdr_perf_fail(g_sdr_iiod_perf_result.timeout_result);
        return;
    }

    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_OPEN;
    rt_snprintf(open_command,
                sizeof(open_command),
                "OPEN cf-ad9361-lpc %lu 00000003\r\n",
                (unsigned long)SDR_PERF_OPEN_SAMPLES);
    g_sdr_iiod_perf_result.open_result =
        sdr_perf_exec_integer(socket_fd, open_command);
    if (g_sdr_iiod_perf_result.open_result < 0)
    {
        closesocket(socket_fd);
        sdr_perf_fail(g_sdr_iiod_perf_result.open_result);
        return;
    }

    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_STREAM;
    g_sdr_iiod_perf_result.recv_calls = 0U;
    g_sdr_iiod_perf_result.cache_fills = 0U;
    start_tick = rt_tick_get();
    while (g_sdr_iiod_perf_result.bytes_received < g_sdr_iiod_perf_result.target_bytes)
    {
        uint64_t remaining = g_sdr_iiod_perf_result.target_bytes -
                             g_sdr_iiod_perf_result.bytes_received;
        rt_uint32_t request_bytes = (remaining > SDR_PERF_REQUEST_BYTES) ?
                                    SDR_PERF_REQUEST_BYTES : (rt_uint32_t)remaining;

        result = sdr_perf_read_stream_block(socket_fd, request_bytes);
        if (result != 0)
        {
            (void)sdr_perf_exec_integer(socket_fd, "CLOSE cf-ad9361-lpc\r\n");
            closesocket(socket_fd);
            sdr_perf_fail(result);
            return;
        }
        g_sdr_iiod_perf_result.read_requests++;
    }

    g_sdr_iiod_perf_result.elapsed_ms = sdr_perf_elapsed_ms(start_tick);
    if (g_sdr_iiod_perf_result.elapsed_ms > 0U)
    {
        g_sdr_iiod_perf_result.payload_mbps_x1000 =
            (rt_uint32_t)((g_sdr_iiod_perf_result.bytes_received * 8ULL) /
                          g_sdr_iiod_perf_result.elapsed_ms);
    }

    (void)sdr_perf_exec_integer(socket_fd, "CLOSE cf-ad9361-lpc\r\n");
    closesocket(socket_fd);
    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_DONE;
    sdr_perf_update_report();
    rt_kprintf("%s\n", (const char *)g_sdr_iiod_perf_report);
}

void sdr_iiod_perf_start(void)
{
    rt_err_t result;

    if (s_sdr_perf_started)
    {
        return;
    }

    rt_memset((void *)&g_sdr_iiod_perf_result, 0, sizeof(g_sdr_iiod_perf_result));
    rt_memset((void *)g_sdr_iiod_perf_report, 0, sizeof(g_sdr_iiod_perf_report));
    g_sdr_iiod_perf_result.magic = SDR_IIOD_PERF_MAGIC;
    g_sdr_iiod_perf_result.schema_version = SDR_IIOD_PERF_SCHEMA_VERSION;
    g_sdr_iiod_perf_result.request_bytes = SDR_PERF_REQUEST_BYTES;
    g_sdr_iiod_perf_result.target_bytes = SDR_PERF_TARGET_BYTES;
    g_sdr_iiod_perf_result.state = SDR_PERF_STATE_INIT;
    sdr_perf_update_report();

    result = rt_thread_init(&s_sdr_perf_thread,
                            "sdrperf",
                            sdr_perf_thread_entry,
                            RT_NULL,
                            s_sdr_perf_stack,
                            sizeof(s_sdr_perf_stack),
                            SDR_PERF_THREAD_PRIORITY,
                            SDR_PERF_THREAD_TICK);
    if (result == RT_EOK)
    {
        s_sdr_perf_started = RT_TRUE;
        result = rt_thread_startup(&s_sdr_perf_thread);
        if (result != RT_EOK)
        {
            sdr_perf_fail(-13000 + result);
        }
    }
    else
    {
        sdr_perf_fail(-14000 + result);
    }
}
