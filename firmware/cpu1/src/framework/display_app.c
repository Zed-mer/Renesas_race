#include "display_app.h"

#include "../display_bringup.h"
#include "../lvgl_app.h"
#include <string.h>
#include "alarm_buzzer.h"
#include "activity_service.h"
#include "analysis_contract.h"
#include "campaign_control.h"
#include "ipc_bridge.h"
#include "ui_model.h"

#define DISPLAY_APP_LIVE_RETRY_DELAY_STEPS    (120U)
#define DISPLAY_APP_TILE_DRAIN_BUDGET          (4U)
/* At the configured panel rate, 90 line events are about two seconds. A
 * continuous scan normally publishes several frames inside this interval. */
#define DISPLAY_APP_LIVE_STALL_LINE_EVENTS     (90U)

_Static_assert(DISPLAY_APP_TILE_DRAIN_BUDGET <=
               RA8P1_DISPLAY_TILE_SLOT_COUNT,
               "tile drain budget exceeds the retained IPC slots");

typedef enum e_display_app_live_recovery_state
{
    DISPLAY_APP_LIVE_RECOVERY_IDLE = 0,
    DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED,
    DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED,
    DISPLAY_APP_LIVE_RECOVERY_BACKOFF
} display_app_live_recovery_state_t;

static uint32_t g_ui_command_sequence;
static uint32_t g_runtime_publish_divider;
static uint32_t g_ipc_frames_received;
static uint32_t g_ipc_tiles_received;
static uint32_t g_ipc_tiles_missed;
static uint32_t g_last_session_id;
static uint32_t g_last_visible_session_id;
static uint32_t g_last_frame_sequence;
static uint32_t g_last_tile_sequence;
static uint32_t g_last_command_sequence;
static uint32_t g_last_command_status;
static uint32_t g_last_command_reason;
static uint32_t g_last_applied_session_id;
static uint32_t g_panel_probe_session_id;
static uint32_t g_panel_probe_sequence;
static uint32_t g_panel_probe_window_sequence;
static bool g_panel_probe_pending;
static volatile uint32_t g_panel_results_presented;
static volatile uint32_t g_panel_results_superseded;
static uint32_t g_live_command_sequence;
static uint32_t g_live_recovery_sequence;
static uint32_t g_live_retry_delay_steps;
static display_app_live_recovery_state_t g_live_recovery_state;
static ra8p1_ui_command_t g_live_start_command;
static bool g_live_start_command_valid;
static uint32_t g_live_last_progress_line_event;
static bool g_live_progress_armed;
volatile uint32_t g_display_app_stall_recoveries;
static bool g_panel_shutdown_latched;
typedef bool (*display_capture_request_api_t)(uint64_t,
                                              uint64_t,
                                              uint32_t,
                                              uint32_t,
                                              uint32_t,
                                              uint32_t);
static display_capture_request_api_t volatile g_capture_request_api;

static bool display_app_submit_capture(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags,
    bool continuous_scan);
static bool display_app_prepare_capture_command(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags,
    bool continuous_scan,
    ra8p1_ui_command_t *command);
static bool display_app_send_stop_command(void);
static bool display_app_resend_live_start(void);
static void display_app_live_retry_service(void);
static void display_app_live_progress_service(bool valid_frame);
static void display_app_panel_presentation_service(
    ra8p1_display_frame_t *frame_probe);
static void display_app_panel_presentation_arm(
    const ra8p1_display_frame_t *frame);

static void display_app_publish_activity_report(
    const rf_v27_activity_round_decision_t *decision)
{
    uint32_t working_mask = 0U;

    if ((decision == NULL) ||
        (((decision->flags & RF_V27_ROUND_DECISION_OUTPUT_VALID) == 0U) &&
         ((decision->flags & RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET) == 0U)))
    {
        return;
    }
    if ((decision->flags & RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET) == 0U)
    {
        for (uint32_t object = 0U; object < RF_V13_OBJECT_COUNT; ++object)
        {
            if (decision->object_activity_state[object] ==
                RF_V25_ACTIVITY_WORKING)
            {
                working_mask |= 1UL << object;
            }
        }
    }
    ipc_bridge_cpu1_activity_report_publish(working_mask);
}

