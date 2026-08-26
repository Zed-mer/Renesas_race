/* ARM Linux IQ UDP sender for the 7020 AD936X SDR. */

#include <stdint.h>

#define SYS_EXIT          1
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_CLOSE         6
#define SYS_SCHED_SETAFFINITY 241
#define SYS_CLOCK_GETTIME 263
#define SYS_SOCKET        281
#define SYS_CONNECT       283
#define SYS_SETSOCKOPT    294
#define SYS_SENDMSG       296
#define SYS_SENDMMSG      374

#define AF_INET         2
#define SOCK_DGRAM      2
#define IPPROTO_UDP     17
#define SOL_SOCKET      1
#define SOL_UDP         17
#define SO_SNDBUF       7
#define UDP_SEGMENT     103
#define CLOCK_MONOTONIC 1

#define RA_ADDR_HOST    0xC0A81F14U
#define IQ_PORT         5003U
#define IQ_MAGIC        0x5149504BU
#define IQ_PACKET_SIZE  1472U
#define IQ_HEADER_SIZE  32U
#define IQ_DATA_SIZE    (IQ_PACKET_SIZE - IQ_HEADER_SIZE)
#define IQ_DEFAULT_PAYLOAD_MBPS 890U
#ifndef IQ_BATCH_SIZE
#define IQ_BATCH_SIZE   32U
#endif

struct linux_timespec
{
    int32_t sec;
    int32_t nsec;
};

struct linux_sockaddr_in
{
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

struct linux_iovec
{
    void *base;
    uint32_t length;
};

struct linux_msghdr
{
    void *name;
    uint32_t name_length;
    struct linux_iovec *iov;
    uint32_t iov_length;
    void *control;
    uint32_t control_length;
    uint32_t flags;
};

struct linux_mmsghdr
{
    struct linux_msghdr header;
    uint32_t message_length;
};

struct linux_cmsghdr
{
    uint32_t length;
    int32_t level;
    int32_t type;
};

static uint8_t s_packets[IQ_BATCH_SIZE][IQ_PACKET_SIZE];
static struct linux_iovec s_iov[IQ_BATCH_SIZE];
static struct linux_mmsghdr s_messages[IQ_BATCH_SIZE];
static struct linux_iovec s_gso_iov;
static struct linux_msghdr s_gso_message;
static uint32_t s_gso_control[4];

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

static long syscall4(long number, long arg0, long arg1, long arg2, long arg3)
{
    register long r0 __asm__("r0") = arg0;
    register long r1 __asm__("r1") = arg1;
    register long r2 __asm__("r2") = arg2;
    register long r3 __asm__("r3") = arg3;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r7) : "memory");
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

static uint32_t now_us(void)
{
    struct linux_timespec time;
    if (syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&time, 0) < 0)
    {
        return 0U;
    }
    return ((uint32_t)time.sec * 1000000U) + ((uint32_t)time.nsec / 1000U);
}

static long read_exact(int fd, uint8_t *data, uint32_t length)
{
    uint32_t received = 0U;
    while (received < length)
    {
        long result = syscall3(SYS_READ, fd, (long)&data[received], length - received);
        if (result <= 0)
        {
            return result;
        }
        received += (uint32_t)result;
    }
    return received;
}

static void set_u32(uint8_t *data, uint32_t offset, uint32_t value)
{
    *(uint32_t *)&data[offset] = value;
}

static void set_u64(uint8_t *data, uint32_t offset, uint64_t value)
{
    *(uint64_t *)&data[offset] = value;
}

static int fill_batch(uint32_t sequence, uint32_t count, int stdin_mode)
{
    uint32_t batch_time_us = now_us();
    uint32_t i;
    for (i = 0U; i < count; i++)
    {
        uint8_t *packet = s_packets[i];
        if (stdin_mode)
        {
            if (read_exact(0, &packet[IQ_HEADER_SIZE], IQ_DATA_SIZE) != IQ_DATA_SIZE)
            {
                return (int)i;
            }
        }
        else
        {
            set_u32(packet, IQ_HEADER_SIZE, sequence + i);
        }

        set_u32(packet, 0U, IQ_MAGIC);
        set_u32(packet, 4U, sequence + i);
        set_u32(packet, 8U, IQ_DATA_SIZE);
        set_u32(packet, 12U, stdin_mode ? 0U : 1U);
        set_u64(packet, 16U, (uint64_t)(sequence + i) * (IQ_DATA_SIZE / 4U));
        set_u32(packet, 24U, batch_time_us);
        set_u32(packet, 28U, 0U);
        s_messages[i].message_length = 0U;
    }
    return (int)count;
}

