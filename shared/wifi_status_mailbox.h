#ifndef RA8P1_WIFI_STATUS_MAILBOX_H
#define RA8P1_WIFI_STATUS_MAILBOX_H

#include <stddef.h>
#include <stdint.h>

#include "resource_layout.h"

#define RA8P1_WIFI_STATUS_MAGIC        (0x49464957UL) /* WIFI */
#define RA8P1_WIFI_STATUS_VERSION      (1U)
#define RA8P1_WIFI_SSID_MAX_BYTES      (32U)

typedef enum e_ra8p1_wifi_connection_state
{
    RA8P1_WIFI_DISCONNECTED = 0,
    RA8P1_WIFI_CONNECTING = 1,
    RA8P1_WIFI_CONNECTED = 2
} ra8p1_wifi_connection_state_t;

/* CPU0 owns this complete cache line. CPU1 consumes it through the even
 * seqlock and never writes it back. Passwords are deliberately excluded. */
typedef struct st_ra8p1_wifi_status_mailbox
{
    volatile uint32_t begin_sequence;
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t cpu0_boot_epoch;
    uint32_t generation;
    uint32_t connection_state;
    char ssid[RA8P1_WIFI_SSID_MAX_BYTES + 1U];
    uint8_t reserved[3];
    volatile uint32_t end_sequence;
} ra8p1_wifi_status_mailbox_t;

#define RA8P1_WIFI_STATUS_MAILBOX \
    ((volatile ra8p1_wifi_status_mailbox_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_WIFI_STATUS_OFFSET))

typedef char ra8p1_wifi_status_mailbox_must_be_64[
    (sizeof(ra8p1_wifi_status_mailbox_t) == RA8P1_WIFI_STATUS_BYTES) ? 1 : -1];
typedef char ra8p1_wifi_status_must_be_cache_line_aligned[
    (((RA8P1_SHARED_RAM_BASE + RA8P1_WIFI_STATUS_OFFSET) & 31U) == 0U) ? 1 : -1];
typedef char ra8p1_wifi_status_must_follow_activity[
    (RA8P1_WIFI_STATUS_OFFSET >=
     (RA8P1_ACTIVITY_CONTROL_OFFSET + RA8P1_ACTIVITY_CONTROL_BYTES)) ? 1 : -1];
typedef char ra8p1_wifi_status_must_precede_display[
    ((RA8P1_WIFI_STATUS_OFFSET + RA8P1_WIFI_STATUS_BYTES) <=
     RA8P1_DISPLAY_STREAM_OFFSET) ? 1 : -1];
typedef char ra8p1_wifi_status_end_sequence_must_be_last[
    (offsetof(ra8p1_wifi_status_mailbox_t, end_sequence) == 60U) ? 1 : -1];

#endif
