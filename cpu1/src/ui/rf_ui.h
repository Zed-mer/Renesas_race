#ifndef RF_UI_H
#define RF_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_UI_CHANNEL_COUNT 4u
#define RF_UI_SPECTRUM_BINS 256u
#define RF_UI_WATERFALL_FREQ_BINS 192u
#define RF_UI_WATERFALL_COLS 160u
#define RF_UI_WATERFALL_HISTORY_COLS 256u
#define RF_UI_WATERFALL_STORAGE_COLS (RF_UI_WATERFALL_HISTORY_COLS * 2u)
#define RF_UI_DETECTION_COUNT 4u
#define RF_UI_MAX_RF_BOXES 4u
#define RF_UI_RF_COORD_SCALE 256u
#define RF_UI_WATERFALL_OVERLAY_BITS_PER_PIXEL 4u
#define RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS 16u

#define RF_UI_RF_BOX_FLAG_VALID               (1u << 0)
#define RF_UI_RF_BOX_FLAG_TIME_CLIPPED        (1u << 1)
#define RF_UI_RF_BOX_FLAG_FREQUENCY_CLIPPED   (1u << 2)
#define RF_UI_RF_BOX_FLAG_VIDEO_20MHZ         (1u << 3)
#define RF_UI_RF_BOX_FLAG_BANDWIDTH_AMBIGUOUS (1u << 4)
#define RF_UI_RF_BOX_FLAG_NEEDS_REVIEW        (1u << 5)

#define RF_UI_CHANNEL_SWITCH_DIAG_MAGIC   0x53574348u /* "SWCH" */
#define RF_UI_CHANNEL_SWITCH_DIAG_VERSION 16u
#define RF_UI_CHANNEL_SOAK_MAGIC          0x534F414Bu /* "SOAK" */
#define RF_UI_CHANNEL_SOAK_VERSION        1u
#define RF_UI_RUNTIME_MONITOR_MAGIC       0x4C495645u /* "LIVE" */
#define RF_UI_RUNTIME_MONITOR_VERSION     2u
#define RF_UI_INPUT_DIAG_MAGIC            0x55494E50u /* "UINP" */
#define RF_UI_INPUT_DIAG_VERSION          1u
#define RF_UI_CHANNEL_NONE                UINT32_MAX

typedef enum {
    RF_UI_INPUT_CONTROL_NONE = 0,
    RF_UI_INPUT_CONTROL_ACQUISITION,
    RF_UI_INPUT_CONTROL_LIVE,
    RF_UI_INPUT_CONTROL_COMPARE_OPEN,
    RF_UI_INPUT_CONTROL_COMPARE_CLOSE,
    RF_UI_INPUT_CONTROL_TARGET,
    RF_UI_INPUT_CONTROL_COMPARE_TARGET,
    RF_UI_INPUT_CONTROL_CHANNEL,
    RF_UI_INPUT_CONTROL_HISTORY_BUTTON,
    RF_UI_INPUT_CONTROL_HISTORY_SLIDER,
    RF_UI_INPUT_CONTROL_WATERFALL,
    RF_UI_INPUT_CONTROL_COUNT,
} rf_ui_input_control_t;