static bool display_app_frame_semantically_valid(const ra8p1_display_frame_t *frame)
{
    uint64_t center_hz;
    if ((frame == NULL) ||
        (frame->analysis.center_index >= RA8P1_CENTER_COUNT))
    {
        return false;
    }
    center_hz = ((uint64_t)frame->analysis.center_frequency_high << 32U) |
                frame->analysis.center_frequency_low;
    return center_hz ==
           ra8p1_center_frequency_hz(frame->analysis.center_index);
}

void display_app_init(void)
{
    ui_model_init();
    ipc_bridge_cpu1_init();
    rf_v27_activity_service_init();
    alarm_buzzer_init();
    g_ui_command_sequence = 0U;
    g_runtime_publish_divider = 0U;
    g_ipc_frames_received = 0U;
    g_ipc_tiles_received = 0U;
    g_ipc_tiles_missed = 0U;
    g_last_session_id = 0U;
    g_last_visible_session_id = 0U;
    g_last_frame_sequence = 0U;
    g_last_tile_sequence = 0U;
    g_last_command_sequence = 0U;
    g_last_command_status = RA8P1_COMMAND_NONE;
    g_last_command_reason = RA8P1_COMMAND_REASON_NONE;
    g_last_applied_session_id = 0U;
    g_panel_probe_session_id = 0U;
    g_panel_probe_sequence = 0U;
    g_panel_probe_window_sequence = 0U;
    g_panel_probe_pending = false;
    g_panel_results_presented = 0U;
    g_panel_results_superseded = 0U;
    g_live_command_sequence = 0U;
    g_live_recovery_sequence = 0U;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
    memset(&g_live_start_command, 0, sizeof(g_live_start_command));
    g_live_start_command_valid = false;
    g_live_last_progress_line_event = 0U;
    g_live_progress_armed = false;
    g_display_app_stall_recoveries = 0U;
    g_panel_shutdown_latched = false;
    g_capture_request_api = display_app_request_capture;
    cpu1_campaign_init();
    /* Start in continuous four-center SCAN mode so the activity service can
     * close complete fusion rounds immediately after boot.  The operator can
     * still switch to a single-center FOCUS stream from the header control. */
    (void)display_app_submit_capture(
        RA8P1_CENTER_2420_HZ,
        RA8P1_CENTER_2464_HZ,
        RA8P1_ANALYSIS_SAMPLE_RATE_HZ,
        RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED,
        RA8P1_RF_CHANNEL_A_MASK,
        RA8P1_ANALYSIS_TILE_SAMPLES,
        RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000,
        0U,
        true);
}

