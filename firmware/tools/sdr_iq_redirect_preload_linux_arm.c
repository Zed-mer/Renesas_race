/*
 * Temporary LD_PRELOAD hook for the SDR /tmp performance relay.
 *
 * Keep the capture agent's configured peer at the real RA8P1 address so its
 * control-source validation remains intact.  Only redirect the connected IQ
 * UDP/5003 socket to the loopback relay.  No SDR firmware file is changed.
 */

typedef __UINT8_TYPE__  u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __INTPTR_TYPE__ sptr;

#define SYS_CONNECT       283
#define AF_INET           2
#define IQ_PORT           5003U
#define RA8P1_ADDRESS_LE  0x141FA8C0U
#define LOOPBACK_LE       0x0100007FU

struct sockaddr_in32
{
    u16 family;
    u16 port;
    u32 address;
    u8 zero[8];
};

static inline long raw_connect(int socket_fd, const void *address,
                               u32 address_length)
{
    register long r0 __asm__("r0") = socket_fd;
    register long r1 __asm__("r1") = (long)(sptr)address;
    register long r2 __asm__("r2") = address_length;
    register long r7 __asm__("r7") = SYS_CONNECT;
    __asm__ volatile ("svc 0" : "+r" (r0) : "r" (r1), "r" (r2),
                      "r" (r7) : "memory", "cc");
    return r0;
}

static u16 host_to_be16(u16 value)
{
    return (u16)((value << 8) | (value >> 8));
}

int connect(int socket_fd, const void *address, u32 address_length)
{
    if ((address != (const void *)0) &&
        (address_length >= sizeof(struct sockaddr_in32)))
    {
        const struct sockaddr_in32 *source =
            (const struct sockaddr_in32 *)address;
        if ((source->family == AF_INET) &&
            (source->port == host_to_be16(IQ_PORT)) &&
            (source->address == RA8P1_ADDRESS_LE))
        {
            struct sockaddr_in32 redirected = *source;
            redirected.address = LOOPBACK_LE;
            return (int)raw_connect(socket_fd, &redirected,
                                    sizeof(redirected));
        }
    }
    return (int)raw_connect(socket_fd, address, address_length);
}

