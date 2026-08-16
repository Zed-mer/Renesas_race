#ifndef LVGL_APP_H
#define LVGL_APP_H

#include "hal_data.h"
#include "framework/display_stream.h"
#include "framework/display_tile.h"
#include "framework/activity_service.h"
#include "framework/system_protocol.h"

#define LVGL_APP_INPUT_DIAG_MAGIC   (0x544F5543U) /* "TOUC" */
#define LVGL_APP_INPUT_DIAG_VERSION (1U)

/** Persistent raw-touch and LVGL-owner polling diagnostics for SWD/RTT. */
typedef struct st_lvgl_app_input_diag
{
    uint32_t magic;
    uint32_t version;
    uint32_t poll_period_ms;
    uint32_t owner_reads;
    uint32_t poll_calls;
    uint32_t poll_errors;
    uint32_t sample_updates;
    uint32_t pressed_samples;
    uint32_t press_transitions;
    uint32_t release_transitions;
    uint32_t late_polls;
    uint32_t last_state;
    uint32_t last_x;
    uint32_t last_y;
    uint32_t last_error;
    uint32_t last_poll_tick_ms;
    uint32_t max_poll_interval_ms;
    uint32_t last_press_tick_ms;
    uint32_t last_release_tick_ms;
} lvgl_app_input_diag_t;

extern volatile lvgl_app_input_diag_t g_lvgl_app_input_diag;

typedef struct st_lvgl_app_runtime_metrics
{
    uint32_t tick_ms;
    uint32_t presented_frame_count;
    uint32_t presented_fps_millihz;
    uint32_t content_frame_count;
    uint32_t content_fps_millihz;
    uint32_t glcdc_underflow_rate_millihz;
    uint32_t window_rate_millihz;
    uint32_t inference_rate_millihz;
    uint32_t tile_rate_millihz;
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
} lvgl_app_runtime_metrics_t;

fsp_err_t lvgl_app_init(bool touch_available);
void lvgl_app_step(uint32_t elapsed_ms);
void lvgl_app_signal_update(const ra8p1_display_frame_t *frame);
void lvgl_app_activity_update(void);
void lvgl_app_activity_round_update(
    const rf_v27_activity_round_decision_t *decision);
/* True only after the matching frame has reached GLCDC through a VSync-safe
 * buffer change. This is panel-presentation evidence, not the SDR ACK gate. */
bool lvgl_app_frame_presented(const ra8p1_display_frame_t *frame);
void lvgl_app_tile_update(const ra8p1_display_tile_payload_t *tile);
void lvgl_app_signal_reset(void);
void lvgl_app_telemetry_update(const ra8p1_system_telemetry_t *telemetry);
void lvgl_app_runtime_metrics_get(lvgl_app_runtime_metrics_t *metrics);
uint32_t lvgl_app_center_valid_mask(void);
bool lvgl_app_center_frame_get(uint32_t center_index, ra8p1_display_frame_t *frame);

#endif