void display_app_step(void)
{
    ra8p1_system_telemetry_t telemetry;
    ra8p1_display_frame_t display_frame;
    ra8p1_wifi_status_mailbox_t wifi_status;
    rf_v27_activity_round_decision_t activity_decision;
    const uint32_t alarm_now_ms = g_display_diag.heartbeat;
    bool display_frame_ready;
    bool display_frame_valid;
    bool activity_output_ready;
    ipc_bridge_cpu1_command_service();
    if (ipc_bridge_cpu1_wifi_status_poll(&wifi_status))
    {
        lvgl_app_wifi_status_update(&wifi_status);
    }
    activity_output_ready = rf_v27_activity_service_poll();
    if (g_panel_shutdown_latched)
    {
        alarm_buzzer_force_off();
        return;
    }
    if (ipc_bridge_cpu1_panel_shutdown_requested())
    {
        alarm_buzzer_force_off();
        g_panel_shutdown_latched = true;
        const fsp_err_t shutdown_error =
            display_panel_graceful_shutdown();
        if (FSP_SUCCESS != shutdown_error)
        {
            g_display_diag.last_error = (int32_t)shutdown_error;
        }
        ipc_bridge_cpu1_panel_shutdown_ack();
        return;
    }
    if (activity_output_ready)
    {
        lvgl_app_activity_update();
    }
    if (rf_v27_activity_service_take_round_decision(&activity_decision))
    {
        alarm_buzzer_apply_round(&activity_decision, alarm_now_ms);
        display_app_publish_activity_report(&activity_decision);
        lvgl_app_activity_round_update(&activity_decision);
    }
    alarm_buzzer_step(alarm_now_ms);
    if (ipc_bridge_cpu1_poll(&telemetry))
    {
        ui_model_update(&telemetry);
        g_last_command_sequence = telemetry.command_sequence;
        g_last_command_status = telemetry.command_status;
        g_last_command_reason = telemetry.command_reason;
        g_last_applied_session_id = telemetry.applied_session_id;
        lvgl_app_telemetry_update(&telemetry);
    }
    cpu1_campaign_service(g_last_command_sequence,
                          g_last_command_status,
                          g_last_command_reason,
                          g_last_applied_session_id,
                          ipc_bridge_cpu1_command_pending());
    display_app_live_retry_service();
    display_app_panel_presentation_service(&display_frame);
    display_frame_ready = ipc_bridge_cpu1_display_poll(&display_frame);
    display_frame_valid = display_frame_ready &&
                          display_app_frame_semantically_valid(&display_frame);
    if (ipc_bridge_cpu1_display_session_changed())
    {
        g_last_tile_sequence = 0U;
        /* CPU0 assigns a fresh session_id to each window transaction so ACK /
         * CREDIT cannot be confused with a previous SDR capture.  It is not a
         * UI stream epoch: clearing the four cached spectra and waterfall here
         * turns every normal frequency handover into a visible black frame.
         * Keep the last fully rendered data until the new, semantically valid
         * frame below replaces it.  A true STOP/error/reboot remains explicit
         * control-plane state and must not be inferred from this per-window
         * transport identifier. */
    }
    if (display_frame_valid)
    {
        g_ipc_frames_received++;
        g_last_session_id = display_frame.session_id;
        g_last_frame_sequence = display_frame.sequence;
        ui_model_update_frame(&display_frame);
        /* The seqlock copy and semantic checks above transfer this complete
         * result into CPU1-owned memory.  Release SDR ACK/CREDIT here, before
         * spectrum rasterization or a throttled LVGL/GLCDC presentation.  A
         * campaign's CPU1-visible count uses the same UI-consumable boundary;
         * physical panel presentation remains independently VSync-qualified. */
        if (ipc_bridge_cpu1_display_visible(&display_frame))
        {
            g_last_visible_session_id = display_frame.session_id;
            cpu1_campaign_result_visible(&display_frame);
        }
        lvgl_app_signal_update(&display_frame);
        display_app_panel_presentation_arm(&display_frame);
    }
    display_app_live_progress_service(display_frame_valid);
    display_app_drain_tiles();
    /* Every waterfall update comes from a retained STFT tile, never from a
     * synthetic animation step. */
    (void) g_display_diag;
    if (++g_runtime_publish_divider >= 20U)
    {
        lvgl_app_runtime_metrics_t metrics;
        ra8p1_runtime_status_t runtime_metrics;
        memset(&runtime_metrics, 0, sizeof(runtime_metrics));
        lvgl_app_runtime_metrics_get(&metrics);
        runtime_metrics.lvgl_tick_ms = metrics.tick_ms;
        runtime_metrics.presented_frame_count = metrics.presented_frame_count;
        runtime_metrics.presented_fps_millihz = metrics.presented_fps_millihz;
        runtime_metrics.glcdc_underflow_rate_millihz = metrics.glcdc_underflow_rate_millihz;
        runtime_metrics.window_rate_millihz = metrics.window_rate_millihz;
        runtime_metrics.inference_rate_millihz = metrics.inference_rate_millihz;
        runtime_metrics.tile_rate_millihz = metrics.tile_rate_millihz;
        runtime_metrics.content_frame_count = metrics.content_frame_count;
        runtime_metrics.content_fps_millihz = metrics.content_fps_millihz;
        runtime_metrics.waterfall_columns_generated = metrics.waterfall_columns_generated;
        runtime_metrics.waterfall_tiles_consumed = metrics.waterfall_tiles_consumed;
        runtime_metrics.waterfall_tiles_dropped = metrics.waterfall_tiles_dropped;
        runtime_metrics.ipc_frames_received = g_ipc_frames_received;
        runtime_metrics.ipc_tiles_received = g_ipc_tiles_received;
        runtime_metrics.ipc_tiles_missed = g_ipc_tiles_missed;
        runtime_metrics.last_session_id = g_last_session_id;
        runtime_metrics.last_frame_sequence = g_last_frame_sequence;
        runtime_metrics.last_tile_sequence = g_last_tile_sequence;
        runtime_metrics.last_command_sequence = g_last_command_sequence;
        runtime_metrics.last_command_status = g_last_command_status;
        runtime_metrics.last_command_reason = g_last_command_reason;
        runtime_metrics.last_applied_session_id = g_last_applied_session_id;
        runtime_metrics.runtime_flags = 0U;
        if (g_last_session_id != 0U) runtime_metrics.runtime_flags |= (1UL << 0);
        if (g_ipc_frames_received != 0U || g_ipc_tiles_received != 0U)
            runtime_metrics.runtime_flags |= (1UL << 1);
        if (g_display_diag.running == 0U) runtime_metrics.runtime_flags |= (1UL << 2);
        if ((ui_model_flags() & RA8P1_MODEL_FLAG_PLACEHOLDER) != 0U)
            runtime_metrics.runtime_flags |= (1UL << 3);
        if (ipc_bridge_cpu1_cpu0_ready())
            runtime_metrics.runtime_flags |= (1UL << 4);
        if (ipc_bridge_cpu1_command_pending())
            runtime_metrics.runtime_flags |= (1UL << 5);
        if (ipc_bridge_cpu1_command_retry_count() != 0U)
            runtime_metrics.runtime_flags |= (1UL << 6);
        if (g_live_recovery_state != DISPLAY_APP_LIVE_RECOVERY_IDLE)
            runtime_metrics.runtime_flags |= (1UL << 7);
        if (g_display_app_stall_recoveries != 0U)
            runtime_metrics.runtime_flags |= (1UL << 16);
        runtime_metrics.runtime_flags |= (ui_model_center_valid_mask() << 8U);
        g_runtime_publish_divider = 0U;
        ipc_bridge_cpu1_runtime_update(g_display_diag.heartbeat,
                                       g_display_diag.stage,
                                       g_display_diag.glcdc_line_events,
                                       g_display_diag.last_error,
                                       g_display_diag.glcdc_underflows,
                                       g_display_diag.running,
                                       &runtime_metrics);
    }
}