/** Persistent UI callback diagnostics; control_events is indexed above. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t events;
    uint32_t handled_events;
    uint32_t ignored_events;
    uint32_t last_control;
    uint32_t last_value;
    uint32_t last_event_code;
    uint32_t last_event_tick_ms;
    uint32_t control_events[RF_UI_INPUT_CONTROL_COUNT];
} rf_ui_input_diag_t;

extern volatile rf_ui_input_diag_t g_rf_ui_input_diag;

typedef enum {
    RF_UI_PAGE_MONITOR = 0,
    RF_UI_PAGE_RECOGNITION = 1,
} rf_ui_page_t;

typedef enum {
    RF_UI_DETECTION_ACTIVE = 0,
    RF_UI_DETECTION_SUSPECTED = 1,
    RF_UI_DETECTION_INACTIVE = 2,
} rf_ui_detection_state_t;

typedef enum {
    RF_UI_FUSION_NO_RF = 0,
    RF_UI_FUSION_UNCERTAIN = 1,
    RF_UI_FUSION_WORKING = 2,
} rf_ui_fusion_activity_t;

#define RF_UI_FUSION_ROUND_HAS_MESSAGE  (1u << 0)
#define RF_UI_FUSION_ROUND_OUTPUT_VALID (1u << 1)
#define RF_UI_FUSION_ROUND_CPU0_RESET   (1u << 2)

typedef struct {
    uint32_t message_sequence;
    uint32_t round_index;
    uint32_t session_id[RF_UI_CHANNEL_COUNT];
    uint32_t window_sequence[RF_UI_CHANNEL_COUNT];
    uint8_t identity_mask;
    uint8_t identity_conflict_mask;
    uint8_t activity_state[RF_UI_DETECTION_COUNT];
    uint8_t flags;
    uint8_t reserved;
} rf_ui_fusion_round_t;

typedef struct {
    int8_t peak_dbfs;
    int8_t noise_floor_dbfs;
    uint8_t occupancy_percent;
    uint32_t age_ms;
} rf_ui_channel_metrics_t;

typedef struct {
    rf_ui_detection_state_t state;
    uint8_t confidence_percent;
    uint8_t channel_index;
} rf_ui_detection_t;

typedef enum {
    RF_UI_CHANNEL_SWITCH_IDLE = 0,
    RF_UI_CHANNEL_SWITCH_WAIT_WINDOW,
    RF_UI_CHANNEL_SWITCH_SPECTRUM_BASE,
    RF_UI_CHANNEL_SWITCH_SPECTRUM_TRACE,
    RF_UI_CHANNEL_SWITCH_SPECTRUM_PEAK,
    RF_UI_CHANNEL_SWITCH_WATERFALL_BASE,
    RF_UI_CHANNEL_SWITCH_WATERFALL_CATCHUP,
    RF_UI_CHANNEL_SWITCH_WATERFALL_RENDER,
} rf_ui_channel_switch_state_t;

/** Persistent switch/build counters for RTT or non-invasive J-Link reads. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t request_generation;
    uint32_t pending_channel;
    uint32_t committed_channel;
    uint32_t build_channel;
    uint32_t active_source;
    uint32_t requests;
    uint32_t cancellations;
    uint32_t complete_windows;
    uint32_t stale_windows;
    uint32_t build_starts;
    uint32_t build_restarts;
    uint32_t build_chunks;
    uint32_t build_rows;
    uint32_t build_completions;
    uint32_t atomic_commits;
    uint32_t last_chunk_bytes;
    uint32_t max_chunk_bytes;
    uint32_t last_session_id;
    uint32_t last_window_sequence;
    uint32_t live_build_starts;
    uint32_t live_build_cancellations;
    uint32_t live_build_chunks;
    uint32_t live_build_rows;
    uint32_t live_build_completions;
    uint32_t live_render_chunks;
    uint32_t live_atomic_commits;
    uint32_t live_last_chunk_bytes;
    uint32_t live_max_chunk_bytes;
    uint32_t live_base_rebuilds;
    uint32_t live_incremental_builds;
    uint32_t spectrum_presents;
    uint32_t waterfall_invalidations;
    uint32_t waterfall_invalidated_rows;
    uint32_t spectrum_invalidations;
    uint32_t spectrum_invalidated_rows;
    uint32_t waterfall_source_rebinds;
    uint32_t spectrum_source_rebinds;
    uint32_t source_rebind_failures;
    uint32_t last_waterfall_descriptor;
    uint32_t last_waterfall_data;
    uint32_t last_waterfall_source;
    uint32_t last_waterfall_render_column;
    uint32_t switch_metadata_stage_steps;
    uint32_t switch_metadata_stage_restarts;
    uint32_t overlay_build_chunks;
    uint32_t overlay_build_rows;
    uint32_t overlay_presents;
    uint32_t overlay_pixels_advanced;
    uint32_t overlay_max_backlog_pixels;
    uint32_t overlay_source_switches;
    uint32_t overlay_guard_bytes;
    uint32_t overlay_guard_max_bytes;
    uint32_t overlay_box_refreshes;
    uint32_t overlay_fallbacks;
    uint32_t overlay_last_fallback_error;
    uint32_t overlay_frame_generation;
    uint32_t overlay_latched_generation;
    uint32_t overlay_sync_starts;
    uint32_t overlay_sync_chunks;
    uint32_t overlay_sync_rows;
    uint32_t overlay_sync_completions;
    uint32_t overlay_sync_last_chunk_bytes;
    uint32_t overlay_sync_max_chunk_bytes;
    uint32_t switch_catchup_passes;
    uint32_t switch_catchup_completions;
    uint32_t switch_catchup_overwrite_restarts;
    uint32_t switch_catchup_head_mismatches;
    uint32_t switch_catchup_backlog_at_render;
    uint32_t switch_catchup_max_backlog_at_render;
    uint32_t live_catchup_passes;
    uint32_t live_catchup_completions;
    uint32_t live_catchup_overwrite_cancellations;
    uint32_t live_catchup_head_mismatches;
    uint32_t live_catchup_backlog_at_ready;
    uint32_t live_catchup_max_backlog_at_ready;
    uint32_t switch_request_line_event;
    uint32_t switch_commit_line_event;
    uint32_t switch_last_latency_line_events;
    uint32_t switch_max_latency_line_events;
    uint32_t switch_metadata_refresh_deferrals;
    uint32_t switch_metadata_post_commit_refreshes;
    uint32_t overlay_latch_pven_deferrals;
    uint32_t overlay_latch_pven_wait_polls;
    uint32_t overlay_latch_pven_confirmations;
    uint32_t overlay_guard_clip_submits;
    uint32_t overlay_guard_clip_pixels;
    uint32_t overlay_guard_clip_zero_prefix_submits;
    uint32_t raw_boxes_received;
    uint32_t box_batches_waiting_for_fusion;
    /* Legacy field name retained for SWD tooling; counts all valid model-box
     * commits, independent of the device-level activity state. */
    uint32_t history_boxes_committed_working;
    uint32_t history_boxes_dropped_idle;
    uint32_t history_boxes_dropped_uncertain;
    uint32_t history_boxes_dropped_stale;
    uint32_t history_boxes_dropped_identity_mismatch;
    uint32_t history_boxes_dropped_out_of_history;
    uint32_t pending_box_batch_high_water;
    uint32_t last_committed_round_index;
    uint32_t switch_cache_hits;
    uint32_t switch_cache_misses;
    uint32_t switch_cache_stale_misses;
    uint32_t switch_cache_catchup_columns;
    uint32_t switch_cache_max_catchup_columns;
} rf_ui_channel_switch_diag_t;

