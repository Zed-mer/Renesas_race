#ifndef CPU0_IPC_BRIDGE_H
#define CPU0_IPC_BRIDGE_H

#include <stdbool.h>
#include "display_stream.h"
#include "display_tile.h"
#include "rf_v13_activity_fusion.h"
#include "latency_telemetry.h"
#include "system_protocol.h"
#include "wifi_status_mailbox.h"

void ipc_bridge_cpu0_init(void);
void ipc_bridge_cpu0_publish(const ra8p1_system_telemetry_t *telemetry);
void ipc_bridge_cpu0_display_publish(const ra8p1_display_frame_t *frame,
                                     const uint8_t *display_tile);
void ipc_bridge_cpu0_display_tile_publish(const uint8_t *display_tile,
                                          uint32_t window_sequence,
                                          uint32_t flags);
/* display_tile points to the complete 16 x 64 analysis matrix.  The bridge
 * emits one compact shared-memory slot for each row in the declared range. */
void ipc_bridge_cpu0_display_tile_publish_ex(const uint8_t *display_tile,
                                             uint32_t window_sequence,
                                             uint32_t flags,
                                             uint32_t center_index,
                                             uint8_t tile_index,
                                             uint8_t tile_count,
                                             uint8_t novel_time_start,
                                             uint8_t novel_time_count);
void ipc_bridge_cpu0_display_session_set(uint32_t session_id);
bool ipc_bridge_cpu0_command_get(ra8p1_ui_command_t *command);
bool ipc_bridge_cpu0_activity_publish(
    const rf_v13_cpu0_round_message_t *message);
bool ipc_bridge_cpu0_activity_report_read(uint32_t *generation,
                                          uint32_t *working_mask);
void ipc_bridge_cpu0_wifi_status_publish(
    ra8p1_wifi_connection_state_t connection_state,
    const char *ssid);

/* CPU0 latency evidence is kept separate from the display payload so a
 * 60-Msample burst never copies raw IQ or enlarges the RPMsg region. */
bool ipc_bridge_cpu0_latency_cycle_now(uint32_t *cycles);
void ipc_bridge_cpu0_latency_session_begin(uint32_t session_id,
                                           uint64_t total_samples,
                                           uint32_t window_samples,
                                           uint32_t stride_samples);
uint32_t ipc_bridge_cpu0_latency_ingress_prepare(uint32_t session_id,
                                                 uint64_t sample_index,
                                                 uint32_t complex_samples,
                                                 uint32_t *ingress_cycles);
void ipc_bridge_cpu0_latency_ingress_commit(uint32_t session_id,
                                            uint32_t window_mask,
                                            uint32_t ingress_cycles);
void ipc_bridge_cpu0_latency_window_complete(uint32_t session_id,
                                             uint32_t window_index,
                                             uint32_t complete_cycles,
                                             bool timing_valid);
void ipc_bridge_cpu0_latency_poll(void);
bool ipc_bridge_cpu0_latency_result_visible(uint32_t session_id,
                                            uint32_t window_index);

#endif