void display_app_set_alarm_muted(bool muted)
{
    alarm_buzzer_set_muted(muted, g_display_diag.heartbeat);
}

bool display_app_alarm_muted(void)
{
    return alarm_buzzer_is_muted();
}

void display_app_drain_tiles_bounded(uint32_t max_tiles)
{
    ra8p1_display_tile_payload_t display_tile;

    if (max_tiles > RA8P1_DISPLAY_TILE_SLOT_COUNT)
    {
        max_tiles = RA8P1_DISPLAY_TILE_SLOT_COUNT;
    }
    display_underflow_context_enter(DISPLAY_UNDERFLOW_CONTEXT_TILE_DRAIN);
    for (uint32_t tile_index = 0U;
         tile_index < max_tiles;
         ++tile_index)
    {
        if (!ipc_bridge_cpu1_display_tile_poll(&display_tile))
        {
            break;
        }
        g_ipc_tiles_received++;
        if (g_last_session_id != display_tile.session_id)
        {
            g_last_session_id = display_tile.session_id;
            g_last_tile_sequence = 0U;
        }
        if (g_last_tile_sequence == 0U)
        {
            if ((display_tile.sequence >= 2U) &&
                (display_tile.sequence < 0x80000000UL))
            {
                g_ipc_tiles_missed += (display_tile.sequence >> 1U) - 1U;
            }
        }
        else if (
            ((uint32_t)(display_tile.sequence - g_last_tile_sequence) > 2U) &&
            ((uint32_t)(display_tile.sequence - g_last_tile_sequence) < 0x80000000UL))
        {
            g_ipc_tiles_missed +=
                ((display_tile.sequence - g_last_tile_sequence) >> 1U) - 1U;
        }
        g_last_tile_sequence = display_tile.sequence;
        lvgl_app_tile_update(&display_tile);
    }
    display_underflow_context_leave(DISPLAY_UNDERFLOW_CONTEXT_TILE_DRAIN);
}