extern volatile rf_ui_channel_switch_diag_t g_rf_ui_channel_switch_diag;

/**
 * Live-memory command/status mailbox for detached four-channel switch tests.
 * Write requested_switches first and command_generation last. Normal firmware
 * operation leaves command_generation at zero and never starts the test.
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command_generation;
    uint32_t active_generation;
    uint32_t requested_switches;
    uint32_t completed_switches;
    uint32_t running;
    uint32_t errors;
    uint32_t next_channel;
    uint32_t last_requested_channel;
} rf_ui_channel_soak_t;

extern volatile rf_ui_channel_soak_t g_rf_ui_channel_soak;

/** Firmware-owned detached display observation window. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command_generation;
    uint32_t active_generation;
    uint32_t requested_line_events;
    uint32_t running;
    uint32_t start_line_event;
    uint32_t end_line_event;
    uint32_t start_underflows;
    uint32_t end_underflows;
    uint32_t start_live_commits;
    uint32_t end_live_commits;
    uint32_t start_spectrum_presents;
    uint32_t end_spectrum_presents;
    uint32_t start_complete_windows;
    uint32_t end_complete_windows;
    uint32_t start_buffer_errors;
    uint32_t end_buffer_errors;
    uint32_t start_overlay_presents;
    uint32_t end_overlay_presents;
    uint32_t start_overlay_pixels;
    uint32_t end_overlay_pixels;
    uint32_t start_overlay_underflows;
    uint32_t end_overlay_underflows;
    uint32_t start_overlay_fallbacks;
    uint32_t end_overlay_fallbacks;
    uint32_t errors;
} rf_ui_runtime_monitor_t;

extern volatile rf_ui_runtime_monitor_t g_rf_ui_runtime_monitor;

/** One VSync-paced CLUT4 Layer 2 frame prepared by the RF UI owner. */
typedef struct {
    uint8_t * base;
    uint32_t hstride_pixels;
    uint32_t generation;
    uint16_t hsize;
    uint16_t vsize;
    int16_t x;
    int16_t y;
    uint16_t pixels_advanced;
    uint16_t transparent_prefix;
    uint8_t source;
    uint8_t phase;
} rf_ui_waterfall_overlay_frame_t;

