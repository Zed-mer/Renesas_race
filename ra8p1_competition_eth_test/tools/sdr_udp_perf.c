/* Minimal ARM Linux UDP peer for the RA8P1 eth_perf service. */

#include <stdint.h>

#define SYS_EXIT          1
#define SYS_WRITE         4
#define SYS_CLOSE         6
#define SYS_CLOCK_GETTIME 263
#define SYS_SOCKET        281
#define SYS_BIND          282
#define SYS_SENDTO        290
#define SYS_RECVFROM      292
#define SYS_SETSOCKOPT    294

#define AF_INET       2
#define SOCK_DGRAM    2
#define IPPROTO_UDP   17
#define SOL_SOCKET    1
#define SO_RCVTIMEO   20
#define SO_RCVBUF     8
#define CLOCK_MONOTONIC 1

#define RA_ADDR_HOST  0xC0A81F14U
#define PERF_SOCKET_PORT 5001U
#define PERF_RAW_RX_PORT 5002U
#define PERF_MS       10000U
#define PAYLOAD_SIZE  1472U
#define PACKET_MAGIC  0x46505445U

struct linux_timespec
{
    int32_t sec;
    int32_t nsec;
};

struct linux_timeval
{
    int32_t sec;
    int32_t usec;
};

struct linux_sockaddr_in
{
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

static long syscall1(long number, long arg0)
{
    register long r0 __asm__("r0") = arg0;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}

static long syscall3(long number, long arg0, long arg1, long arg2)
{
    register long r0 __asm__("r0") = arg0;
    register long r1 __asm__("r1") = arg1;
    register long r2 __asm__("r2") = arg2;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static long syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4)
{
    register long r0 __asm__("r0") = arg0;
    register long r1 __asm__("r1") = arg1;
    register long r2 __asm__("r2") = arg2;
    register long r3 __asm__("r3") = arg3;
    register long r4 __asm__("r4") = arg4;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7) : "memory");
    return r0;
}

static long syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5)
{
    register long r0 __asm__("r0") = arg0;
    register long r1 __asm__("r1") = arg1;
    register long r2 __asm__("r2") = arg2;
    register long r3 __asm__("r3") = arg3;
    register long r4 __asm__("r4") = arg4;
    register long r5 __asm__("r5") = arg5;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7) : "memory");
    return r0;
}

static uint16_t swap16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t swap32(uint32_t value)
{
    return __builtin_bswap32(value);
}