void display_app_drain_tiles(void)
{
    /* A 1 ms owner loop empties a full 16-slot burst in at most four passes
     * without competing with GLCDC through one large ingestion burst. */
    display_app_drain_tiles_bounded(DISPLAY_APP_TILE_DRAIN_BUDGET);
}

static void display_app_panel_presentation_service(
    ra8p1_display_frame_t *frame_probe)
{
    if (!g_panel_probe_pending || (frame_probe == NULL))
    {
        return;
    }

    frame_probe->session_id = g_panel_probe_session_id;
    frame_probe->sequence = g_panel_probe_sequence;
    frame_probe->analysis.window_sequence = g_panel_probe_window_sequence;
    if (lvgl_app_frame_presented(frame_probe))
    {
        g_panel_probe_pending = false;
        g_panel_results_presented++;
    }
}

static void display_app_panel_presentation_arm(
    const ra8p1_display_frame_t *frame)
{
    if (frame == NULL)
    {
        return;
    }
    if (g_panel_probe_pending)
    {
        g_panel_results_superseded++;
    }
    g_panel_probe_session_id = frame->session_id;
    g_panel_probe_sequence = frame->sequence;
    g_panel_probe_window_sequence = frame->analysis.window_sequence;
    g_panel_probe_pending = true;
}

bool display_app_request_capture(uint64_t center_a_hz,
                                 uint64_t center_b_hz,
                                 uint32_t sample_rate_hz,
                                 uint32_t iq_format,
                                 uint32_t channel_mask,
                                 uint32_t fft_interval_samples)
{
    return display_app_request_capture_with_controls(
        center_a_hz,
        center_b_hz,
        sample_rate_hz,
        iq_format,
        channel_mask,
        fft_interval_samples,
        RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000,
        0U);
}

bool display_app_request_capture_with_controls(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags)
{
    const bool scan_all =
        (center_a_hz == RA8P1_CENTER_2420_HZ) &&
        (center_b_hz == RA8P1_CENTER_2464_HZ);
    return display_app_submit_capture(center_a_hz,
                                      center_b_hz,
                                      sample_rate_hz,
                                      iq_format,
                                      channel_mask,
                                      fft_interval_samples,
                                      target_payload_mbps_x1000,
                                      test_fault_flags,
                                      scan_all);
}

static bool display_app_submit_capture(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags,
    bool continuous_scan)
{
    ra8p1_ui_command_t command;
    bool sent;
    if (!display_app_prepare_capture_command(
            center_a_hz,
            center_b_hz,
            sample_rate_hz,
            iq_format,
            channel_mask,
            fft_interval_samples,
            target_payload_mbps_x1000,
            test_fault_flags,
            continuous_scan,
            &command))
    {
        return false;
    }
    g_ui_command_sequence++;
    if (g_ui_command_sequence == 0U)
    {
        g_ui_command_sequence = 1U;
    }
    command.sequence = g_ui_command_sequence;
    sent = ipc_bridge_cpu1_command_send(&command);
    if (sent)
    {
        if (continuous_scan)
        {
            g_live_start_command = command;
            g_live_start_command_valid = true;
            g_live_command_sequence = command.sequence;
        }
        else
        {
            g_live_start_command_valid = false;
            g_live_command_sequence = 0U;
        }
        g_live_recovery_sequence = 0U;
        g_live_retry_delay_steps = 0U;
        g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
    }
    return sent;
}