/** Physical RF bounds normalized to one completed analysis window. */
typedef struct {
    uint8_t frequency_start_q8;
    uint8_t time_start_q8;
    uint8_t frequency_span_q8;
    uint8_t time_span_q8;
    uint8_t detection_index;
    uint8_t confidence_percent;
    uint8_t flags;
    uint8_t reserved;
} rf_ui_rf_box_t;

/**
 * Build the complete 1024 x 600 UI on lv_screen_active().
 * Call after lv_init() and display creation, from the sole LVGL owner task.
 */
void rf_ui_create(void);

void rf_ui_set_page(rf_ui_page_t page);
rf_ui_page_t rf_ui_get_page(void);
void rf_ui_set_running(int running);
int rf_ui_is_running(void);
void rf_ui_toggle_running(void);

/** Disable the startup demo animation after real IPC data becomes available. */
void rf_ui_set_external_spectrum_mode(bool enabled);

/**
 * Select the one channel shown by the large spectrum and waterfall views.
 * These functions, like every LVGL-facing UI API, belong to the LVGL owner.
 */
bool rf_ui_set_selected_channel(uint32_t channel_index);
/** Return the channel whose labels and image sources are currently committed. */
uint32_t rf_ui_get_selected_channel(void);
/** Return the newest requested channel, or the committed channel when idle. */
uint32_t rf_ui_get_pending_channel(void);

/** Keep the acquisition badge and data-fusion policy aligned with CPU0. */
void rf_ui_set_focus_mode(bool focus_mode);
bool rf_ui_is_focus_mode(void);

/**
 * Replace one channel's native 256-bin spectrum. Lower-resolution inputs are
 * rejected so the UI cannot silently present interpolated points as real RF
 * resolution. Data is copied before return. This and the other update APIs
 * must be called from the LVGL owner task.
 */
bool rf_ui_update_spectrum(uint32_t channel_index,
                           const uint8_t * bins,
                           size_t bin_count);

/** Cache a spectrum with the RF window identity used by atomic channel switch. */
bool rf_ui_update_spectrum_window(uint32_t channel_index,
                                  const uint8_t * bins,
                                  size_t bin_count,
                                  uint32_t session_id,
                                  uint32_t window_sequence);

/**
 * Announce the real final row of a complete RF window after it was ingested.
 * The caller must only use novel_time_start == 15 (tile height - 1).
 */
bool rf_ui_note_complete_window(uint32_t channel_index,
                                uint32_t session_id,
                                uint32_t window_sequence,
                                uint32_t transport_sequence);

/** Process at most one bounded background build chunk from the LVGL owner. */
bool rf_ui_channel_switch_step(void);
bool rf_ui_channel_switch_busy(void);
void rf_ui_channel_switch_diag_get(rf_ui_channel_switch_diag_t * diagnostics);
void rf_ui_runtime_monitor_step(void);

/**
 * Rasterize and invalidate the newest selected-channel spectrum, if dirty.
 * Returns true only when a new spectrum invalidation was submitted.
 */
bool rf_ui_present_spectrum(void);

/**
 * Replace one waterfall history. Input columns are frequency bins; input row
 * 0 is oldest and the final row is newest. The visible texture stores 160
 * native pooled-RF time columns, with frequency on Y and time on X. A larger
 * 256-column history backs the paused review view.
 */
bool rf_ui_update_waterfall(uint32_t channel_index,
                            const uint8_t * intensities,
                            size_t row_stride,
                            uint32_t row_count,
                            uint32_t column_count);

/**
 * Push one new frequency slice into the time history. row_index is retained
 * for ABI compatibility; the newest slice is displayed at the right edge.
 */
