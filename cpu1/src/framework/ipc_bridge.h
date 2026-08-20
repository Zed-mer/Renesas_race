#ifndef CPU1_IPC_BRIDGE_H
#define CPU1_IPC_BRIDGE_H

#include <stdbool.h>
#include "display_stream.h"
#include "display_tile.h"
#include "ipc_mailbox.h"
#include "latency_telemetry.h"
#include "rf_v13_activity_fusion.h"
#include "system_protocol.h"

void ipc_bridge_cpu1_init(void);
bool ipc_bridge_cpu1_poll(ra8p1_system_telemetry_t *telemetry);
bool ipc_bridge_cpu1_display_poll(ra8p1_display_frame_t *frame);
/* Publish the ownership ACK after the polled frame has been copied into
 * CPU1-owned UI memory. This does not assert physical panel presentation. */
bool ipc_bridge_cpu1_display_visible(const ra8p1_display_frame_t *frame);
bool ipc_bridge_cpu1_display_tile_poll(ra8p1_display_tile_payload_t *tile);
bool ipc_bridge_cpu1_display_session_changed(void);
bool ipc_bridge_cpu1_activity_poll(
    rf_v13_cpu0_round_message_t *message,
    bool *cpu0_epoch_changed);
bool ipc_bridge_cpu1_command_send(const ra8p1_ui_command_t *command);
void ipc_bridge_cpu1_command_service(void);
bool ipc_bridge_cpu1_cpu0_ready(void);
bool ipc_bridge_cpu1_command_pending(void);
uint32_t ipc_bridge_cpu1_command_retry_count(void);
bool ipc_bridge_cpu1_panel_shutdown_requested(void);
void ipc_bridge_cpu1_panel_shutdown_ack(void);
void ipc_bridge_cpu1_activity_report_publish(uint32_t working_mask);
void ipc_bridge_cpu1_runtime_update(uint32_t heartbeat,
                                    uint32_t display_stage,
                                    uint32_t glcdc_line_events,
                                    int32_t last_error,
                                    uint32_t glcdc_underflows,
                                    uint32_t display_running,
                                    const ra8p1_runtime_status_t *metrics);

#endif