static bool display_app_prepare_capture_command(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags,
    bool continuous_scan,
    ra8p1_ui_command_t *command)
{
    const bool scan_all =
        (center_a_hz == RA8P1_CENTER_2420_HZ) &&
        (center_b_hz == RA8P1_CENTER_2464_HZ);
    if ((command == NULL) ||
        (target_payload_mbps_x1000 <
         RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000) ||
        (target_payload_mbps_x1000 >
         RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000) ||
        ((target_payload_mbps_x1000 % 1000U) != 0U) ||
        ((test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) != 0U))
    {
        return false;
    }
    memset(command, 0, sizeof(*command));
    command->magic = RA8P1_SYSTEM_PROTOCOL_MAGIC;
    command->version = RA8P1_SYSTEM_PROTOCOL_VERSION;
    command->size = (uint16_t) sizeof(*command);
    command->requested_center_hz_low = (uint32_t)center_a_hz;
    command->requested_center_hz_high = (uint32_t)(center_a_hz >> 32U);
    command->requested_center_b_hz_low = (uint32_t)center_b_hz;
    command->requested_center_b_hz_high = (uint32_t)(center_b_hz >> 32U);
    command->requested_sample_rate_hz = sample_rate_hz;
    command->requested_bandwidth_hz = RA8P1_ANALYSIS_BANDWIDTH_HZ;
    command->requested_gain_mdB = 0;
    command->requested_iq_format = iq_format;
    command->flags = RA8P1_COMMAND_FLAG_START |
                     RA8P1_COMMAND_FLAG_APPLY_RF;
    if (scan_all)
    {
        command->flags |= RA8P1_COMMAND_FLAG_SCAN_ALL;
    }
    if (continuous_scan)
    {
        /* CPU0 owns both live scheduling forms.  SCAN_ALL selects a repeated
         * four-center sweep; otherwise requested_center_hz is repeated. */
        command->flags |= RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS;
    }
    command->requested_channel_mask = channel_mask;
    command->requested_fft_interval_samples = fft_interval_samples;
    command->action = RA8P1_COMMAND_ACTION_START;
    command->target_payload_mbps_x1000 = target_payload_mbps_x1000;
    command->test_fault_flags = test_fault_flags;
    return true;
}

bool display_app_request_focus(uint32_t center_index)
{
    ra8p1_ui_command_t command;
    if ((center_index >= RA8P1_CENTER_COUNT) ||
        cpu1_campaign_owns_scheduler() ||
        !display_app_prepare_capture_command(
            ra8p1_center_frequency_hz(center_index),
            0ULL,
            RA8P1_ANALYSIS_SAMPLE_RATE_HZ,
            RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED,
            RA8P1_RF_CHANNEL_A_MASK,
            RA8P1_ANALYSIS_TILE_SAMPLES,
            RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000,
            0U,
            true,
            &command))
    {
        return false;
    }

    /* Store the desired START without sending it.  STOP/CANCEL must retire
     * the old scheduler first; display_app_live_retry_service waits for the
     * matching STOPPED telemetry before it can resend this command. */
    g_live_start_command = command;
    g_live_start_command_valid = true;
    g_live_command_sequence = 0U;
    g_live_recovery_sequence = 0U;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;
    return true;
}

bool display_app_request_scan(void)
{
    ra8p1_ui_command_t command;
    if (cpu1_campaign_owns_scheduler() ||
        !display_app_prepare_capture_command(
            RA8P1_CENTER_2420_HZ,
            RA8P1_CENTER_2464_HZ,
            RA8P1_ANALYSIS_SAMPLE_RATE_HZ,
            RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED,
            RA8P1_RF_CHANNEL_A_MASK,
            RA8P1_ANALYSIS_TILE_SAMPLES,
            RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000,
            0U,
            true,
            &command))
    {
        return false;
    }

    g_live_start_command = command;
    g_live_start_command_valid = true;
    g_live_command_sequence = 0U;
    g_live_recovery_sequence = 0U;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;
    return true;
}