static __attribute__((used, noinline)) int program_main(uint32_t *stack)
{
    int argc = (int)stack[0];
    char **argv = (char **)&stack[1];
    struct linux_sockaddr_in peer = {0};
    uint32_t target_mbps;
    uint32_t duration_s;
    uint32_t sequence = 0U;
    uint64_t payload_bytes = 0U;
    uint64_t iq_bytes = 0U;
    uint32_t start_us;
    uint32_t elapsed_us = 0U;
    uint32_t i;
    uint32_t cpu_mask = 1U << 1;
    int send_buffer = 4 * 1024 * 1024;
    int affinity_ok;
    int gso_enabled = 1;
    int socket_fd;
    int stdin_mode;

    if ((argc < 2) || (argc > 4) ||
        ((argv[1][0] != 's') && (argv[1][0] != 'p') && (argv[1][0] != 'i')))
    {
        print_text("usage: sdr_iq_udp synthetic|pipe [payload_mbps] [seconds]\n");
        return 2;
    }
    stdin_mode = (argv[1][0] != 's');
    target_mbps = (argc >= 3) ? parse_u32(argv[2]) : IQ_DEFAULT_PAYLOAD_MBPS;
    duration_s = (argc >= 4) ? parse_u32(argv[3]) : 10U;

    affinity_ok = (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_mask), (long)&cpu_mask) == 0);

    socket_fd = (int)syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return 3;
    }
    (void)syscall5(SYS_SETSOCKOPT, socket_fd, SOL_SOCKET, SO_SNDBUF,
                   (long)&send_buffer, sizeof(send_buffer));
    peer.family = AF_INET;
    peer.port = swap16(IQ_PORT);
    peer.address = swap32(RA_ADDR_HOST);
    if (syscall3(SYS_CONNECT, socket_fd, (long)&peer, sizeof(peer)) < 0)
    {
        (void)syscall1(SYS_CLOSE, socket_fd);
        return 4;
    }

    for (i = 0U; i < IQ_BATCH_SIZE; i++)
    {
        s_iov[i].base = s_packets[i];
        s_iov[i].length = IQ_PACKET_SIZE;
        s_messages[i].header.name = 0;
        s_messages[i].header.name_length = 0U;
        s_messages[i].header.iov = &s_iov[i];
        s_messages[i].header.iov_length = 1U;
        s_messages[i].header.control = 0;
        s_messages[i].header.control_length = 0U;
        s_messages[i].header.flags = 0U;
    }

    s_gso_iov.base = &s_packets[0][0];
    s_gso_message.iov = &s_gso_iov;
    s_gso_message.iov_length = 1U;
    s_gso_message.control = s_gso_control;
    s_gso_message.control_length = sizeof(s_gso_control);
    ((struct linux_cmsghdr *)s_gso_control)->length = sizeof(struct linux_cmsghdr) + sizeof(uint16_t);
    ((struct linux_cmsghdr *)s_gso_control)->level = SOL_UDP;
    ((struct linux_cmsghdr *)s_gso_control)->type = UDP_SEGMENT;
    *(uint16_t *)((uint8_t *)s_gso_control + sizeof(struct linux_cmsghdr)) = IQ_PACKET_SIZE;

    start_us = now_us();
    while ((elapsed_us < (duration_s * 1000000U)) || stdin_mode)
    {
        int batch_count = fill_batch(sequence, IQ_BATCH_SIZE, stdin_mode);
        uint32_t sent_count = 0U;
        if (batch_count <= 0)
        {
            break;
        }
        if (gso_enabled)
        {
            long sent_bytes;
            s_gso_iov.length = (uint32_t)batch_count * IQ_PACKET_SIZE;
            sent_bytes = syscall3(SYS_SENDMSG, socket_fd, (long)&s_gso_message, 0);
            if ((sent_bytes > 0) && ((sent_bytes % IQ_PACKET_SIZE) == 0))
            {
                sent_count = (uint32_t)sent_bytes / IQ_PACKET_SIZE;
            }
            else
            {
                gso_enabled = 0;
            }
        }
        while (sent_count < (uint32_t)batch_count)
        {
            long sent = syscall4(SYS_SENDMMSG,
                                 socket_fd,
                                 (long)&s_messages[sent_count],
                                 (uint32_t)batch_count - sent_count,
                                 0);
            if (sent <= 0)
            {
                (void)syscall1(SYS_CLOSE, socket_fd);
                return 5;
            }
            sent_count += (uint32_t)sent;
        }

        sequence += sent_count;
        payload_bytes += (uint64_t)sent_count * IQ_PACKET_SIZE;
        iq_bytes += (uint64_t)sent_count * IQ_DATA_SIZE;
        if (target_mbps > 0U)
        {
            uint32_t target_us = (uint32_t)((payload_bytes * 8U) / target_mbps);
            while ((uint32_t)(now_us() - start_us) < target_us)
            {
            }
        }
        elapsed_us = now_us() - start_us;
        if (stdin_mode && (duration_s > 0U) && (elapsed_us >= duration_s * 1000000U))
        {
            break;
        }
    }

    print_text("SDR_IQ_UDP packets=");
    print_u64(sequence);
    print_text(" payload_bytes=");
    print_u64(payload_bytes);
    print_text(" iq_bytes=");
    print_u64(iq_bytes);
    print_text(" elapsed_us=");
    print_u64(elapsed_us);
    print_text(" payload_mbps_x1000=");
    print_u64(elapsed_us ? ((payload_bytes * 8000U) / elapsed_us) : 0U);
    print_text(" iq_mbps_x1000=");
    print_u64(elapsed_us ? ((iq_bytes * 8000U) / elapsed_us) : 0U);
    print_text(" mode=");
    print_text(stdin_mode ? "pipe" : "synthetic");
    print_text(" transport=");
    print_text(gso_enabled ? "udp_gso" : "sendmmsg");
    print_text(" cpu=");
    print_text(affinity_ok ? "1\n" : "any\n");

    (void)syscall1(SYS_CLOSE, socket_fd);
    return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov r0, sp\n"
        "bl program_main\n"
        "mov r7, #1\n"
        "svc 0\n");
}
