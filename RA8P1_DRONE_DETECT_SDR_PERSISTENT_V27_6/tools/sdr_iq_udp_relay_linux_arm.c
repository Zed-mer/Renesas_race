/*
 * Temporary SDR-side IQSC window relay for ARMv7 Linux.
 *
 * The production capture agent may send to 127.0.0.1:5003 while this process
 * buffers one complete IQSC window and forwards it to RA8P1 at a controlled
 * payload rate.  It uses Linux EABI syscalls directly so the existing
 * arm-none-eabi toolchain can build it without changing the SDR rootfs.
 */

#include "sdr_iq_udp_relay_state.h"
#include "sdr_iq_udp_relay_pacer.h"

typedef __UINT8_TYPE__  u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef __INT32_TYPE__  s32;
typedef __INTPTR_TYPE__ sptr;

#define SYS_EXIT             1
#define SYS_WRITE            4
#define SYS_CLOSE            6
#define SYS_NANOSLEEP        162
#define SYS_CLOCK_GETTIME    263
#define SYS_SOCKET           281
#define SYS_BIND             282
#define SYS_CONNECT          283
#define SYS_SEND             289
#define SYS_RECV             291
#define SYS_RECVFROM         292
#define SYS_SETSOCKOPT       294
#define SYS_SENDMSG          296

#define AF_INET              2
#define SOCK_DGRAM           2
#define IPPROTO_UDP          17
#define SOL_SOCKET           1
#define SO_SNDBUF            7
#define SO_RCVBUF            8
#define SOL_UDP              17
#define UDP_SEGMENT          103
#define CLOCK_MONOTONIC      1

#define IQ_PORT              5003U
#define IQ_MAGIC             0x5149504BU
#define IQ_HEADER_BYTES      32U
#define IQ_DATA_BYTES        1440U
#define IQ_FULL_BYTES        (IQ_HEADER_BYTES + IQ_DATA_BYTES)
#define IQ_FLAG_START        (1U << 3)
#define IQ_FLAG_END          (1U << 4)
#define IQ_EXPECTED_PAYLOAD  2361344ULL
#define IQ_EXPECTED_PACKETS  1642U
#define IQ_MAX_PACKETS       1644U
#define IQ_MAX_DATAGRAM      IQ_FULL_BYTES
#define IQ_MAX_BATCH         64U

struct sockaddr_in32
{
    u16 family;
    u16 port;
    u32 address;
    u8 zero[8];
};

struct timespec32
{
    s32 seconds;
    s32 nanoseconds;
};

struct iovec32
{
    void *base;
    u32 length;
};

struct msghdr32
{
    void *name;
    u32 name_length;
    struct iovec32 *iov;
    u32 iov_count;
    void *control;
    u32 control_length;
    u32 flags;
};

struct udp_segment_control
{
    u32 length;
    s32 level;
    s32 type;
    u16 segment_bytes;
    u16 padding;
};

static u8 g_packets[IQ_MAX_PACKETS][IQ_MAX_DATAGRAM]
    __attribute__((aligned(64)));
static u16 g_packet_lengths[IQ_MAX_PACKETS];

static inline long syscall1(long number, long a0)
{
    register long r0 __asm__("r0") = a0;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r7) : "memory", "cc");
    return r0;
}

static inline long syscall2(long number, long a0, long a1)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r7) : "memory", "cc");
    return r0;
}

static inline long syscall3(long number, long a0, long a1, long a2)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r2),
                      "r" (r7) : "memory", "cc");
    return r0;
}

static inline long syscall4(long number, long a0, long a1, long a2, long a3)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r2),
                      "r" (r3), "r" (r7) : "memory", "cc");
    return r0;
}

static inline long syscall5(long number, long a0, long a1, long a2, long a3,
                            long a4)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r4 __asm__("r4") = a4;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r2),
                      "r" (r3), "r" (r4), "r" (r7) : "memory", "cc");
    return r0;
}

static inline long syscall6(long number, long a0, long a1, long a2, long a3,
                            long a4, long a5)
{
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r4 __asm__("r4") = a4;
    register long r5 __asm__("r5") = a5;
    register long r7 __asm__("r7") = number;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r2),
                      "r" (r3), "r" (r4), "r" (r5), "r" (r7) :
                      "memory", "cc");
    return r0;
}

static u16 host_to_be16(u16 value)
{
    return (u16)((value << 8) | (value >> 8));
}