static bool display_app_live_rejection_is_transient(uint32_t reason)
{
    return (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_BUSY) ||
           (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED) ||
           (reason == RA8P1_COMMAND_REASON_SDR_CONTROL_TIMEOUT);
}

static bool display_app_send_stop_command(void)
{
    ra8p1_ui_command_t command;
    memset(&command, 0, sizeof(command));
    command.magic = RA8P1_SYSTEM_PROTOCOL_MAGIC;
    command.version = RA8P1_SYSTEM_PROTOCOL_VERSION;
    command.size = (uint16_t)sizeof(command);
    g_ui_command_sequence++;
    if (g_ui_command_sequence == 0U)
    {
        g_ui_command_sequence = 1U;
    }
    command.sequence = g_ui_command_sequence;
    command.flags = RA8P1_COMMAND_FLAG_STOP;
    command.action = RA8P1_COMMAND_ACTION_STOP;
    return ipc_bridge_cpu1_command_send(&command);
}

static bool display_app_resend_live_start(void)
{
    ra8p1_ui_command_t command;
    if (!g_live_start_command_valid)
    {
        return false;
    }
    command = g_live_start_command;
    g_ui_command_sequence++;
    if (g_ui_command_sequence == 0U)
    {
        g_ui_command_sequence = 1U;
    }
    command.sequence = g_ui_command_sequence;
    if (!ipc_bridge_cpu1_command_send(&command))
    {
        return false;
    }
    g_live_start_command = command;
    g_live_command_sequence = command.sequence;
    g_live_recovery_sequence = 0U;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
    g_live_progress_armed = false;
    return true;
}

static void display_app_live_progress_service(bool valid_frame)
{
    const bool current_live_command =
        (g_live_command_sequence != 0U) &&
        (g_last_command_sequence == g_live_command_sequence);
    const uint32_t line_event = g_display_diag.glcdc_line_events;

    if (valid_frame)
    {
        g_live_last_progress_line_event = line_event;
        g_live_progress_armed = true;
        return;
    }
    if (cpu1_campaign_owns_scheduler() ||
        (g_live_recovery_state != DISPLAY_APP_LIVE_RECOVERY_IDLE) ||
        !g_live_start_command_valid || !current_live_command ||
        (g_last_command_status != RA8P1_COMMAND_APPLIED) ||
        ipc_bridge_cpu1_command_pending() ||
        !ipc_bridge_cpu1_cpu0_ready())
    {
        g_live_progress_armed = false;
        return;
    }
    if (!g_live_progress_armed)
    {
        g_live_last_progress_line_event = line_event;
        g_live_progress_armed = true;
        return;
    }
    if ((line_event - g_live_last_progress_line_event) <
        DISPLAY_APP_LIVE_STALL_LINE_EVENTS)
    {
        return;
    }

    /* Reuse the ordered recovery path. It first retires every active,
     * prefetched and fallback SDR identity, waits for STOPPED telemetry, then
     * reissues the exact saved continuous command. */
    g_display_app_stall_recoveries++;
    g_live_progress_armed = false;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;
}