bool rf_ui_update_waterfall_row(uint32_t channel_index,
                                uint32_t row_index,
                                const uint8_t * intensities,
                                size_t column_count);

/** Append several oldest-to-newest frequency slices without moving history. */
bool rf_ui_update_waterfall_rows(uint32_t channel_index,
                                 const uint8_t * intensities,
                                 size_t row_stride,
                                 uint32_t row_count,
                                 uint32_t column_count);

/**
 * Advance RF time with explicit unavailable-data columns. The visible write
 * count is capped to one complete history while transport diagnostics retain
 * the full loss count. These columns are not interpreted as zero intensity.
 */
bool rf_ui_append_waterfall_gap_columns(uint32_t channel_index,
                                        uint32_t column_count);

/**
 * Present the newest selected-channel ring head. Data ingestion only writes
 * the dual-mapped RGB565 ring; this call bounds full-image invalidation to the
 * display cadence. Returns true when an invalidation was submitted.
 */
bool rf_ui_present_waterfall(void);
/** Return true while a bounded live waterfall build or deferred render is active. */
bool rf_ui_waterfall_build_busy(void);

/** CLUT4 Layer 2 contract. All calls belong to the LVGL owner thread. */
bool rf_ui_waterfall_overlay_palette_get(const uint32_t ** palette,
                                         uint32_t * color_count);
bool rf_ui_waterfall_overlay_prepare_frame(
    rf_ui_waterfall_overlay_frame_t * frame);
void rf_ui_waterfall_overlay_frame_submitted(uint32_t generation);
void rf_ui_waterfall_overlay_frame_latched(uint32_t generation);
void rf_ui_waterfall_overlay_set_enabled(bool enabled);
void rf_ui_waterfall_overlay_fail(uint32_t error);
bool rf_ui_waterfall_overlay_requested(void);
bool rf_ui_waterfall_overlay_disable_ready(void);

bool rf_ui_update_channel_metrics(uint32_t channel_index,
                                  const rf_ui_channel_metrics_t * metrics);
bool rf_ui_update_detection(uint32_t detection_index,
                            const rf_ui_detection_t * detection);

/**
 * Stage one channel's raw RF boxes after the matching 16-column waterfall
 * window has been ingested. No geometry becomes visible or persistent until
 * rf_ui_apply_fusion_round() supplies an exact session/window identity for the
 * same four-frequency round.
 */
bool rf_ui_update_rf_boxes(uint32_t channel_index,
                           const rf_ui_rf_box_t * boxes,
                           size_t box_count,
                           uint32_t session_id,
                           uint32_t window_sequence);

/** Resolve staged boxes using one explicitly identified V27 fusion output. */
void rf_ui_apply_fusion_round(const rf_ui_fusion_round_t * round);

/** Resolve at most one staged batch inside the existing VSync SDRAM budget. */
bool rf_ui_box_fusion_step(void);

/** Drop unresolved geometry while preserving already committed history. */
void rf_ui_reset_rf_box_fusion(void);

/** Every completed center updates a small selector pulse, even when its plot is not selected. */
void rf_ui_mark_channel_result(uint32_t channel_index,
                               uint32_t window_sequence,
                               uint8_t confidence_percent);
void rf_ui_force_channel_result_redraw(uint32_t channel_index);
void rf_ui_set_model_placeholder(bool placeholder);

/** Set the acquisition/scan-rate text in tenths of hertz (82 -> "8.2 Hz"). */
void rf_ui_set_scan_rate_x10(uint16_t rate_x10);

/** Update measured transport rate without changing the sample-derived RF time axis. */
void rf_ui_set_global_tile_rate_millihz(uint32_t global_rate_millihz);

/**
 * Update the always-visible rendering telemetry overlay.
 * Frequencies are expressed in millihertz. Render time is the maximum observed
 * during the latest profiling interval, in microseconds. Underflows are the
 * cumulative GLCDC underflow count since startup.
 * Unchanged values do not cause LVGL text updates or invalidation.
 */
void rf_ui_set_render_metrics(uint32_t panel_millihz,
                               uint32_t presented_millihz,
                               uint32_t render_max_us,
                               uint32_t underflow_count);

#ifdef __cplusplus
}
#endif

#endif /* RF_UI_H */
