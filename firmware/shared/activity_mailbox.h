#ifndef RA8P1_ACTIVITY_MAILBOX_H
#define RA8P1_ACTIVITY_MAILBOX_H

#include <stddef.h>
#include <stdint.h>

#include "resource_layout.h"
#include "rf_v13_activity_fusion.h"

#define RA8P1_ACTIVITY_CONTROL_MAGIC   (0x56544341UL) /* ACTV */
#define RA8P1_ACTIVITY_CONTROL_VERSION (2U)
#define RA8P1_ACTIVITY_CACHE_LINE_BYTES (32U)
#define RA8P1_ACTIVITY_CPU0_FLAG_READY (1UL << 0)
#define RA8P1_ACTIVITY_CPU0_FLAG_PANEL_SHUTDOWN_REQUEST (1UL << 1)
#define RA8P1_ACTIVITY_CPU1_FLAG_ONLINE (1UL << 0)
#define RA8P1_ACTIVITY_CPU1_FLAG_PANEL_SHUTDOWN_ACK (1UL << 1)
#define RA8P1_ACTIVITY_REPORT_MASK_BITS (4U)
#define RA8P1_ACTIVITY_REPORT_MASK (0x0FUL)

/* Each core owns one cache-line-sized half.  A core only cleans its own half,
 * so an ACK can never write back stale producer state (or vice versa). */
typedef struct st_ra8p1_activity_cpu0_state
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    volatile uint32_t boot_epoch;
    volatile uint32_t begin_sequence;
    volatile uint32_t end_sequence;
    volatile uint32_t message_sequence;
    volatile uint32_t publish_drops;
    volatile uint32_t flags;
} ra8p1_activity_cpu0_state_t;

typedef struct st_ra8p1_activity_cpu1_state
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    volatile uint32_t boot_epoch;
    volatile uint32_t observed_cpu0_epoch;
    volatile uint32_t acknowledged_message_sequence;
    volatile uint32_t protocol_errors;
    volatile uint32_t flags;
    /* Low bits carry the working-object mask; high bits are a monotonic
     * generation published after each valid CPU1 activity decision. */
    volatile uint32_t report_word;
} ra8p1_activity_cpu1_state_t;

typedef struct st_ra8p1_activity_control
{
    ra8p1_activity_cpu0_state_t cpu0;
    ra8p1_activity_cpu1_state_t cpu1;
} ra8p1_activity_control_t;

#define RA8P1_ACTIVITY_CONTROL \
    ((volatile ra8p1_activity_control_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_ACTIVITY_CONTROL_OFFSET))

#define RA8P1_ACTIVITY_MESSAGE \
    ((volatile rf_v13_cpu0_round_message_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_ACTIVITY_MESSAGE_OFFSET))

typedef char ra8p1_activity_cpu0_state_must_be_32[
    (sizeof(ra8p1_activity_cpu0_state_t) == 32U) ? 1 : -1];
typedef char ra8p1_activity_cpu1_state_must_be_32[
    (sizeof(ra8p1_activity_cpu1_state_t) == 32U) ? 1 : -1];
typedef char ra8p1_activity_control_must_be_64[
    (sizeof(ra8p1_activity_control_t) == RA8P1_ACTIVITY_CONTROL_BYTES) ? 1 : -1];
typedef char ra8p1_activity_cpu0_state_must_start_control[
    (offsetof(ra8p1_activity_control_t, cpu0) == 0U) ? 1 : -1];
typedef char ra8p1_activity_cpu1_state_must_start_second_cache_line[
    (offsetof(ra8p1_activity_control_t, cpu1) ==
     RA8P1_ACTIVITY_CACHE_LINE_BYTES) ? 1 : -1];
typedef char ra8p1_activity_message_must_be_512[
    (sizeof(rf_v13_cpu0_round_message_t) == RA8P1_ACTIVITY_MESSAGE_BYTES) ? 1 : -1];
typedef char ra8p1_activity_control_base_must_be_cache_line_aligned[
    (((RA8P1_SHARED_RAM_BASE + RA8P1_ACTIVITY_CONTROL_OFFSET) &
      (RA8P1_ACTIVITY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_activity_control_size_must_be_cache_line_aligned[
    ((RA8P1_ACTIVITY_CONTROL_BYTES &
      (RA8P1_ACTIVITY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_activity_message_base_must_be_cache_line_aligned[
    (((RA8P1_SHARED_RAM_BASE + RA8P1_ACTIVITY_MESSAGE_OFFSET) &
      (RA8P1_ACTIVITY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_activity_message_size_must_be_cache_line_aligned[
    ((RA8P1_ACTIVITY_MESSAGE_BYTES &
      (RA8P1_ACTIVITY_CACHE_LINE_BYTES - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_activity_control_must_follow_handshake[
    (RA8P1_ACTIVITY_CONTROL_OFFSET >=
     (RA8P1_IPC_HANDSHAKE_OFFSET + RA8P1_IPC_HANDSHAKE_BYTES)) ? 1 : -1];
typedef char ra8p1_activity_control_must_precede_display[
    ((RA8P1_ACTIVITY_CONTROL_OFFSET + RA8P1_ACTIVITY_CONTROL_BYTES) <=
     RA8P1_DISPLAY_STREAM_OFFSET) ? 1 : -1];
typedef char ra8p1_activity_message_must_precede_command[
    ((RA8P1_ACTIVITY_MESSAGE_OFFSET + RA8P1_ACTIVITY_MESSAGE_BYTES) <=
     RA8P1_IPC_COMMAND_OFFSET) ? 1 : -1];
typedef char ra8p1_activity_message_must_follow_tiles[
    ((RA8P1_DISPLAY_TILE_OFFSET + RA8P1_DISPLAY_TILE_BYTES) <=
     RA8P1_ACTIVITY_MESSAGE_OFFSET) ? 1 : -1];

#endif