static u32 get_le32(const u8 *data)
{
    return (u32)data[0] | ((u32)data[1] << 8) |
           ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u32 text_length(const char *text)
{
    u32 length = 0U;
    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

static void write_text(const char *text)
{
    (void)syscall3(SYS_WRITE, 1, (long)(sptr)text, text_length(text));
}

static char *append_text(char *cursor, const char *text)
{
    while (*text != '\0')
    {
        *cursor++ = *text++;
    }
    return cursor;
}

static char *append_u64(char *cursor, u64 value)
{
    char reverse[24];
    u32 count = 0U;
    do
    {
        reverse[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0ULL);
    while (count != 0U)
    {
        *cursor++ = reverse[--count];
    }
    return cursor;
}

static u32 parse_u32(const char *text, u32 fallback, u32 minimum, u32 maximum)
{
    u32 value = 0U;
    u32 digits = 0U;
    if (text == (const char *)0)
    {
        return fallback;
    }
    while ((*text >= '0') && (*text <= '9'))
    {
        const u32 digit = (u32)(*text++ - '0');
        if (value > ((maximum - digit) / 10U))
        {
            return fallback;
        }
        value = value * 10U + digit;
        digits++;
    }
    if ((digits == 0U) || (*text != '\0') || (value < minimum) ||
        (value > maximum))
    {
        return fallback;
    }
    return value;
}

static u64 monotonic_us(void)
{
    struct timespec32 now;
    if (syscall2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC,
                 (long)(sptr)&now) < 0)
    {
        return 0ULL;
    }
    return (u64)(u32)now.seconds * 1000000ULL +
           (u64)(u32)now.nanoseconds / 1000ULL;
}

static void pacer_init(struct payload_pacer *pacer, u32 rate_mbps, u64 now_us)
{
    ra8p1_iqrelay_pacer_init(pacer, rate_mbps, now_us);
}

static void pacer_wait(struct payload_pacer *pacer)
{
    u64 now_us;
    do
    {
        now_us = monotonic_us();
    } while (now_us < pacer->deadline_us);
    (void)ra8p1_iqrelay_pacer_observe(pacer, now_us);
}

static void pacer_commit(struct payload_pacer *pacer, u64 payload_bytes)
{
    ra8p1_iqrelay_pacer_commit(pacer, payload_bytes);
}

static void pacer_finish(const struct payload_pacer *pacer)
{
    while (monotonic_us() < pacer->deadline_us)
    {
    }
}

static long send_datagram(int socket_fd, const void *data, u32 length)
{
    return syscall4(SYS_SEND, socket_fd, (long)(sptr)data, length, 0);
}

static long send_gso(int socket_fd, const void *data, u32 count)
{
    struct iovec32 iov;
    struct msghdr32 message;
    struct udp_segment_control control;
    u8 *bytes = (u8 *)&message;
    u32 index;

    for (index = 0U; index < sizeof(message); index++)
    {
        bytes[index] = 0U;
    }
    iov.base = (void *)data;
    iov.length = count * IQ_FULL_BYTES;
    control.length = 14U;
    control.level = SOL_UDP;
    control.type = UDP_SEGMENT;
    control.segment_bytes = IQ_FULL_BYTES;
    control.padding = 0U;
    message.iov = &iov;
    message.iov_count = 1U;
    message.control = &control;
    message.control_length = sizeof(control);
    return syscall3(SYS_SENDMSG, socket_fd, (long)(sptr)&message, 0);
}

static int forward_window(int output_fd, u32 packet_count, u32 rate_mbps,
                          u32 batch_size)
{
    struct payload_pacer pacer;
    u64 payload_bytes = 0ULL;
    u64 started_us;
    u64 elapsed_us;
    u32 session_id;
    u32 packet_index;
    u32 gso_batches = 0U;
    u32 gso_fallbacks = 0U;
    u32 send_errors = 0U;
    char line[512];
    char *cursor;

    if ((packet_count < 3U) ||
        ((get_le32(&g_packets[0][12]) & IQ_FLAG_START) == 0U) ||
        ((get_le32(&g_packets[packet_count - 1U][12]) & IQ_FLAG_END) == 0U))
    {
        return 0;
    }
    session_id = get_le32(&g_packets[0][24]);
    started_us = monotonic_us();
    pacer_init(&pacer, rate_mbps, started_us);
    if (send_datagram(output_fd, g_packets[0], g_packet_lengths[0]) !=
        (long)g_packet_lengths[0])
    {
        return 0;
    }

    packet_index = 1U;
    while (packet_index + 1U < packet_count)
    {
        u32 group = 0U;
        u64 group_payload = 0ULL;
        while ((group < batch_size) &&
               (packet_index + group + 1U < packet_count) &&
               (g_packet_lengths[packet_index + group] == IQ_FULL_BYTES))
        {
            group_payload += get_le32(&g_packets[packet_index + group][8]);
            group++;
        }
        pacer_wait(&pacer);
        if (group >= 2U)
        {
            const long sent = send_gso(output_fd, g_packets[packet_index], group);
            if (sent == (long)(group * IQ_FULL_BYTES))
            {
                gso_batches++;
            }
            else
            {
                u32 item;
                gso_fallbacks++;
                for (item = 0U; item < group; item++)
                {
                    if (send_datagram(output_fd, g_packets[packet_index + item],
                                      g_packet_lengths[packet_index + item]) !=
                        (long)g_packet_lengths[packet_index + item])
                    {
                        send_errors++;
                        break;
                    }
                }
            }
            packet_index += group;
        }
        else
        {
            const u32 data_bytes = get_le32(&g_packets[packet_index][8]);
            group_payload = data_bytes;
            if (send_datagram(output_fd, g_packets[packet_index],
                              g_packet_lengths[packet_index]) !=
                (long)g_packet_lengths[packet_index])
            {
                send_errors++;
            }
            packet_index++;
        }
        payload_bytes += group_payload;
        pacer_commit(&pacer, group_payload);
        if (send_errors != 0U)
        {
            break;
        }
    }
    pacer_finish(&pacer);
    if ((send_errors == 0U) &&
        (send_datagram(output_fd, g_packets[packet_count - 1U],
                       g_packet_lengths[packet_count - 1U]) !=
         (long)g_packet_lengths[packet_count - 1U]))
    {
        send_errors++;
    }
    elapsed_us = monotonic_us() - started_us;

    cursor = append_text(line, "IQRELAY window session=");
    cursor = append_u64(cursor, session_id);
    cursor = append_text(cursor, " packets=");
    cursor = append_u64(cursor, packet_count);
    cursor = append_text(cursor, " payload_bytes=");
    cursor = append_u64(cursor, payload_bytes);
    cursor = append_text(cursor, " send_elapsed_us=");
    cursor = append_u64(cursor, elapsed_us);
    cursor = append_text(cursor, " actual_mbps_x1000=");
    cursor = append_u64(cursor, (elapsed_us != 0ULL) ?
                        (payload_bytes * 8000ULL) / elapsed_us : 0ULL);
    cursor = append_text(cursor, " target_mbps=");
    cursor = append_u64(cursor, rate_mbps);
    cursor = append_text(cursor, " batch=");
    cursor = append_u64(cursor, batch_size);
    cursor = append_text(cursor, " gso_batches=");
    cursor = append_u64(cursor, gso_batches);
    cursor = append_text(cursor, " gso_fallbacks=");
    cursor = append_u64(cursor, gso_fallbacks);
    cursor = append_text(cursor, " pacing_rebases=");
    cursor = append_u64(cursor, pacer.late_rebases);
    cursor = append_text(cursor, " pacing_max_late_us=");
    cursor = append_u64(cursor, pacer.max_late_us);
    cursor = append_text(cursor, " send_errors=");
    cursor = append_u64(cursor, send_errors);
    *cursor++ = '\n';
    *cursor = '\0';
    write_text(line);

    return (send_errors == 0U) &&
           (payload_bytes == IQ_EXPECTED_PAYLOAD) &&
           (packet_count == IQ_EXPECTED_PACKETS);
}

__attribute__((used, noinline))
static int relay_main(u32 *stack)
{
    const u32 argc = stack[0];
    char **argv = (char **)(sptr)&stack[1];
    const u32 rate_mbps = parse_u32((argc > 1U) ? argv[1] : (char *)0,
                                    850U, 1U, 940U);
    const u32 batch_size = parse_u32((argc > 2U) ? argv[2] : (char *)0,
                                     16U, 1U, IQ_MAX_BATCH);
    struct sockaddr_in32 local;
    struct sockaddr_in32 peer;
    u32 socket_buffer = 8U * 1024U * 1024U;
    ra8p1_iqrelay_tracker_t tracker;
    int input_fd;
    int output_fd;
    char ready[192];
    char *cursor;

    ra8p1_iqrelay_tracker_reset(&tracker);

    input_fd = (int)syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    output_fd = (int)syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((input_fd < 0) || (output_fd < 0))
    {
        write_text("IQRELAY fatal=socket\n");
        return 10;
    }
    (void)syscall5(SYS_SETSOCKOPT, input_fd, SOL_SOCKET, SO_RCVBUF,
                   (long)(sptr)&socket_buffer, sizeof(socket_buffer));
    (void)syscall5(SYS_SETSOCKOPT, output_fd, SOL_SOCKET, SO_SNDBUF,
                   (long)(sptr)&socket_buffer, sizeof(socket_buffer));

    local.family = AF_INET;
    local.port = host_to_be16(IQ_PORT);
    local.address = 0U;
    for (u32 index = 0U; index < sizeof(local.zero); index++)
    {
        local.zero[index] = 0U;
    }
    if (syscall3(SYS_BIND, input_fd, (long)(sptr)&local, sizeof(local)) < 0)
    {
        write_text("IQRELAY fatal=bind_5003\n");
        return 11;
    }

    peer.family = AF_INET;
    peer.port = host_to_be16(IQ_PORT);
    /* 192.168.31.20 represented as bytes in little-endian ARM memory. */
    peer.address = 0x141FA8C0U;
    for (u32 index = 0U; index < sizeof(peer.zero); index++)
    {
        peer.zero[index] = 0U;
    }
    if (syscall3(SYS_CONNECT, output_fd, (long)(sptr)&peer, sizeof(peer)) < 0)
    {
        write_text("IQRELAY fatal=connect_ra8p1\n");
        return 12;
    }

    cursor = append_text(ready, "IQRELAY ready listen=0.0.0.0:5003 dest=192.168.31.20:5003 target_mbps=");
    cursor = append_u64(cursor, rate_mbps);
    cursor = append_text(cursor, " batch=");
    cursor = append_u64(cursor, batch_size);
    *cursor++ = '\n';
    *cursor = '\0';
    write_text(ready);

    for (;;)
    {
        struct sockaddr_in32 source;
        u32 source_length = sizeof(source);
        u32 receive_slot;
        long received;
        u32 flags;
        u32 sequence;
        ra8p1_iqrelay_accept_t accept;

        if (tracker.packet_count >= IQ_MAX_PACKETS)
        {
            ra8p1_iqrelay_tracker_reset(&tracker);
        }
        receive_slot = tracker.packet_count;
        received = syscall6(SYS_RECVFROM, input_fd,
                            (long)(sptr)g_packets[receive_slot],
                            IQ_MAX_DATAGRAM, 0, (long)(sptr)&source,
                            (long)(sptr)&source_length);
        if (received < 0)
        {
            if (received == -4)
            {
                continue;
            }
            write_text("IQRELAY warning=recv_failed\n");
            continue;
        }
        if ((received < (long)IQ_HEADER_BYTES) ||
            (source_length < sizeof(source)) || (source.family != AF_INET) ||
            (get_le32(g_packets[receive_slot]) != IQ_MAGIC) ||
            (get_le32(&g_packets[receive_slot][8]) + IQ_HEADER_BYTES !=
             (u32)received))
        {
            continue;
        }
        flags = get_le32(&g_packets[receive_slot][12]);
        sequence = get_le32(&g_packets[receive_slot][4]);
        accept = ra8p1_iqrelay_tracker_classify(&tracker, flags,
                                                IQ_FLAG_START, sequence,
                                                source.address, source.port);
        if (accept == RA8P1_IQRELAY_DROP)
        {
            continue;
        }
        if (accept == RA8P1_IQRELAY_SEQUENCE_RESET)
        {
            write_text("IQRELAY warning=sequence_reset\n");
            continue;
        }
        if (accept == RA8P1_IQRELAY_RESTART_ACCEPT)
        {
            ra8p1_iqrelay_copy_datagram(g_packets[0],
                                        g_packets[receive_slot],
                                        (u32)received);
            receive_slot = 0U;
        }
        g_packet_lengths[tracker.packet_count] = (u16)received;
        ra8p1_iqrelay_tracker_commit(&tracker);
        if ((flags & IQ_FLAG_END) != 0U)
        {
            (void)forward_window(output_fd, tracker.packet_count, rate_mbps,
                                 batch_size);
            ra8p1_iqrelay_tracker_reset(&tracker);
        }
    }
}

__attribute__((naked, noreturn, section(".text.start")))
void _start(void)
{
    __asm__ volatile (
        "mov r0, sp\n"
        "bl relay_main\n"
        "mov r7, #1\n"
        "svc 0\n"
        "b .\n");
}