static uint32_t text_length(const char *text)
{
    uint32_t length = 0U;
    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

static void print_text(const char *text)
{
    (void)syscall3(SYS_WRITE, 1, (long)text, text_length(text));
}

static void print_u64(uint64_t value)
{
    char digits[24];
    uint32_t used = 0U;
    do
    {
        digits[used++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    while (used > 0U)
    {
        used--;
        (void)syscall3(SYS_WRITE, 1, (long)&digits[used], 1);
    }
}

static void print_result(const char *direction,
                         uint64_t packets,
                         uint64_t bytes,
                         uint32_t elapsed_ms,
                         const char *peer_report,
                         uint32_t peer_length)
{
    uint64_t mbps_x1000 = elapsed_ms ? ((bytes * 8U) / elapsed_ms) : 0U;

    print_text("SDR_UDP_PERF direction=");
    print_text(direction);
    print_text(" packets=");
    print_u64(packets);
    print_text(" bytes=");
    print_u64(bytes);
    print_text(" elapsed_ms=");
    print_u64(elapsed_ms);
    print_text(" mbps_x1000=");
    print_u64(mbps_x1000);
    print_text("\nRA_REPORT ");
    (void)syscall3(SYS_WRITE, 1, (long)peer_report, peer_length);
    print_text("\n");
}

static uint32_t now_ms(void)
{
    struct linux_timespec time;
    if (syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&time, 0) < 0)
    {
        return 0U;
    }
    return ((uint32_t)time.sec * 1000U) + ((uint32_t)time.nsec / 1000000U);
}

static uint32_t now_us(void)
{
    struct linux_timespec time;
    if (syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&time, 0) < 0)
    {
        return 0U;
    }
    return ((uint32_t)time.sec * 1000000U) + ((uint32_t)time.nsec / 1000U);
}

static uint32_t parse_u32(const char *text)
{
    uint32_t value = 0U;
    while ((*text >= '0') && (*text <= '9'))
    {
        value = (value * 10U) + (uint32_t)(*text - '0');
        text++;
    }
    return value;
}

static int begins_with(const uint8_t *data, long length, const char *prefix)
{
    uint32_t i;
    uint32_t prefix_length = text_length(prefix);

    if (length < (long)prefix_length)
    {
        return 0;
    }
    for (i = 0U; i < prefix_length; i++)
    {
        if (data[i] != (uint8_t)prefix[i])
        {
            return 0;
        }
    }
    return 1;
}

static long send_text(int socket_fd, const struct linux_sockaddr_in *peer, const char *text)
{
    return syscall6(SYS_SENDTO,
                    socket_fd,
                    (long)text,
                    text_length(text),
                    0,
                    (long)peer,
                    sizeof(*peer));
}

static long receive_packet(int socket_fd, uint8_t *buffer, uint32_t capacity)
{
    return syscall6(SYS_RECVFROM, socket_fd, (long)buffer, capacity, 0, 0, 0);
}

static int run_sdr_to_ra(int socket_fd,
                         const struct linux_sockaddr_in *peer,
                         uint32_t target_mbps)
{
    static uint8_t payload[PAYLOAD_SIZE];
    uint8_t response[256];
    uint64_t packets = 0U;
    uint64_t bytes = 0U;
    uint32_t start_ms;
    uint32_t start_us;
    uint32_t elapsed_ms;
    uint32_t stop_start_ms;
    uint32_t attempt;
    long received;
    int ready = 0;

    if (send_text(socket_fd, peer, "PERF RXSTART") < 0)
    {
        return 20;
    }
    for (attempt = 0U; attempt < 16U; attempt++)
    {
        received = receive_packet(socket_fd, response, sizeof(response));
        if (received < 0)
        {
            break;
        }
        if (begins_with(response, received, "PERF RXREADY"))
        {
            ready = 1;
            break;
        }
    }
    if (!ready)
    {
        return 21;
    }

    *(uint32_t *)&payload[0] = PACKET_MAGIC;
    start_ms = now_ms();
    start_us = now_us();
    do
    {
        long sent;
        *(uint32_t *)&payload[4] = (uint32_t)packets;
        sent = syscall6(SYS_SENDTO,
                        socket_fd,
                        (long)payload,
                        sizeof(payload),
                        0,
                        (long)peer,
                        sizeof(*peer));
        if (sent == (long)sizeof(payload))
        {
            packets++;
            bytes += sizeof(payload);
        }
        if (target_mbps > 0U)
        {
            uint32_t target_elapsed_us = (uint32_t)((bytes * 8U) / target_mbps);
            while ((uint32_t)(now_us() - start_us) < target_elapsed_us)
            {
            }
        }
        elapsed_ms = now_ms() - start_ms;
    } while (elapsed_ms < PERF_MS);

    stop_start_ms = now_ms();
    while ((now_ms() - stop_start_ms) < 200U)
    {
    }
    for (attempt = 0U; attempt < 8U; attempt++)
    {
        if (send_text(socket_fd, peer, "PERF RXSTOP") < 0)
        {
            return 22;
        }
    }

    for (attempt = 0U; attempt < 16U; attempt++)
    {
        received = receive_packet(socket_fd, response, sizeof(response));
        if (begins_with(response, received, "PERF RXDONE"))
        {
            print_result("SDR_TO_RA", packets, bytes, elapsed_ms, (const char *)response, (uint32_t)received);
            return 0;
        }
    }
    return 23;
}

static int run_ra_to_sdr(int socket_fd, const struct linux_sockaddr_in *peer)
{
    uint8_t response[2048];
    uint8_t report[256];
    uint64_t packets = 0U;
    uint64_t bytes = 0U;
    uint32_t start_ms;
    uint32_t elapsed_ms;
    uint32_t report_length = 0U;

    if (send_text(socket_fd, peer, "PERF TX 10000 1472") < 0)
    {
        return 30;
    }
    if (!begins_with(response,
                     receive_packet(socket_fd, response, sizeof(response)),
                     "PERF TXREADY"))
    {
        return 31;
    }

    start_ms = now_ms();
    for (;;)
    {
        long received = receive_packet(socket_fd, response, sizeof(response));
        if (received < 0)
        {
            return 32;
        }
        if (begins_with(response, received, "PERF TXDONE"))
        {
            uint32_t i;
            report_length = (received > (long)sizeof(report)) ? sizeof(report) : (uint32_t)received;
            for (i = 0U; i < report_length; i++)
            {
                report[i] = response[i];
            }
            break;
        }
        packets++;
        bytes += (uint32_t)received;
    }
    elapsed_ms = now_ms() - start_ms;
    print_result("RA_TO_SDR", packets, bytes, elapsed_ms, (const char *)report, report_length);
    return 0;
}

static __attribute__((used, noinline)) int program_main(uint32_t *stack)
{
    int argc = (int)stack[0];
    char **argv = (char **)&stack[1];
    struct linux_sockaddr_in local = {0};
    struct linux_sockaddr_in peer = {0};
    struct linux_timeval timeout = {2, 0};
    int receive_buffer = 4 * 1024 * 1024;
    int socket_fd;
    int result;

    if ((argc < 2) || (argc > 3) || ((argv[1][0] != 't') && (argv[1][0] != 'r')))
    {
        print_text("usage: sdr_udp_perf tx [payload_mbps]|rx\n");
        return 2;
    }

    socket_fd = (int)syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return 3;
    }
    (void)syscall5(SYS_SETSOCKOPT, socket_fd, SOL_SOCKET, SO_RCVTIMEO, (long)&timeout, sizeof(timeout));
    (void)syscall5(SYS_SETSOCKOPT, socket_fd, SOL_SOCKET, SO_RCVBUF, (long)&receive_buffer, sizeof(receive_buffer));

    local.family = AF_INET;
    local.port = 0U;
    local.address = 0U;
    if (syscall3(SYS_BIND, socket_fd, (long)&local, sizeof(local)) < 0)
    {
        (void)syscall1(SYS_CLOSE, socket_fd);
        return 4;
    }

    peer.family = AF_INET;
    peer.port = swap16((argv[1][0] == 't') ? PERF_RAW_RX_PORT : PERF_SOCKET_PORT);
    peer.address = swap32(RA_ADDR_HOST);
    result = (argv[1][0] == 't')
                 ? run_sdr_to_ra(socket_fd, &peer, (argc == 3) ? parse_u32(argv[2]) : 0U)
                 : run_ra_to_sdr(socket_fd, &peer);
    (void)syscall1(SYS_CLOSE, socket_fd);
    return result;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov r0, sp\n"
        "bl program_main\n"
        "mov r7, #1\n"
        "svc 0\n");
}
