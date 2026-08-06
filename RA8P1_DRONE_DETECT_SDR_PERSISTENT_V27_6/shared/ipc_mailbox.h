#ifndef RA8P1_IPC_MAILBOX_H
#define RA8P1_IPC_MAILBOX_H

#include <stdint.h>
#include "resource_layout.h"
#include "system_protocol.h"

#define RA8P1_RUNTIME_METRICS_VERSION (3U)
#define RA8P1_RUNTIME_STATUS_BYTES    (128U)

#define RA8P1_IPC_HANDSHAKE_MAGIC     (0x48414E44UL) /* HAND */
#define RA8P1_IPC_HANDSHAKE_VERSION   (1U)
#define RA8P1_IPC_CPU0_FLAG_READY     (1UL << 0)
#define RA8P1_IPC_CPU1_FLAG_ONLINE   (1UL << 0)
#define RA8P1_IPC_CPU1_FLAG_COMMAND_PENDING (1UL << 1)
#define RA8P1_IPC_CPU1_FLAG_COMMAND_ACKED   (1UL << 2)

typedef struct st_ra8p1_ipc_cpu0_state
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    volatile uint32_t boot_epoch;
    volatile uint32_t ready_sequence;
    volatile uint32_t acknowledged_cpu1_epoch;
    volatile uint32_t acknowledged_command_sequence;
    volatile uint32_t acknowledged_mailbox_sequence;
    volatile uint32_t flags;
} ra8p1_ipc_cpu0_state_t;

typedef struct st_ra8p1_ipc_cpu1_state
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    volatile uint32_t boot_epoch;
    volatile uint32_t observed_cpu0_epoch;
    volatile uint32_t published_command_sequence;
    volatile uint32_t published_mailbox_sequence;
    volatile uint32_t command_retry_count;
    volatile uint32_t flags;
} ra8p1_ipc_cpu1_state_t;

typedef struct st_ra8p1_ipc_handshake
{
    ra8p1_ipc_cpu0_state_t cpu0;
    ra8p1_ipc_cpu1_state_t cpu1;
} ra8p1_ipc_handshake_t;

typedef struct st_ra8p1_telemetry_mailbox
{
    volatile uint32_t begin_sequence;
    ra8p1_system_telemetry_t payload;
    volatile uint32_t end_sequence;
    uint32_t reserved[10];
} ra8p1_telemetry_mailbox_t;

typedef struct st_ra8p1_command_mailbox
{
    volatile uint32_t begin_sequence;
    ra8p1_ui_command_t payload;
    volatile uint32_t end_sequence;
    uint32_t reserved[5];
} ra8p1_command_mailbox_t;

typedef struct st_ra8p1_runtime_status
{
    volatile uint32_t begin_sequence;
    uint32_t cpu1_heartbeat;
    uint32_t display_stage;
    uint32_t glcdc_line_events;
    int32_t last_error;
    volatile uint32_t end_sequence;
    uint32_t glcdc_underflows;
    uint32_t display_running;
    uint32_t metrics_version;
    uint32_t lvgl_tick_ms;
    uint32_t presented_frame_count;
    uint32_t presented_fps_millihz;
    uint32_t glcdc_underflow_rate_millihz;
    uint32_t window_rate_millihz;
    uint32_t inference_rate_millihz;
    uint32_t tile_rate_millihz;
    uint32_t content_frame_count;
    uint32_t content_fps_millihz;
    uint32_t waterfall_columns_generated;
    uint32_t waterfall_tiles_consumed;
    uint32_t waterfall_tiles_dropped;
    uint32_t ipc_frames_received;
    uint32_t ipc_tiles_received;
    uint32_t ipc_tiles_missed;
    uint32_t last_session_id;
    uint32_t last_frame_sequence;
    uint32_t last_tile_sequence;
    uint32_t last_command_sequence;
    uint32_t last_command_status;
    uint32_t last_command_reason;
    uint32_t last_applied_session_id;
    uint32_t runtime_flags;
} ra8p1_runtime_status_t;

#define RA8P1_TELEMETRY_MAILBOX \
    ((volatile ra8p1_telemetry_mailbox_t *) (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_TELEMETRY_OFFSET))
#define RA8P1_COMMAND_MAILBOX \
    ((volatile ra8p1_command_mailbox_t *) (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_COMMAND_OFFSET))
#define RA8P1_IPC_HANDSHAKE \
    ((volatile ra8p1_ipc_handshake_t *) (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_HANDSHAKE_OFFSET))
#define RA8P1_RUNTIME_STATUS \
    ((volatile ra8p1_runtime_status_t *) (RA8P1_SHARED_RAM_BASE + RA8P1_IPC_RUNTIME_OFFSET))

typedef char ra8p1_ipc_cpu0_state_size_must_be_32[
    (sizeof(ra8p1_ipc_cpu0_state_t) == 32U) ? 1 : -1];
typedef char ra8p1_ipc_cpu1_state_size_must_be_32[
    (sizeof(ra8p1_ipc_cpu1_state_t) == 32U) ? 1 : -1];
typedef char ra8p1_ipc_handshake_size_must_be_64[
    (sizeof(ra8p1_ipc_handshake_t) == RA8P1_IPC_HANDSHAKE_BYTES) ? 1 : -1];
typedef char ra8p1_telemetry_mailbox_size_must_be_192[
    (sizeof(ra8p1_telemetry_mailbox_t) == 192U) ? 1 : -1];
typedef char ra8p1_command_mailbox_size_must_be_96[
    (sizeof(ra8p1_command_mailbox_t) == 96U) ? 1 : -1];
typedef char ra8p1_runtime_status_size_must_match[
    (sizeof(ra8p1_runtime_status_t) == RA8P1_RUNTIME_STATUS_BYTES) ? 1 : -1];
typedef char ra8p1_runtime_status_must_precede_rpmsg[
    ((RA8P1_IPC_RUNTIME_OFFSET + RA8P1_RUNTIME_STATUS_BYTES) <=
     RA8P1_IPC_RPMSG_OFFSET) ? 1 : -1];
typedef char ra8p1_ipc_handshake_must_follow_telemetry[
    ((RA8P1_IPC_TELEMETRY_OFFSET + sizeof(ra8p1_telemetry_mailbox_t)) <=
     RA8P1_IPC_HANDSHAKE_OFFSET) ? 1 : -1];

#endif