static void display_app_live_retry_service(void)
{
    const bool current_live_command =
        (g_live_command_sequence != 0U) &&
        (g_last_command_sequence == g_live_command_sequence);

    if (cpu1_campaign_owns_scheduler())
    {
        g_live_command_sequence = 0U;
        g_live_recovery_sequence = 0U;
        g_live_retry_delay_steps = 0U;
        g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
        g_live_start_command_valid = false;
        return;
    }
    if ((g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_IDLE) &&
        current_live_command &&
        (g_last_command_status == RA8P1_COMMAND_REJECTED))
    {
        if (!display_app_live_rejection_is_transient(g_last_command_reason))
        {
            g_live_command_sequence = 0U;
            g_live_start_command_valid = false;
            return;
        }
        g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;
        g_live_retry_delay_steps = 0U;
    }

    if (g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED)
    {
        if (ipc_bridge_cpu1_command_pending() ||
            !ipc_bridge_cpu1_cpu0_ready())
        {
            return;
        }
        if (g_live_retry_delay_steps != 0U)
        {
            g_live_retry_delay_steps--;
            return;
        }
        if (display_app_send_stop_command())
        {
            g_live_recovery_sequence = g_ui_command_sequence;
            g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED;
        }
        else
        {
            g_live_retry_delay_steps = DISPLAY_APP_LIVE_RETRY_DELAY_STEPS;
        }
        return;
    }

    if (g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_WAIT_STOPPED)
    {
        if (g_last_command_sequence != g_live_recovery_sequence)
        {
            return;
        }
        if ((g_last_command_status == RA8P1_COMMAND_APPLIED) &&
            (g_last_command_reason == RA8P1_COMMAND_REASON_STOPPED))
        {
            g_live_retry_delay_steps = DISPLAY_APP_LIVE_RETRY_DELAY_STEPS;
            g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_BACKOFF;
        }
        else if (g_last_command_status == RA8P1_COMMAND_REJECTED)
        {
            if (display_app_live_rejection_is_transient(g_last_command_reason))
            {
                g_live_retry_delay_steps = DISPLAY_APP_LIVE_RETRY_DELAY_STEPS;
                g_live_recovery_state =
                    DISPLAY_APP_LIVE_RECOVERY_STOP_REQUIRED;
            }
            else
            {
                g_live_command_sequence = 0U;
                g_live_recovery_sequence = 0U;
                g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
                g_live_start_command_valid = false;
            }
        }
        return;
    }

    if (g_live_recovery_state == DISPLAY_APP_LIVE_RECOVERY_BACKOFF)
    {
        if (g_live_retry_delay_steps != 0U)
        {
            g_live_retry_delay_steps--;
            return;
        }
        if (ipc_bridge_cpu1_command_pending() ||
            !ipc_bridge_cpu1_cpu0_ready())
        {
            return;
        }
        if (!display_app_resend_live_start())
        {
            if (!g_live_start_command_valid)
            {
                g_live_command_sequence = 0U;
                g_live_recovery_sequence = 0U;
                g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
                return;
            }
            g_live_retry_delay_steps = DISPLAY_APP_LIVE_RETRY_DELAY_STEPS;
        }
    }
}

bool display_app_campaign_command_start(uint32_t center_index,
                                        bool scan_all,
                                        bool continuous_scan,
                                        uint32_t target_payload_mbps_x1000,
                                        uint32_t test_fault_flags)
{
    uint64_t center_a_hz;
    uint64_t center_b_hz;
    if ((!scan_all && (center_index >= RA8P1_CENTER_COUNT)) ||
        (scan_all && (center_index != 0U)))
    {
        return false;
    }
    center_a_hz = scan_all ? RA8P1_CENTER_2420_HZ :
                             ra8p1_center_frequency_hz(center_index);
    center_b_hz = scan_all ? RA8P1_CENTER_2464_HZ : 0U;
    return display_app_submit_capture(
        center_a_hz,
        center_b_hz,
        RA8P1_ANALYSIS_SAMPLE_RATE_HZ,
        RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED,
        RA8P1_RF_CHANNEL_A_MASK,
        RA8P1_ANALYSIS_TILE_SAMPLES,
        target_payload_mbps_x1000,
        test_fault_flags,
        continuous_scan);
}

bool display_app_campaign_command_stop(void)
{
    const bool sent = display_app_send_stop_command();
    if (sent)
    {
        display_app_campaign_takeover();
    }
    return sent;
}

void display_app_campaign_takeover(void)
{
    /* CPU1 only submits explicit campaign commands.  Continuous scheduling
     * is held by CPU0 and is stopped through display_app_campaign_command_stop.
     */
    g_live_command_sequence = 0U;
    g_live_recovery_sequence = 0U;
    g_live_retry_delay_steps = 0U;
    g_live_recovery_state = DISPLAY_APP_LIVE_RECOVERY_IDLE;
    g_live_start_command_valid = false;
}

uint32_t display_app_last_issued_command_sequence(void)
{
    return g_ui_command_sequence;
}

uint32_t display_app_last_visible_session_id(void)
{
    return g_last_visible_session_id;
}
