#include "lvgl_app.h"
#include "display_bringup.h"
#include "gt911_touch.h"
#include "lvgl.h"
#include <string.h>
#include "framework/activity_service.h"
#include "framework/display_app.h"
#include "framework/system_protocol.h"
#include "ui/rf_ui.h"

/*
 * The browser UI prototype uses the same 1024 x 600 coordinate system as the
 * panel.  Keep the page geometry fixed so a touch coordinate maps directly to
 * the display and so the direct LVGL buffers do not resize at runtime.
 */
#define UI_SCREEN_WIDTH             (DISPLAY_HSIZE_INPUT0)
#define UI_SCREEN_HEIGHT            (DISPLAY_VSIZE_INPUT0)
#define UI_HEADER_HEIGHT            (52U)
#define UI_FOOTER_HEIGHT            (56U)
#define UI_CONTENT_HEIGHT           (UI_SCREEN_HEIGHT - UI_HEADER_HEIGHT - UI_FOOTER_HEIGHT)
#define UI_CARD_WIDTH               (494U)
#define UI_CARD_HEIGHT              (228U)
#define UI_CARD_X0                  (12U)
#define UI_CARD_X1                  (518U)
#define UI_CARD_Y0                  (12U)
#define UI_CARD_Y1                  (252U)
#define UI_PLOT_X                   (30U)
#define UI_SPECTRUM_PLOT_Y          (44U)
#define UI_SPECTRUM_WIDTH           (450U)
#define UI_SPECTRUM_HEIGHT          (118U)
#define UI_WATERFALL_PLOT_Y         (40U)
#define UI_WATERFALL_WIDTH          (450U)
#define UI_WATERFALL_HEIGHT         (148U)
#define UI_WATERFALL_RING_WIDTH     (448U)
#define UI_WATERFALL_STORAGE_WIDTH  (UI_WATERFALL_RING_WIDTH * 2U)
#define UI_WATERFALL_STRIDE_BYTES   (UI_WATERFALL_STORAGE_WIDTH * 2U)
#define UI_WATERFALL_STORAGE_PIXELS \
    (UI_WATERFALL_HEIGHT * UI_WATERFALL_STORAGE_WIDTH)
#define UI_SPECTRUM_POINTS          (RA8P1_DISPLAY_SPECTRUM_BINS)
#define UI_DISPLAY_REFRESH_PERIOD_MS (5U)
#define UI_TOUCH_POLL_PERIOD_MS      (10U)
#define UI_DATA_PERIOD_MS           (10U)  /* Poll the shared data stream at 100 Hz. */
#define UI_SPECTRUM_UPDATE_PERIOD_MS (100U) /* Bound full software-canvas redraws. */
#define UI_WATERFALL_PRESENT_PERIOD_MS (5U) /* Submit new RF rows before the next VSync. */
#define UI_RF_OVERLAY_STABLE_VSYNCS    (8U)
#define UI_RF_OVERLAY_ENABLE_CLEAN_VSYNCS (8U)
#define UI_RF_OVERLAY_MONITOR_VSYNCS   (2812U)
#define UI_TEXT_UPDATE_PERIOD_MS    (100U)
#define UI_MASK_UPDATE_PERIOD_MS    (100U)
#define UI_GLCDC_SUBMIT_RETRY_LIMIT (4U)
#define UI_DEFERRED_RESYNC_MAX_BYTES (32U * 1024U)
#define UI_SDRAM_WORK_BUDGET_US       (8000U)
#define UI_SDRAM_WORK_GUARD_US        (1500U)
#define UI_CHANNEL_SWITCH_MAX_STEPS_PER_VSYNC (12U)
#define UI_CHANNEL_COUNT            (RA8P1_CENTER_COUNT)
#define UI_CLASS_COUNT              (4U)
#define UI_SINGLE_FLOW_ENABLED      (1U)
#define UI_MASK_PREVIEW_WIDTH       (256U)
#define UI_MASK_PREVIEW_HEIGHT      (128U)
#define UI_WATERFALL_BOX_WINDOW_WIDTH (RA8P1_DISPLAY_TILE_HEIGHT)
#define UI_TILE_QUEUE_CAPACITY       (RA8P1_DISPLAY_TILE_SLOT_COUNT)
#define UI_TILE_LOGICAL_RUN_SLOTS    (2U)
#define UI_CPU0_CYCLES_PER_MS       (1000000U)
#define UI_WATERFALL_OVERLAY_CHANNEL (0U)
#define UI_WATERFALL_OVERLAY_BATCH_TILES (2U)
#define UI_WATERFALL_OVERLAY_X       (UI_CARD_X0 + UI_PLOT_X)
#define UI_WATERFALL_OVERLAY_Y       (UI_HEADER_HEIGHT + UI_CARD_Y0 + UI_WATERFALL_PLOT_Y)
#define UI_WATERFALL_OVERLAY_BATCH_COLUMNS \
    (RA8P1_DISPLAY_TILE_HEIGHT * UI_WATERFALL_OVERLAY_BATCH_TILES)
#define UI_WATERFALL_GLCDC_READ_PIXELS \
    (((UI_WATERFALL_WIDTH * sizeof(uint16_t) + 63U) / 64U) * 32U)
#define UI_GLCDC_BURST_BYTES              (64U)
#define UI_GLCDC_DIMENSION_MASK           (0x7FFU)
#define UI_GLCDC_LINE_OFFSET_MASK         (0xFFFFU)
#define UI_GLCDC_TRANSFER_COUNT_MASK      (0xFFFFU)
#define UI_GLCDC_BLEND_ON_LOWER_LAYER     (3U)
#define UI_GLCDC_RECT_ALPHA_ENABLE        (1U << 12)

/* A GLCDC RGB565 line is fetched in 64-byte bursts.  The ring advances by
 * two 16-column tiles (32 pixels = 64 bytes), so its circumference must also
 * be a multiple of 32 pixels.  The visible image is 450 pixels wide; the
 * final two pixels are a harmless repeat of the start of the ring. */
typedef char ui_waterfall_ring_width_is_tile_aligned[
    ((UI_WATERFALL_RING_WIDTH % (RA8P1_DISPLAY_TILE_HEIGHT *
                                 UI_WATERFALL_OVERLAY_BATCH_TILES)) == 0U) ? 1 : -1];
typedef char ui_waterfall_stride_is_glcdc_aligned[
    ((UI_WATERFALL_STRIDE_BYTES % 64U) == 0U) ? 1 : -1];
typedef char ui_waterfall_glcdc_read_span_fits[
    (((UI_WATERFALL_RING_WIDTH - UI_WATERFALL_OVERLAY_BATCH_COLUMNS) +
      UI_WATERFALL_GLCDC_READ_PIXELS) <= UI_WATERFALL_STORAGE_WIDTH) ? 1 : -1];
typedef char ui_tile_logical_run_slots_must_be_power_of_two[
    ((UI_TILE_LOGICAL_RUN_SLOTS != 0U) &&
     ((UI_TILE_LOGICAL_RUN_SLOTS & (UI_TILE_LOGICAL_RUN_SLOTS - 1U)) == 0U)) ? 1 : -1];
typedef char ui_spectrum_bins_must_match_cross_core_stream[
    (RF_UI_SPECTRUM_BINS == RA8P1_DISPLAY_SPECTRUM_BINS) ? 1 : -1];
typedef char ui_rf_coordinate_scale_must_match_cross_core_stream[
    (RF_UI_RF_COORD_SCALE == RA8P1_DISPLAY_RF_COORD_SCALE) ? 1 : -1];
typedef char ui_rf_box_count_must_match_cross_core_stream[
    (RF_UI_MAX_RF_BOXES == RA8P1_DISPLAY_MAX_BOXES) ? 1 : -1];
typedef char ui_fusion_identity_count_must_match_centers[
    (RF_V13_DISPLAY_IDENTITY_COUNT == RA8P1_CENTER_COUNT) ? 1 : -1];

#define UI_COLOR_BACKGROUND         lv_color_hex(0x111416)
#define UI_COLOR_HEADER             lv_color_hex(0x15191B)
#define UI_COLOR_CARD               lv_color_hex(0x1A1F21)
#define UI_COLOR_CARD_ALT           lv_color_hex(0x191E20)
#define UI_COLOR_BORDER             lv_color_hex(0x343B3E)
#define UI_COLOR_GRID               lv_color_hex(0x30383B)
#define UI_COLOR_TEXT               lv_color_hex(0xF1F5F4)
#define UI_COLOR_MUTED              lv_color_hex(0x8F9A9D)
#define UI_COLOR_AXIS               lv_color_hex(0x788487)
#define UI_COLOR_ACCENT             lv_color_hex(0x35D8D0)
#define UI_COLOR_SUCCESS            lv_color_hex(0x91D45B)
#define UI_COLOR_WARNING            lv_color_hex(0xF4B84A)
#define UI_COLOR_DANGER             lv_color_hex(0xF06A5E)
#define UI_COLOR_PINK               lv_color_hex(0xE17AC6)

typedef enum e_ui_page
{
    UI_PAGE_MONITOR = 0,
    UI_PAGE_RECOGNITION = 1
} ui_page_t;

typedef struct st_ui_channel_info
{
    const char * id;
    const char * band;
    uint32_t center_mhz;
    uint8_t occupancy;
    int16_t peak_dbfs;
    int16_t noise_dbfs;
} ui_channel_info_t;

typedef struct st_ui_drone_info
{
    const char * name;
    const char * short_name;
    const char * state;
    uint8_t confidence;
    uint8_t channel_index;
    uint32_t color;
} ui_drone_info_t;

typedef struct st_ui_clipped_box
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} ui_clipped_box_t;

typedef struct st_ui_tile_logical_run
{
    bool valid;
    uint32_t session_id;
    uint32_t window_sequence;
    uint8_t last_time_start;
} ui_tile_logical_run_t;

typedef struct st_ui_flow_rf_box_batch
{
    bool pending;
    uint32_t session_id;
    uint32_t window_sequence;
    uint8_t count;
    rf_ui_rf_box_t boxes[RF_UI_MAX_RF_BOXES];
} ui_flow_rf_box_batch_t;

typedef enum e_ui_rf_overlay_state
{
    UI_RF_OVERLAY_OFF = 0,
    UI_RF_OVERLAY_WAIT_STABLE,
    UI_RF_OVERLAY_CLUT1_WAIT,
    UI_RF_OVERLAY_CLUT0_WAIT,
    UI_RF_OVERLAY_ENABLE_WAIT,
    UI_RF_OVERLAY_ACTIVE,
    UI_RF_OVERLAY_DISABLE_WAIT,
} ui_rf_overlay_state_t;

static const ui_channel_info_t g_channels[UI_CHANNEL_COUNT] =
{
    { "CH1", "2.4G-A", 2420U, 0U, 0, 0 },
    { "CH2", "2.4G-B", 2464U, 0U, 0, 0 },
    { "CH3", "5.8G-A", 5760U, 0U, 0, 0 },
    { "CH4", "5.8G-B", 5816U, 0U, 0, 0 }
};

static const ui_drone_info_t g_drones[UI_CLASS_COUNT] =
{
    { "DJI MINI 3 PRO", "MINI3", "PLACEHOLDER", 0U, 0U, 0x35D8D0U },
    { "XIAOBAWANG", "XIAOBW", "PLACEHOLDER", 0U, 0U, 0xF4B84AU },
    { "AT9S", "AT9S", "PLACEHOLDER", 0U, 0U, 0x91D45BU },
    { "YUNZHUO T12", "T12", "PLACEHOLDER", 0U, 0U, 0xE17AC6U }
};

/* The spectrum work buffers stay in CPU1's uncached on-chip RAM.  Keeping the
 * high-write trace generation off SDRAM leaves the external bus to GLCDC and
 * the two display framebuffers.  Waterfall history remains in SDRAM because
 * it is larger and is consumed as compact column blocks. */
static uint16_t g_spectrum_buffer[UI_CHANNEL_COUNT][UI_SPECTRUM_WIDTH * UI_SPECTRUM_HEIGHT]
    BSP_ALIGN_VARIABLE(64) BSP_PLACE_IN_SECTION(".ram_noinit_nocache");
static uint16_t g_waterfall_buffer[UI_CHANNEL_COUNT][UI_WATERFALL_STORAGE_PIXELS]
    BSP_ALIGN_VARIABLE(64) BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".sdram_noinit");
static uint16_t g_mask_buffer[UI_MASK_PREVIEW_WIDTH * UI_MASK_PREVIEW_HEIGHT]
    BSP_ALIGN_VARIABLE(64) BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".sdram_noinit");
static uint16_t g_spectrum_line_y[UI_SPECTRUM_WIDTH];
static uint16_t g_heat_color_lut[256U];

static lv_display_t * g_lvgl_display;
static lv_obj_t * g_monitor_page;
static lv_obj_t * g_recognition_page;
static lv_obj_t * g_spectrum_canvas[UI_CHANNEL_COUNT];
static lv_obj_t * g_waterfall_canvas[UI_CHANNEL_COUNT];
static lv_image_dsc_t g_waterfall_image[UI_CHANNEL_COUNT];
static lv_obj_t * g_waterfall_boxes[UI_CHANNEL_COUNT][RA8P1_DISPLAY_MAX_BOXES];
static lv_obj_t * g_channel_center_label[UI_CHANNEL_COUNT];
static lv_obj_t * g_channel_meta_label[UI_CHANNEL_COUNT];
static lv_obj_t * g_channel_occupancy_label[UI_CHANNEL_COUNT];
static lv_obj_t * g_waterfall_title_label[UI_CHANNEL_COUNT];
static lv_obj_t * g_header_title;
static lv_obj_t * g_header_subtitle;
static lv_obj_t * g_monitor_status_label;
static lv_obj_t * g_analysis_status_label;
static lv_obj_t * g_mask_canvas;
static lv_obj_t * g_class_tag[UI_CLASS_COUNT];
static lv_obj_t * g_class_state_label[UI_CLASS_COUNT];
static lv_obj_t * g_class_score_label[UI_CLASS_COUNT];
static lv_obj_t * g_status_dot;
static lv_obj_t * g_status_label;
static lv_obj_t * g_fps_label;
static lv_obj_t * g_demo_badge;
static lv_obj_t * g_demo_badge_label;
static lv_obj_t * g_run_button;
static lv_obj_t * g_run_button_label;
static lv_obj_t * g_drone_status_strip;
static lv_obj_t * g_monitor_nav;
static lv_obj_t * g_recognition_nav;
static lv_obj_t * g_monitor_nav_line;
static lv_obj_t * g_recognition_nav_line;
static lv_obj_t * g_monitor_nav_label;
static lv_obj_t * g_recognition_nav_label;
static lv_timer_t * g_data_timer;
static lv_indev_t * g_touch_input;
static display_runtime_cfg_t g_waterfall_overlay_cfg;
static display_clut_cfg_t g_rf_overlay_clut_cfg;
static rf_ui_waterfall_overlay_frame_t g_rf_overlay_frame;

static bool g_touch_available;
static bool g_touch_pressed;
static uint32_t g_touch_owner_poll_tick;
static bool g_waterfall_running = true;
static bool g_waterfall_overlay_enabled;
static bool g_waterfall_overlay_pending;
static uint32_t g_waterfall_overlay_line_event;
static ui_rf_overlay_state_t g_rf_overlay_state;
static bool g_rf_overlay_failed;
static bool g_rf_overlay_present_pending;
static bool g_rf_overlay_pven_wait_recorded;
static uint32_t g_rf_overlay_operation_line_event;
static uint32_t g_rf_overlay_last_line_event;
static uint32_t g_rf_overlay_last_underflows;
static uint32_t g_rf_overlay_stable_vsyncs;
static uint32_t g_rf_overlay_enable_clean_vsyncs;
static uint32_t g_rf_overlay_last_underflow_line_event;
static bool g_rf_overlay_enable_underflow_tolerated;
static bool g_rf_overlay_underflow_line_valid;
static uint32_t g_rf_overlay_pending_generation;
static uint32_t g_rf_overlay_monitor_start_line;
static uint32_t g_rf_overlay_monitor_start_underflows;
static uint32_t g_rf_overlay_monitor_start_layer2_underflows;
static bool g_live_text_dirty;
static ui_page_t g_active_page = UI_PAGE_MONITOR;
static bool g_flush_pending;
static bool g_flush_content_pending;
static bool g_spectrum_content_dirty;
static uint32_t g_spectrum_dirty_mask;
static uint32_t g_last_spectrum_update_tick;
static bool g_spectrum_present_valid;
static uint32_t g_last_waterfall_update_tick;
static bool g_waterfall_present_valid;
static bool g_waterfall_visual_dirty;
static uint32_t g_spectrum_rendered_mask;
static bool g_content_generation_pending;
static uint32_t g_waterfall_rendered_session;
static uint32_t g_waterfall_rendered_sequence;
static uint32_t g_waterfall_ring_head[UI_CHANNEL_COUNT];
static uint32_t g_tile_queue_head;
static uint32_t g_tile_queue_count;
static ra8p1_display_tile_payload_t g_tile_queue[UI_TILE_QUEUE_CAPACITY];
static uint16_t g_touch_x;
static uint16_t g_touch_y;
static uint32_t g_signal_phase;
static uint32_t g_noise_seed = 0x7A5C39E1U;
static ra8p1_display_frame_t g_live_signal_frame;
static bool g_live_signal_valid;
static bool g_visibility_frame_pending;
static bool g_visibility_flush_armed;
static bool g_visibility_vsync_pending;
static bool g_visibility_frame_presented;
static uint32_t g_visibility_session_id;
static uint32_t g_visibility_sequence;
static uint32_t g_visibility_window_sequence;
static uint32_t g_visibility_vsync_session_id;
static uint32_t g_visibility_vsync_sequence;
static uint32_t g_visibility_vsync_window_sequence;
static uint32_t g_visibility_presented_session_id;
static uint32_t g_visibility_presented_sequence;
static uint32_t g_visibility_presented_window_sequence;
static ra8p1_display_frame_t g_center_frames[RA8P1_CENTER_COUNT];
static uint32_t g_center_valid_mask;
static ra8p1_system_telemetry_t g_live_telemetry;
static bool g_live_telemetry_valid;
static bool g_mask_dirty;
static uint32_t g_last_mask_update_tick;
static uint32_t g_last_text_update_tick;
static bool g_result_rate_valid;
static uint32_t g_result_rate_publish_tick;
static uint32_t g_result_rate_frame_count;
static uint32_t g_result_rate_inference_count;
static uint32_t g_window_rate_x100;
static uint32_t g_inference_rate_x100;
static bool g_tile_rate_valid;
static uint32_t g_tile_rate_tick_ms;
static uint32_t g_tile_rate_count;
static uint32_t g_tile_rate_x100;
static uint32_t g_flush_line_event;
static uint32_t g_sdram_work_line_event;
static bool g_deferred_resync_observed;
static uint32_t g_presented_frame_count;
static uint32_t g_fps_last_tick_ms;
static uint32_t g_fps_last_frame_count;
static uint32_t g_fps_last_underflow_count;
static uint32_t g_presented_fps_millihz;
static volatile uint32_t g_ui_content_frame_count;
static uint32_t g_content_last_tick_ms;
static uint32_t g_content_last_frame_count;
static uint32_t g_content_fps_millihz;
static uint32_t g_underflow_rate_millihz;
static uint32_t g_lvgl_refresh_start_cycles;
static uint32_t g_lvgl_refresh_max_cycles;
static uint32_t g_lvgl_refresh_count;
static uint64_t g_lvgl_refresh_cycles_total;
static uint32_t g_lvgl_flush_wait_max_cycles;
static uint32_t g_lvgl_flush_wait_count;

volatile lvgl_app_input_diag_t g_lvgl_app_input_diag = {
    .magic = LVGL_APP_INPUT_DIAG_MAGIC,
    .version = LVGL_APP_INPUT_DIAG_VERSION,
    .poll_period_ms = UI_TOUCH_POLL_PERIOD_MS,
};
static uint64_t g_lvgl_flush_wait_cycles_total;
static volatile uint32_t g_lvgl_tick_ms;

/* Cumulative counters are intentionally exported for non-invasive J-Link
 * profiling.  They isolate chart-data generation from LVGL/VSync time. */
static volatile uint32_t g_ui_waterfall_gen_cycles;
static volatile uint32_t g_ui_waterfall_gen_max_cycles;
static volatile uint32_t g_ui_waterfall_columns_generated;
static volatile uint32_t g_ui_waterfall_tiles_consumed;
static volatile uint32_t g_ui_waterfall_tiles_dropped;
static uint32_t g_tile_last_received_session;
static uint32_t g_tile_last_received_sequence;
static uint32_t g_tile_session_center_mask;
static uint32_t g_tile_center_valid_mask;
static uint32_t g_tile_center_last_sequence[RA8P1_CENTER_COUNT];
static bool g_tile_discontinuity_active[RA8P1_CENTER_COUNT];
static bool g_tile_logical_session_valid[RA8P1_CENTER_COUNT];
static uint32_t g_tile_logical_session[RA8P1_CENTER_COUNT];
static bool g_tile_logical_max_window_valid[RA8P1_CENTER_COUNT];
static uint32_t g_tile_logical_max_window[RA8P1_CENTER_COUNT];
static ui_tile_logical_run_t
    g_tile_logical_runs[RA8P1_CENTER_COUNT][UI_TILE_LOGICAL_RUN_SLOTS];
static ui_flow_rf_box_batch_t
    g_flow_rf_box_batches[RA8P1_CENTER_COUNT][UI_TILE_LOGICAL_RUN_SLOTS];
static volatile uint32_t g_ui_spectrum_gen_cycles;
static volatile uint32_t g_ui_spectrum_gen_max_cycles;
static volatile uint32_t g_ui_spectrum_redraws;
static bool g_content_measurement_enabled;
static void ui_waterfall_page_event(lv_event_t * event);
static void ui_monitor_page_event(lv_event_t * event);
static uint32_t ui_frame_center_index(const ra8p1_display_frame_t * frame);

static void ui_tile_logical_center_reset(uint32_t center, uint32_t session_id)
{
    memset(g_tile_logical_runs[center], 0,
           sizeof(g_tile_logical_runs[center]));
    g_tile_logical_session_valid[center] = true;
    g_tile_logical_session[center] = session_id;
    g_tile_logical_max_window_valid[center] = false;
    g_tile_logical_max_window[center] = 0U;
    g_tile_discontinuity_active[center] = false;
}

static void ui_tile_logical_history_reset(void)
{
    memset(g_tile_discontinuity_active, 0,
           sizeof(g_tile_discontinuity_active));
    memset(g_tile_logical_session_valid, 0,
           sizeof(g_tile_logical_session_valid));
    memset(g_tile_logical_session, 0, sizeof(g_tile_logical_session));
    memset(g_tile_logical_max_window_valid, 0,
           sizeof(g_tile_logical_max_window_valid));
    memset(g_tile_logical_max_window, 0,
           sizeof(g_tile_logical_max_window));
    memset(g_tile_logical_runs, 0, sizeof(g_tile_logical_runs));
    memset(g_flow_rf_box_batches, 0, sizeof(g_flow_rf_box_batches));
}

static bool ui_tile_history_reset_required(
    const ra8p1_display_tile_payload_t * tile,
    uint32_t gap_columns)
{
    const uint32_t center = tile->center_index;
    int32_t matching_slot = -1;
    bool reset_history = false;

    if (!g_tile_logical_session_valid[center] ||
        (g_tile_logical_session[center] != tile->session_id))
    {
        /* CPU0 uses a fresh display session for normal capture transactions,
         * including planned center rotation. Session identity still scopes
         * row matching, but is not itself evidence of lost RF history. */
        ui_tile_logical_center_reset(center, tile->session_id);
    }

    for (uint32_t slot = 0U; slot < UI_TILE_LOGICAL_RUN_SLOTS; ++slot)
    {
        const ui_tile_logical_run_t * run = &g_tile_logical_runs[center][slot];
        if (run->valid &&
            (run->session_id == tile->session_id) &&
            (run->window_sequence == tile->window_sequence))
        {
            matching_slot = (int32_t)slot;
            break;
        }
    }

    if (matching_slot >= 0)
    {
        const ui_tile_logical_run_t * run =
            &g_tile_logical_runs[center][(uint32_t)matching_slot];
        if ((tile->novel_time_start <= run->last_time_start) ||
            (gap_columns != 0U))
        {
            /* A same-window row restart is an SDR RETRY even when the source
             * did not set DISCONTINUITY. If its first rows were overwritten,
             * a transport gap is indistinguishable from that retry, so clear
             * conservatively instead of joining two RF attempts. */
            reset_history = true;
            ui_tile_logical_center_reset(center, tile->session_id);
            matching_slot = -1;
        }
    }
    else if (g_tile_logical_max_window_valid[center] &&
             ((int32_t)(tile->window_sequence -
                        g_tile_logical_max_window[center]) < 0))
    {
        /* A whole-session retry restarts logical windows at zero while the
         * transport sequence remains monotonic. */
        reset_history = true;
        ui_tile_logical_center_reset(center, tile->session_id);
    }

    if (matching_slot < 0)
    {
        uint32_t replacement = UI_TILE_LOGICAL_RUN_SLOTS;
        for (uint32_t slot = 0U; slot < UI_TILE_LOGICAL_RUN_SLOTS; ++slot)
        {
            const ui_tile_logical_run_t * run =
                &g_tile_logical_runs[center][slot];
            if (!run->valid)
            {
                replacement = slot;
                break;
            }
            if ((replacement == UI_TILE_LOGICAL_RUN_SLOTS) &&
                (run->last_time_start == (RA8P1_DISPLAY_TILE_HEIGHT - 1U)))
            {
                replacement = slot;
            }
        }
        if (replacement == UI_TILE_LOGICAL_RUN_SLOTS)
        {
            replacement = tile->window_sequence &
                          (UI_TILE_LOGICAL_RUN_SLOTS - 1U);
        }
        matching_slot = (int32_t)replacement;
    }

    ui_tile_logical_run_t * run =
        &g_tile_logical_runs[center][(uint32_t)matching_slot];
    run->valid = true;
    run->session_id = tile->session_id;
    run->window_sequence = tile->window_sequence;
    run->last_time_start = tile->novel_time_start;

    if (!g_tile_logical_max_window_valid[center] ||
        ((int32_t)(tile->window_sequence -
                   g_tile_logical_max_window[center]) > 0))
    {
        g_tile_logical_max_window_valid[center] = true;
        g_tile_logical_max_window[center] = tile->window_sequence;
    }

    if ((tile->flags & RA8P1_DISPLAY_FLAG_DISCONTINUITY) != 0U)
    {
        if (!g_tile_discontinuity_active[center])
        {
            reset_history = true;
        }
        g_tile_discontinuity_active[center] = true;
    }
    else
    {
        g_tile_discontinuity_active[center] = false;
    }
    return reset_history;
}

static bool ui_frame_identity_matches(const ra8p1_display_frame_t *frame,
                                      uint32_t session_id,
                                      uint32_t sequence,
                                      uint32_t window_sequence)
{
    return (frame != NULL) &&
           (frame->session_id == session_id) &&
           (frame->sequence == sequence) &&
           (frame->analysis.window_sequence == window_sequence);
}

static void ui_visibility_reset(void)
{
    g_visibility_frame_pending = false;
    g_visibility_flush_armed = false;
    g_visibility_vsync_pending = false;
    g_visibility_frame_presented = false;
    g_visibility_session_id = 0U;
    g_visibility_sequence = 0U;
    g_visibility_window_sequence = 0U;
    g_visibility_vsync_session_id = 0U;
    g_visibility_vsync_sequence = 0U;
    g_visibility_vsync_window_sequence = 0U;
    g_visibility_presented_session_id = 0U;
    g_visibility_presented_sequence = 0U;
    g_visibility_presented_window_sequence = 0U;
}

static bool ui_visibility_render_required(void)
{
    return g_visibility_frame_pending &&
           !g_visibility_frame_presented &&
           !g_visibility_vsync_pending &&
           !g_visibility_flush_armed &&
           g_live_signal_valid &&
           ui_frame_identity_matches(&g_live_signal_frame,
                                     g_visibility_session_id,
                                     g_visibility_sequence,
                                     g_visibility_window_sequence);
}

static void ui_visibility_content_prepared(void)
{
    if (ui_visibility_render_required())
    {
        /* The following LVGL flush contains this CPU0 result. It becomes
         * externally visible only after BufferChange succeeds and VSync fires. */
        g_visibility_flush_armed = true;
    }
}

static void ui_visibility_retry_after_flush_failure(void)
{
    if (!g_visibility_flush_armed)
    {
        return;
    }

    g_visibility_flush_armed = false;
    if (UI_SINGLE_FLOW_ENABLED)
    {
        uint32_t center = ui_frame_center_index(&g_live_signal_frame);
        const uint32_t selected_center = rf_ui_get_selected_channel();
        if (center >= RF_UI_CHANNEL_COUNT)
        {
            center = selected_center;
        }
        rf_ui_force_channel_result_redraw(center);
        if (center == selected_center)
        {
            /* Re-submit the selected spectrum on the next application step.
             * The identity is armed only after that real invalidation. */
            g_spectrum_present_valid = false;
        }
        else
        {
            /* The retry invalidation is the non-selected selector pulse. */
            ui_visibility_content_prepared();
        }
        return;
    }
    if (g_active_page == UI_PAGE_MONITOR)
    {
        if (g_live_signal_frame.analysis.center_index < UI_CHANNEL_COUNT)
        {
            g_spectrum_dirty_mask |=
                (1UL << g_live_signal_frame.analysis.center_index);
        }
        g_spectrum_content_dirty = true;
    }
    else
    {
        g_mask_dirty = true;
    }
}

static uint16_t ui_rgb565(uint32_t red, uint32_t green, uint32_t blue)
{
    return (uint16_t) ((((red & 0xF8U) << 8U) |
                        ((green & 0xFCU) << 3U) |
                       (blue >> 3U)));
}

static uint16_t ui_hex_rgb565(uint32_t color)
{
    return ui_rgb565((color >> 16U) & 0xFFU,
                     (color >> 8U) & 0xFFU,
                     color & 0xFFU);
}

static uint32_t ui_abs_diff(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

static uint32_t ui_noise(uint32_t channel, uint32_t x, uint32_t y, uint32_t phase)
{
    uint32_t value = g_noise_seed ^ (channel * 0x9E3779B9U) ^
                      (x * 0x45D9F3BU) ^ (y * 0x27D4EB2DU) ^ phase;
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    return value;
}

static uint8_t ui_add_peak(uint8_t level,
                           uint32_t coordinate,
                           uint32_t center,
                           uint32_t width,
                           uint32_t height)
{
    const uint32_t distance = ui_abs_diff(coordinate, center);
    if (distance < width)
    {
        const uint32_t contribution = ((width - distance) * height) / width;
        const uint32_t sum = (uint32_t) level + contribution;
        return (uint8_t) ((sum > 255U) ? 255U : sum);
    }
    return level;
}

static uint8_t ui_synthetic_spectrum_level(uint32_t channel, uint32_t bin)
{
    uint32_t phase = g_signal_phase / 3U;
    uint8_t level = (uint8_t) (35U + (ui_noise(channel, bin, 11U, phase) % 15U));

    if (channel == 0U)
    {
        level = ui_add_peak(level, bin, 40U, 13U, 168U);
        level = ui_add_peak(level, bin, 91U, 7U, 50U);
    }
    else if (channel == 1U)
    {
        level = ui_add_peak(level, bin, 91U, 17U, 195U);
        level = ui_add_peak(level, bin, 32U, 5U, 35U);
    }
    else if (channel == 2U)
    {
        level = ui_add_peak(level, bin, 65U, 12U, 156U);
        level = ui_add_peak(level, bin, 23U, 4U, 38U);
    }
    else
    {
        level = ui_add_peak(level, bin, 28U, 8U, 85U);
        level = ui_add_peak(level, bin, 83U, 20U, 126U);
        level = ui_add_peak(level, bin, 108U, 5U, 58U);
    }

    return level;
}

static const ra8p1_display_frame_t * ui_center_frame(uint32_t center_index)
{
    const ra8p1_display_frame_t * frame;

    if ((center_index >= RA8P1_CENTER_COUNT) ||
        (rf_ui_is_focus_mode() &&
         (center_index != rf_ui_get_selected_channel())) ||
        ((g_center_valid_mask & (1UL << center_index)) == 0U))
    {
        return NULL;
    }
    frame = &g_center_frames[center_index];
    return ((frame->channel_mask & RA8P1_RF_CHANNEL_A_MASK) != 0U) ? frame : NULL;
}

static uint8_t ui_channel_level(uint32_t channel, uint32_t bin)
{
    const ra8p1_display_frame_t * frame = ui_center_frame(channel);
    if (frame != NULL)
    {
        return frame->spectrum[0][bin % RA8P1_DISPLAY_SPECTRUM_BINS];
    }
    if (g_live_signal_valid || (g_center_valid_mask != 0U))
    {
        return 0U;
    }
    return ui_synthetic_spectrum_level(channel, bin);
}

static uint8_t ui_interpolated_spectrum_level(uint32_t channel, uint32_t x)
{
    const uint32_t scaled = x * (UI_SPECTRUM_POINTS - 1U);
    const uint32_t bin = scaled / (UI_SPECTRUM_WIDTH - 1U);
    const uint32_t remainder = scaled % (UI_SPECTRUM_WIDTH - 1U);
    const uint32_t next_bin = (bin + 1U < UI_SPECTRUM_POINTS) ? (bin + 1U) : bin;
    const uint32_t level = ui_channel_level(channel, bin);
    const uint32_t next_level = ui_channel_level(channel, next_bin);
    return (uint8_t) (((level * ((UI_SPECTRUM_WIDTH - 1U) - remainder)) +
                       (next_level * remainder)) /
                      (UI_SPECTRUM_WIDTH - 1U));
}

static uint32_t ui_channel_occupancy_percent(uint32_t channel)
{
    const ra8p1_display_frame_t * frame = ui_center_frame(channel);
    uint32_t histogram[32] = {0U};
    uint32_t cumulative = 0U;
    uint32_t noise_bucket = 0U;
    uint32_t occupied = 0U;

    if (frame == NULL)
    {
        return UINT32_MAX;
    }
    for (uint32_t bin = 0U; bin < RA8P1_DISPLAY_SPECTRUM_BINS; ++bin)
    {
        histogram[frame->spectrum[0][bin] >> 3U]++;
    }
    for (noise_bucket = 0U; noise_bucket < 32U; ++noise_bucket)
    {
        cumulative += histogram[noise_bucket];
        if (cumulative >= (RA8P1_DISPLAY_SPECTRUM_BINS / 4U))
        {
            break;
        }
    }
    const uint32_t threshold = ((noise_bucket + 3U) < 32U) ?
                               ((noise_bucket + 3U) << 3U) : 255U;
    for (uint32_t bin = 0U; bin < RA8P1_DISPLAY_SPECTRUM_BINS; ++bin)
    {
        if (frame->spectrum[0][bin] >= threshold)
        {
            occupied++;
        }
    }
    return (occupied * 100U + (RA8P1_DISPLAY_SPECTRUM_BINS / 2U)) /
           RA8P1_DISPLAY_SPECTRUM_BINS;
}

static uint8_t ui_waterfall_level(uint32_t channel,
                                  uint32_t time_column,
                                  uint32_t frequency_row)
{
    const ra8p1_display_frame_t * frame = ui_center_frame(channel);
    if (frame != NULL)
    {
        const uint32_t bin = ((UI_WATERFALL_HEIGHT - 1U - frequency_row) *
                              (UI_SPECTRUM_POINTS - 1U)) /
                             (UI_WATERFALL_HEIGHT - 1U);
        FSP_PARAMETER_NOT_USED(time_column);
        return frame->spectrum[0][bin % RA8P1_DISPLAY_SPECTRUM_BINS];
    }
    if (g_live_signal_valid || (g_center_valid_mask != 0U))
    {
        return 0U;
    }

    const uint32_t time = time_column + (g_signal_phase / 2U);
    uint32_t level = 18U + (ui_noise(channel, time_column, frequency_row, time) % 23U);

    if (channel == 0U)
    {
        const uint32_t pulse = time % 42U;
        if (pulse < 30U && ui_abs_diff(frequency_row, 97U) < 10U)
        {
            level += 122U + (9U - ui_abs_diff(frequency_row, 97U)) * 7U;
        }
    }
    else if (channel == 1U)
    {
        if ((time % 113U) < 3U)
        {
            level += (ui_abs_diff(frequency_row, 62U) < 4U) ? 155U : 20U;
        }
    }
    else if (channel == 2U)
    {
        const uint32_t pulse = time % 36U;
        if (pulse < 28U && ui_abs_diff(frequency_row, 74U) < 12U)
        {
            level += 145U + (11U - ui_abs_diff(frequency_row, 74U)) * 5U;
        }
    }
    else
    {
        const uint32_t pulse = time % 58U;
        if (pulse > 9U && pulse < 47U && ui_abs_diff(frequency_row, 108U) < 14U)
        {
            level += 82U + (13U - ui_abs_diff(frequency_row, 108U)) * 3U;
        }
    }

    return (uint8_t) ((level > 255U) ? 255U : level);
}

static uint16_t ui_heat_color(uint8_t level)
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (level < 64U)
    {
        red = 7U;
        green = 16U + (level / 3U);
        blue = 25U + (level / 2U);
    }
    else if (level < 128U)
    {
        const uint32_t t = level - 64U;
        red = 10U + (t / 5U);
        green = 48U + (t * 2U);
        blue = 72U + t;
    }
    else if (level < 192U)
    {
        const uint32_t t = level - 128U;
        red = 26U + (t * 3U);
        green = 176U + (t / 2U);
        blue = 168U - (t * 2U);
    }
    else
    {
        const uint32_t t = level - 192U;
        red = 218U + (t / 4U);
        green = (194U > (t * 2U)) ? (194U - (t * 2U)) : 35U;
        blue = (70U > (t / 2U)) ? (70U - (t / 2U)) : 10U;
    }

    return ui_rgb565(red, green, blue);
}

static void ui_init_heat_color_lut(void)
{
    for (uint32_t level = 0U; level < 256U; ++level)
    {
        g_heat_color_lut[level] = ui_heat_color((uint8_t) level);
    }
}

static uint16_t ui_heat_color_fast(uint8_t level)
{
    return g_heat_color_lut[level];
}

static uint32_t ui_class_color(uint32_t class_id)
{
    static const uint32_t colors[UI_CLASS_COUNT] =
    {
        0x35D8D0U, 0xF4B84AU, 0x91D45BU, 0xE17AC6U
    };
    return colors[class_id % UI_CLASS_COUNT];
}

static bool ui_mask_bit(uint32_t x, uint32_t y)
{
    const uint32_t index = (y * RA8P1_DISPLAY_MASK_WIDTH) + x;
    return ((g_live_signal_frame.analysis.mask_bits[index >> 3U] &
             (uint8_t)(1U << (index & 7U))) != 0U);
}

static bool ui_frame_npu_output_valid(const ra8p1_display_frame_t * frame)
{
    return (frame != NULL) &&
           (frame->analysis.npu_ready != 0U) &&
           ((frame->flags & RA8P1_DISPLAY_FLAG_MODEL_MASK_VALID) != 0U);
}

static bool ui_npu_output_valid(void)
{
    return g_live_signal_valid && ui_frame_npu_output_valid(&g_live_signal_frame);
}

static bool ui_any_npu_output_valid(void)
{
    for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
    {
        if (ui_frame_npu_output_valid(ui_center_frame(center)))
        {
            return true;
        }
    }
    return false;
}

static uint32_t ui_cached_model_flags(void)
{
    uint32_t flags = g_live_telemetry_valid ? g_live_telemetry.model_flags : 0U;
    for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
    {
        if ((g_center_valid_mask & (1UL << center)) != 0U)
        {
            flags |= g_center_frames[center].analysis.model_flags;
        }
    }
    return flags;
}

static uint32_t ui_frame_center_index(const ra8p1_display_frame_t *frame)
{
    static const uint64_t centers[RA8P1_CENTER_COUNT] = {
        RA8P1_CENTER_2420_HZ,
        RA8P1_CENTER_2464_HZ,
        RA8P1_CENTER_5760_HZ,
        RA8P1_CENTER_5816_HZ
    };
    uint64_t center_hz;

    if (frame == NULL)
    {
        return UINT32_MAX;
    }
    if (frame->analysis.center_index < RA8P1_CENTER_COUNT)
    {
        return frame->analysis.center_index;
    }
    center_hz = ((uint64_t)frame->analysis.center_frequency_high << 32U) |
                frame->analysis.center_frequency_low;
    for (uint32_t index = 0U; index < RA8P1_CENTER_COUNT; ++index)
    {
        if (center_hz == centers[index])
        {
            return index;
        }
    }
    return UINT32_MAX;
}

static void ui_hide_waterfall_boxes(void)
{
    for (uint32_t channel = 0U; channel < UI_CHANNEL_COUNT; ++channel)
    {
        for (uint32_t box_index = 0U; box_index < RA8P1_DISPLAY_MAX_BOXES; ++box_index)
        {
            if (g_waterfall_boxes[channel][box_index] != NULL)
            {
                lv_obj_add_flag(g_waterfall_boxes[channel][box_index], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static bool ui_clip_detection_box(const ra8p1_detection_box_t * box,
                                  ui_clipped_box_t * clipped)
{
    uint32_t frequency_end;
    uint32_t time_end;
    uint32_t x;
    uint32_t y;
    uint32_t x_end;
    uint32_t y_end;
    const uint8_t flags = (box == NULL) ? 0U :
        (uint8_t)(box->metadata >> RA8P1_DISPLAY_BOX_FLAGS_SHIFT);

    if ((box == NULL) || (clipped == NULL) ||
        ((flags & RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID) == 0U) ||
        (box->frequency_span_q8 == 0U) || (box->time_span_q8 == 0U))
    {
        return false;
    }

    frequency_end = (uint32_t)box->frequency_start_q8 +
                    box->frequency_span_q8;
    time_end = (uint32_t)box->time_start_q8 + box->time_span_q8;
    if (frequency_end > RA8P1_DISPLAY_RF_COORD_SCALE)
    {
        frequency_end = RA8P1_DISPLAY_RF_COORD_SCALE;
    }
    if (time_end > RA8P1_DISPLAY_RF_COORD_SCALE)
    {
        time_end = RA8P1_DISPLAY_RF_COORD_SCALE;
    }

    x = ((uint32_t)box->frequency_start_q8 *
         RA8P1_DISPLAY_MASK_WIDTH) / RA8P1_DISPLAY_RF_COORD_SCALE;
    y = ((uint32_t)box->time_start_q8 *
         RA8P1_DISPLAY_MASK_HEIGHT) / RA8P1_DISPLAY_RF_COORD_SCALE;
    x_end = (frequency_end * RA8P1_DISPLAY_MASK_WIDTH +
             RA8P1_DISPLAY_RF_COORD_SCALE - 1U) /
            RA8P1_DISPLAY_RF_COORD_SCALE;
    y_end = (time_end * RA8P1_DISPLAY_MASK_HEIGHT +
             RA8P1_DISPLAY_RF_COORD_SCALE - 1U) /
            RA8P1_DISPLAY_RF_COORD_SCALE;
    if ((x_end <= x) || (y_end <= y))
    {
        return false;
    }

    clipped->x = x;
    clipped->y = y;
    clipped->width = x_end - x;
    clipped->height = y_end - y;
    return true;
}

static void ui_update_waterfall_boxes(void)
{
    ui_hide_waterfall_boxes();
    if (g_active_page != UI_PAGE_RECOGNITION)
    {
        return;
    }

    for (uint32_t channel = 0U; channel < UI_CHANNEL_COUNT; ++channel)
    {
        const ra8p1_display_frame_t * frame = ui_center_frame(channel);
        const uint32_t box_count = (frame != NULL) &&
                                   (frame->analysis.box_count < RA8P1_DISPLAY_MAX_BOXES) ?
                                   frame->analysis.box_count :
                                   ((frame != NULL) ? RA8P1_DISPLAY_MAX_BOXES : 0U);
        if (!ui_frame_npu_output_valid(frame))
        {
            continue;
        }
        for (uint32_t box_index = 0U; box_index < box_count; ++box_index)
        {
            const ra8p1_detection_box_t * box = &frame->analysis.boxes[box_index];
            ui_clipped_box_t clipped;
            lv_obj_t * object = g_waterfall_boxes[channel][box_index];
            uint32_t frequency_end;
            uint32_t left;
            uint32_t right;
            uint32_t top;
            uint32_t bottom;

            if ((object == NULL) || !ui_clip_detection_box(box, &clipped))
            {
                continue;
            }

            /* CPU0 boxes use x=frequency and y=time.  The waterfall uses
             * x=time and y=frequency, with high frequency at the top. */
            left = (UI_WATERFALL_WIDTH - UI_WATERFALL_BOX_WINDOW_WIDTH) +
                   (clipped.y * UI_WATERFALL_BOX_WINDOW_WIDTH /
                    RA8P1_DISPLAY_MASK_HEIGHT);
            right = (UI_WATERFALL_WIDTH - UI_WATERFALL_BOX_WINDOW_WIDTH) +
                    ((clipped.y + clipped.height) * UI_WATERFALL_BOX_WINDOW_WIDTH /
                     RA8P1_DISPLAY_MASK_HEIGHT);
            frequency_end = clipped.x + clipped.width;
            top = (RA8P1_DISPLAY_MASK_WIDTH - frequency_end) *
                  UI_WATERFALL_HEIGHT / RA8P1_DISPLAY_MASK_WIDTH;
            bottom = (RA8P1_DISPLAY_MASK_WIDTH - clipped.x) *
                     UI_WATERFALL_HEIGHT / RA8P1_DISPLAY_MASK_WIDTH;

            if (left > UI_WATERFALL_WIDTH) left = UI_WATERFALL_WIDTH;
            if (right > UI_WATERFALL_WIDTH) right = UI_WATERFALL_WIDTH;
            if (top > UI_WATERFALL_HEIGHT) top = UI_WATERFALL_HEIGHT;
            if (bottom > UI_WATERFALL_HEIGHT) bottom = UI_WATERFALL_HEIGHT;
            if ((right <= left) || (bottom <= top))
            {
                continue;
            }
            lv_obj_set_pos(object,
                           (int32_t)(UI_PLOT_X + left),
                           (int32_t)(UI_WATERFALL_PLOT_Y + top));
            lv_obj_set_size(object, (int32_t)(right - left), (int32_t)(bottom - top));
            lv_obj_set_style_border_color(object,
                                          lv_color_hex(ui_class_color(box->class_id)), 0);
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_draw_mask_preview(void)
{
    const uint16_t background = ui_rgb565(12U, 18U, 22U);
    const uint16_t grid = ui_rgb565(40U, 51U, 56U);
    const uint32_t scale_x = UI_MASK_PREVIEW_WIDTH / RA8P1_DISPLAY_MASK_WIDTH;
    const uint32_t scale_y = UI_MASK_PREVIEW_HEIGHT / RA8P1_DISPLAY_MASK_HEIGHT;
    uint32_t x;
    uint32_t y;

    if ((g_mask_canvas == NULL) || (g_active_page != UI_PAGE_RECOGNITION))
    {
        return;
    }

    for (y = 0U; y < UI_MASK_PREVIEW_HEIGHT; ++y)
    {
        for (x = 0U; x < UI_MASK_PREVIEW_WIDTH; ++x)
        {
            g_mask_buffer[(y * UI_MASK_PREVIEW_WIDTH) + x] = background;
        }
    }

    for (y = 0U; y <= RA8P1_DISPLAY_MASK_HEIGHT; ++y)
    {
        const uint32_t row = (y * UI_MASK_PREVIEW_HEIGHT) /
                             RA8P1_DISPLAY_MASK_HEIGHT;
        if (row >= UI_MASK_PREVIEW_HEIGHT)
        {
            continue;
        }
        for (x = 0U; x < UI_MASK_PREVIEW_WIDTH; ++x)
        {
            g_mask_buffer[(row * UI_MASK_PREVIEW_WIDTH) + x] = grid;
        }
    }
    for (x = 0U; x <= RA8P1_DISPLAY_MASK_WIDTH; ++x)
    {
        const uint32_t column = (x * UI_MASK_PREVIEW_WIDTH) /
                                RA8P1_DISPLAY_MASK_WIDTH;
        if (column >= UI_MASK_PREVIEW_WIDTH)
        {
            continue;
        }
        for (y = 0U; y < UI_MASK_PREVIEW_HEIGHT; ++y)
        {
            g_mask_buffer[(y * UI_MASK_PREVIEW_WIDTH) + column] = grid;
        }
    }

    if (ui_npu_output_valid())
    {
        for (y = 0U; y < RA8P1_DISPLAY_MASK_HEIGHT; ++y)
        {
            for (x = 0U; x < RA8P1_DISPLAY_MASK_WIDTH; ++x)
            {
                if (ui_mask_bit(x, y))
                {
                    uint32_t py;
                    uint32_t px;
                    const uint16_t color = ui_heat_color_fast((uint8_t)(150U + ((x + y) & 63U)));
                    for (py = y * scale_y; py < ((y + 1U) * scale_y); ++py)
                    {
                        for (px = x * scale_x; px < ((x + 1U) * scale_x); ++px)
                        {
                            g_mask_buffer[(py * UI_MASK_PREVIEW_WIDTH) + px] = color;
                        }
                    }
                }
            }
        }

        for (uint32_t box_index = 0U;
             (box_index < g_live_signal_frame.analysis.box_count) &&
             (box_index < RA8P1_DISPLAY_MAX_BOXES);
             ++box_index)
        {
            const ra8p1_detection_box_t *box = &g_live_signal_frame.analysis.boxes[box_index];
            ui_clipped_box_t clipped;
            const uint16_t color = ui_hex_rgb565(ui_class_color(box->class_id));
            uint32_t left;
            uint32_t top;
            uint32_t right_exclusive;
            uint32_t bottom_exclusive;
            uint32_t right;
            uint32_t bottom;

            if (!ui_clip_detection_box(box, &clipped))
            {
                continue;
            }
            left = clipped.x * scale_x;
            top = clipped.y * scale_y;
            right_exclusive = (clipped.x + clipped.width) * scale_x;
            bottom_exclusive = (clipped.y + clipped.height) * scale_y;
            if (right_exclusive > UI_MASK_PREVIEW_WIDTH)
            {
                right_exclusive = UI_MASK_PREVIEW_WIDTH;
            }
            if (bottom_exclusive > UI_MASK_PREVIEW_HEIGHT)
            {
                bottom_exclusive = UI_MASK_PREVIEW_HEIGHT;
            }
            if ((right_exclusive <= left) || (bottom_exclusive <= top))
            {
                continue;
            }
            right = right_exclusive - 1U;
            bottom = bottom_exclusive - 1U;
            for (x = left; x <= right; ++x)
            {
                g_mask_buffer[(top * UI_MASK_PREVIEW_WIDTH) + x] = color;
                g_mask_buffer[(bottom * UI_MASK_PREVIEW_WIDTH) + x] = color;
            }
            for (y = top; y <= bottom; ++y)
            {
                g_mask_buffer[(y * UI_MASK_PREVIEW_WIDTH) + left] = color;
                g_mask_buffer[(y * UI_MASK_PREVIEW_WIDTH) + right] = color;
            }
        }
    }

    lv_obj_invalidate(g_mask_canvas);
}

static void ui_draw_spectrum(uint32_t channel)
{
    uint16_t * buffer = &g_spectrum_buffer[channel][0];
    const uint16_t background = ui_rgb565(18U, 23U, 25U);
    const uint16_t fill = ui_rgb565(19U, 62U, 61U);
    const uint16_t grid = ui_rgb565(48U, 56U, 59U);
    const uint16_t trace = ui_rgb565(53U, 216U, 208U);
    const uint16_t peak = ui_rgb565(244U, 184U, 74U);
    uint32_t x;
    uint32_t y;
    uint32_t previous_y = 0U;
    uint32_t peak_x = 0U;
    uint8_t peak_level = 0U;

    /* Calculate each trace ordinate once.  The row-major fill below keeps
     * SDRAM writes contiguous instead of writing one strided column at a
     * time, which is materially faster on the uncached CPU1 framebuffer. */
    for (x = 0U; x < UI_SPECTRUM_WIDTH; x++)
    {
        const uint8_t level = ui_interpolated_spectrum_level(channel, x);
        g_spectrum_line_y[x] = (uint16_t)((UI_SPECTRUM_HEIGHT - 1U) -
                                          ((uint32_t) level *
                                           (UI_SPECTRUM_HEIGHT - 1U) / 255U));
        if (level >= peak_level)
        {
            peak_level = level;
            peak_x = x;
        }
    }

    for (y = 0U; y < UI_SPECTRUM_HEIGHT; y++)
    {
        uint16_t * row = &buffer[y * UI_SPECTRUM_WIDTH];
        for (x = 0U; x < UI_SPECTRUM_WIDTH; x++)
        {
            row[x] = (y > g_spectrum_line_y[x]) ? fill : background;
        }
    }

    for (y = 0U; y <= 3U; y++)
    {
        const uint32_t grid_y = (y * (UI_SPECTRUM_HEIGHT - 1U)) / 3U;
        for (x = 0U; x < UI_SPECTRUM_WIDTH; x++)
        {
            buffer[(grid_y * UI_SPECTRUM_WIDTH) + x] = grid;
        }
    }

    previous_y = g_spectrum_line_y[0];
    for (x = 0U; x < UI_SPECTRUM_WIDTH; x++)
    {
        const uint32_t line_y = g_spectrum_line_y[x];
        const uint32_t low_y = (line_y < previous_y) ? line_y : previous_y;
        const uint32_t high_y = (line_y > previous_y) ? line_y : previous_y;
        for (y = low_y; y <= high_y; y++)
        {
            buffer[(y * UI_SPECTRUM_WIDTH) + x] = trace;
            if ((y + 1U) < UI_SPECTRUM_HEIGHT)
            {
                buffer[((y + 1U) * UI_SPECTRUM_WIDTH) + x] = trace;
            }
        }
        previous_y = line_y;
    }

    const uint32_t peak_y = g_spectrum_line_y[peak_x];
    for (x = (peak_x > 1U) ? peak_x - 1U : 0U;
         (x < UI_SPECTRUM_WIDTH) && (x <= peak_x + 1U);
         x++)
    {
        for (y = 0U; y < 3U; y++)
        {
            if ((peak_y + y) < UI_SPECTRUM_HEIGHT)
            {
                buffer[((peak_y + y) * UI_SPECTRUM_WIDTH) + x] = peak;
            }
        }
    }
}

static void ui_init_waterfalls(void)
{
    uint32_t channel;
    uint32_t x;
    uint32_t y;
    for (channel = 0U; channel < UI_CHANNEL_COUNT; channel++)
    {
        g_waterfall_ring_head[channel] = 0U;
        for (y = 0U; y < UI_WATERFALL_HEIGHT; y++)
        {
            uint16_t * const row = &g_waterfall_buffer[channel][y * UI_WATERFALL_STORAGE_WIDTH];
            for (x = 0U; x < UI_WATERFALL_RING_WIDTH; x++)
            {
                const uint16_t color = ui_heat_color_fast(ui_waterfall_level(channel, x, y));
                row[x] = color;
                row[x + UI_WATERFALL_RING_WIDTH] = color;
            }
        }
    }
}

static uint8_t * ui_waterfall_overlay_address(uint32_t channel)
{
    if (channel >= UI_CHANNEL_COUNT)
    {
        return NULL;
    }
    return (uint8_t *)&g_waterfall_buffer[channel][g_waterfall_ring_head[channel]];
}

static void ui_waterfall_overlay_poll(void)
{
    if (g_waterfall_overlay_pending &&
        (g_waterfall_overlay_line_event != g_display_diag.glcdc_line_events))
    {
        g_waterfall_overlay_pending = false;
        if (g_content_measurement_enabled)
        {
            /* The pointer became visible after a real GLCDC line event. */
            g_ui_content_frame_count++;
        }
    }
}

static bool ui_poll_glcdc_layer2_underflow(void)
{
    /* The generated project leaves the GR2 underflow IRQ disabled because
     * layer 2 is normally unused.  The status/clear bits are still available
     * when the runtime overlay is enabled, so sample them from the LVGL task
     * and keep the diagnostic counter honest. */
    const bool layer2_active = g_waterfall_overlay_enabled ||
        g_rf_overlay_state == UI_RF_OVERLAY_ENABLE_WAIT ||
        g_rf_overlay_state == UI_RF_OVERLAY_ACTIVE ||
        g_rf_overlay_state == UI_RF_OVERLAY_DISABLE_WAIT;
    if (layer2_active && (R_GLCDC->SYSCNT.STMON_b.L2UNDF != 0U))
    {
        const uint32_t line_event = g_display_diag.glcdc_line_events;
        R_GLCDC->SYSCNT.STCLR_b.L2UNDFCLR = 1U;
        if (g_rf_overlay_underflow_line_valid &&
            (g_rf_overlay_last_underflow_line_event == line_event))
        {
            return true;
        }
        g_rf_overlay_underflow_line_valid = true;
        g_rf_overlay_last_underflow_line_event = line_event;
        g_display_diag.overlay_underflows++;
        g_display_diag.glcdc_underflows++;
        g_display_diag.underflow_last_context =
            DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT;
        g_display_diag.underflow_waterfall_present++;
        if ((g_rf_overlay_state == UI_RF_OVERLAY_ENABLE_WAIT) &&
            !g_rf_overlay_enable_underflow_tolerated)
        {
            /* Some GLCDC revisions can report one cold FIFO miss as Layer 2
             * is first enabled. Keep scanning, but require a clean stability
             * window and fail immediately if another frame underflows. */
            g_rf_overlay_enable_underflow_tolerated = true;
            g_rf_overlay_enable_clean_vsyncs = 0U;
            g_display_diag.overlay_enable_clean_vsyncs = 0U;
            g_display_diag.overlay_startup_underflows_tolerated++;
            return true;
        }
        if (!g_rf_overlay_failed &&
            g_rf_overlay_state != UI_RF_OVERLAY_OFF)
        {
            g_rf_overlay_failed = true;
            g_display_diag.overlay_errors++;
            g_display_diag.overlay_last_error = (uint32_t)FSP_ERR_UNDERFLOW;
            rf_ui_waterfall_overlay_fail((uint32_t)FSP_ERR_UNDERFLOW);
        }
        return true;
    }
    return false;
}

static void ui_rf_waterfall_overlay_fail(fsp_err_t error)
{
    if (g_rf_overlay_failed)
    {
        return;
    }
    g_rf_overlay_failed = true;
    g_display_diag.overlay_errors++;
    g_display_diag.overlay_last_error = (uint32_t)error;
    rf_ui_waterfall_overlay_fail((uint32_t)error);
}

static fsp_err_t ui_rf_waterfall_overlay_layer_submit(
    const rf_ui_waterfall_overlay_frame_t * frame)
{
    if ((frame == NULL) || (frame->base == NULL))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    const uint64_t stride_bits = (uint64_t)frame->hstride_pixels *
        RF_UI_WATERFALL_OVERLAY_BITS_PER_PIXEL;
    if (((((uintptr_t)frame->base) & (UI_GLCDC_BURST_BYTES - 1U)) != 0U) ||
        ((stride_bits & 7U) != 0U) ||
        (((stride_bits / 8U) & (UI_GLCDC_BURST_BYTES - 1U)) != 0U))
    {
        return FSP_ERR_INVALID_ALIGNMENT;
    }

    if ((frame->hsize == 0U) || (frame->vsize == 0U) ||
        ((frame->hsize & 1U) != 0U) || (frame->x < 0) || (frame->y < 0) ||
        (frame->transparent_prefix > frame->hsize) ||
        ((frame->transparent_prefix & 1U) != 0U) ||
        (frame->hsize > UI_GLCDC_DIMENSION_MASK) ||
        (frame->vsize > UI_GLCDC_DIMENSION_MASK))
    {
        return FSP_ERR_INVALID_LAYER_SETTING;
    }
    if ((g_display_ctrl.state != DISPLAY_STATE_DISPLAYING) ||
        (g_display_ctrl.p_cfg == NULL))
    {
        return FSP_ERR_INVALID_MODE;
    }
    if ((R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].VEN_b.PVEN != 0U) ||
        (R_GLCDC->BG.EN_b.VEN != 0U))
    {
        return FSP_ERR_INVALID_UPDATE_TIMING;
    }

    const uint32_t back_porch_x =
        g_display_ctrl.p_cfg->output.htiming.back_porch;
    const uint32_t back_porch_y =
        g_display_ctrl.p_cfg->output.vtiming.back_porch;
    const uint32_t graphics_start_x = back_porch_x + (uint32_t)frame->x;
    const uint32_t graphics_start_y = back_porch_y + (uint32_t)frame->y;
    const uint32_t line_offset_bytes = (uint32_t)(stride_bits / 8U);
    const uint32_t line_read_bytes =
        ((((uint32_t)frame->hsize *
           RF_UI_WATERFALL_OVERLAY_BITS_PER_PIXEL) / 8U) +
         (UI_GLCDC_BURST_BYTES - 1U)) & ~(UI_GLCDC_BURST_BYTES - 1U);
    const uint32_t transfer_count = line_read_bytes / UI_GLCDC_BURST_BYTES;
    if (((uint32_t)frame->x + frame->hsize >
         g_display_ctrl.p_cfg->output.htiming.display_cyc) ||
        ((uint32_t)frame->y + frame->vsize >
         g_display_ctrl.p_cfg->output.vtiming.display_cyc) ||
        (graphics_start_x > UI_GLCDC_DIMENSION_MASK) ||
        (graphics_start_y > UI_GLCDC_DIMENSION_MASK) ||
        (line_offset_bytes > UI_GLCDC_LINE_OFFSET_MASK) ||
        (transfer_count == 0U) ||
        (transfer_count > (UI_GLCDC_TRANSFER_COUNT_MASK + 1U)))
    {
        return FSP_ERR_INVALID_LAYER_SETTING;
    }

    /* These registers share the Layer 2 shadow latch. The rectangle makes
     * only the base-alignment prefix transparent; the 800 valid pixels keep
     * their CLUT alpha. Do not touch CLUTINT, which selects the active table. */
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLMRD = 1U;
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLM2 =
        (uint32_t)(uintptr_t)frame->base;
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLM3 =
        (line_offset_bytes & UI_GLCDC_LINE_OFFSET_MASK) << 16;
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLM5 =
        ((((uint32_t)frame->vsize - 1U) & UI_GLCDC_DIMENSION_MASK) << 16) |
        ((transfer_count - 1U) & UI_GLCDC_TRANSFER_COUNT_MASK);
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].FLM6 =
        ((uint32_t)GLCDC_INPUT_INTERFACE_FORMAT_CLUT4) << 28;
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB2 =
        ((graphics_start_y & UI_GLCDC_DIMENSION_MASK) << 16) |
        ((uint32_t)frame->vsize & UI_GLCDC_DIMENSION_MASK);
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB3 =
        ((graphics_start_x & UI_GLCDC_DIMENSION_MASK) << 16) |
        ((uint32_t)frame->hsize & UI_GLCDC_DIMENSION_MASK);

    uint32_t blend_control = UI_GLCDC_BLEND_ON_LOWER_LAYER;
    if (frame->transparent_prefix != 0U)
    {
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB4 =
            ((graphics_start_y & UI_GLCDC_DIMENSION_MASK) << 16) |
            ((uint32_t)frame->vsize & UI_GLCDC_DIMENSION_MASK);
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB5 =
            ((graphics_start_x & UI_GLCDC_DIMENSION_MASK) << 16) |
            ((uint32_t)frame->transparent_prefix & UI_GLCDC_DIMENSION_MASK);
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB6 = 0U;
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB7 = 0U;
        blend_control |= UI_GLCDC_RECT_ALPHA_ENABLE;
        g_rf_ui_channel_switch_diag.overlay_guard_clip_submits++;
        g_rf_ui_channel_switch_diag.overlay_guard_clip_pixels +=
            frame->transparent_prefix;
    }
    else
    {
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB4 = 0U;
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB5 = 0U;
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB6 = 0U;
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB7 = 0U;
        g_rf_ui_channel_switch_diag
            .overlay_guard_clip_zero_prefix_submits++;
    }
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].AB1 = blend_control;

    __DSB();
    R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].VEN_b.PVEN = 1U;
    return FSP_SUCCESS;
}

static void ui_rf_waterfall_overlay_monitor_start(uint32_t line_event)
{
    g_rf_overlay_monitor_start_line = line_event;
    g_rf_overlay_monitor_start_underflows =
        g_display_diag.glcdc_underflows;
    g_rf_overlay_monitor_start_layer2_underflows =
        g_display_diag.overlay_underflows;
}

static void ui_rf_waterfall_overlay_monitor_step(uint32_t line_event)
{
    if((line_event - g_rf_overlay_monitor_start_line) <
       UI_RF_OVERLAY_MONITOR_VSYNCS) {
        return;
    }
    g_display_diag.overlay_monitor_start_line =
        g_rf_overlay_monitor_start_line;
    g_display_diag.overlay_monitor_end_line = line_event;
    g_display_diag.overlay_monitor_start_underflows =
        g_rf_overlay_monitor_start_underflows;
    g_display_diag.overlay_monitor_end_underflows =
        g_display_diag.glcdc_underflows;
    g_display_diag.overlay_monitor_start_layer2_underflows =
        g_rf_overlay_monitor_start_layer2_underflows;
    g_display_diag.overlay_monitor_end_layer2_underflows =
        g_display_diag.overlay_underflows;
    g_display_diag.overlay_monitor_windows++;
    ui_rf_waterfall_overlay_monitor_start(line_event);
}

static void ui_rf_waterfall_overlay_init(void)
{
    const uint32_t * palette = NULL;
    uint32_t color_count = 0U;
    g_rf_overlay_state = UI_RF_OVERLAY_OFF;
    g_rf_overlay_failed = false;
    g_rf_overlay_present_pending = false;
    g_rf_overlay_pven_wait_recorded = false;
    g_rf_overlay_operation_line_event = g_display_diag.glcdc_line_events;
    g_rf_overlay_last_line_event = g_display_diag.glcdc_line_events;
    g_rf_overlay_last_underflows = g_display_diag.glcdc_underflows;
    g_rf_overlay_stable_vsyncs = 0U;
    g_rf_overlay_enable_clean_vsyncs = 0U;
    g_rf_overlay_enable_underflow_tolerated = false;
    g_rf_overlay_underflow_line_valid = false;
    g_rf_overlay_pending_generation = 0U;
    g_rf_overlay_monitor_start_line = g_display_diag.glcdc_line_events;
    g_rf_overlay_monitor_start_underflows = g_display_diag.glcdc_underflows;
    g_rf_overlay_monitor_start_layer2_underflows =
        g_display_diag.overlay_underflows;
    g_display_diag.overlay_state = (uint32_t)UI_RF_OVERLAY_OFF;
    g_display_diag.overlay_enable_clean_vsyncs = 0U;
    g_display_diag.overlay_fallback_ready = 0U;
    g_display_diag.overlay_monitor_windows = 0U;
    g_display_diag.overlay_monitor_start_line = 0U;
    g_display_diag.overlay_monitor_end_line = 0U;
    g_display_diag.overlay_monitor_start_underflows = 0U;
    g_display_diag.overlay_monitor_end_underflows = 0U;
    g_display_diag.overlay_monitor_start_layer2_underflows = 0U;
    g_display_diag.overlay_monitor_end_layer2_underflows = 0U;
    memset(&g_rf_overlay_frame, 0, sizeof(g_rf_overlay_frame));
    if (R_GLCDC->SYSCNT.STMON_b.L2UNDF != 0U)
    {
        R_GLCDC->SYSCNT.STCLR_b.L2UNDFCLR = 1U;
    }
    if (!rf_ui_waterfall_overlay_palette_get(&palette, &color_count) ||
        (palette == NULL) ||
        (color_count != RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS))
    {
        return;
    }
    g_rf_overlay_clut_cfg.p_base = (uint32_t *)palette;
    g_rf_overlay_clut_cfg.start = 0U;
    g_rf_overlay_clut_cfg.size = (uint16_t)color_count;
    g_rf_overlay_state = UI_RF_OVERLAY_WAIT_STABLE;
}

static void ui_rf_waterfall_overlay_disable(void)
{
    __DSB();
    const fsp_err_t err = R_GLCDC_BufferChange(
        &g_display_ctrl, NULL, DISPLAY_FRAME_LAYER_2);
    if (err == FSP_SUCCESS)
    {
        g_rf_overlay_operation_line_event =
            g_display_diag.glcdc_line_events;
        g_rf_overlay_state = UI_RF_OVERLAY_DISABLE_WAIT;
    }
    else if (err != FSP_ERR_INVALID_UPDATE_TIMING)
    {
        g_display_diag.overlay_errors++;
        g_display_diag.overlay_last_error = (uint32_t)err;
        g_display_diag.overlay_enabled = 0U;
        rf_ui_waterfall_overlay_set_enabled(false);
        g_rf_overlay_state = UI_RF_OVERLAY_OFF;
    }
}

static void ui_rf_waterfall_overlay_step(bool allow_present)
{
    const bool layer2_underflow = ui_poll_glcdc_layer2_underflow();
    const uint32_t line_event = g_display_diag.glcdc_line_events;
    const bool line_advanced = line_event != g_rf_overlay_last_line_event;
    if (line_advanced)
    {
        g_rf_overlay_last_line_event = line_event;
        if (g_rf_overlay_state == UI_RF_OVERLAY_ACTIVE)
        {
            ui_rf_waterfall_overlay_monitor_step(line_event);
        }
    }

    const bool overlay_line_ready = g_rf_overlay_present_pending &&
        line_event != g_rf_overlay_operation_line_event;
    const bool overlay_update_pending =
        R_GLCDC->GR[DISPLAY_FRAME_LAYER_2].VEN_b.PVEN != 0U;
    if (overlay_line_ready && overlay_update_pending)
    {
        g_rf_ui_channel_switch_diag.overlay_latch_pven_wait_polls++;
        if (!g_rf_overlay_pven_wait_recorded)
        {
            g_rf_overlay_pven_wait_recorded = true;
            g_rf_ui_channel_switch_diag.overlay_latch_pven_deferrals++;
        }
    }
    if (overlay_line_ready && !overlay_update_pending)
    {
        rf_ui_waterfall_overlay_frame_latched(
            g_rf_overlay_pending_generation);
        g_rf_overlay_present_pending = false;
        g_rf_overlay_pven_wait_recorded = false;
        g_rf_ui_channel_switch_diag.overlay_latch_pven_confirmations++;
        if (g_content_measurement_enabled)
        {
            g_ui_content_frame_count++;
        }
    }

    if (g_rf_overlay_failed)
    {
        if ((g_rf_overlay_state == UI_RF_OVERLAY_ENABLE_WAIT) ||
            (g_rf_overlay_state == UI_RF_OVERLAY_ACTIVE))
        {
            /* Keep the last valid Layer 2 image covering the plot until the
             * bounded RGB565 fallback has completed its LVGL transaction.
             * The ready flag is raised before the final invalidation; this
             * owner sees it on the following pass, after lv_timer_handler()
             * has submitted and VSync-latched the software image below it. */
            if (!g_rf_overlay_present_pending && line_advanced &&
                rf_ui_waterfall_overlay_disable_ready())
            {
                ui_rf_waterfall_overlay_disable();
            }
        }
        else if ((g_rf_overlay_state != UI_RF_OVERLAY_DISABLE_WAIT) &&
                 (g_rf_overlay_state != UI_RF_OVERLAY_OFF))
        {
            g_display_diag.overlay_enabled = 0U;
            rf_ui_waterfall_overlay_set_enabled(false);
            g_rf_overlay_state = UI_RF_OVERLAY_OFF;
        }
    }

    switch (g_rf_overlay_state)
    {
        case UI_RF_OVERLAY_WAIT_STABLE:
            if (!line_advanced) break;
            if (g_display_diag.glcdc_underflows !=
                g_rf_overlay_last_underflows)
            {
                g_rf_overlay_last_underflows =
                    g_display_diag.glcdc_underflows;
                g_rf_overlay_stable_vsyncs = 0U;
                break;
            }
            g_rf_overlay_stable_vsyncs++;
            if (g_rf_overlay_stable_vsyncs < UI_RF_OVERLAY_STABLE_VSYNCS)
            {
                break;
            }
            {
                const fsp_err_t err = R_GLCDC_ClutUpdate(
                    &g_display_ctrl, &g_rf_overlay_clut_cfg,
                    DISPLAY_FRAME_LAYER_2);
                if (err == FSP_SUCCESS)
                {
                    g_rf_overlay_operation_line_event = line_event;
                    g_rf_overlay_state = UI_RF_OVERLAY_CLUT1_WAIT;
                }
                else if (err != FSP_ERR_INVALID_UPDATE_TIMING)
                {
                    ui_rf_waterfall_overlay_fail(err);
                }
            }
            break;

        case UI_RF_OVERLAY_CLUT1_WAIT:
            if (!line_advanced ||
                line_event == g_rf_overlay_operation_line_event) break;
            {
                const fsp_err_t err = R_GLCDC_ClutUpdate(
                    &g_display_ctrl, &g_rf_overlay_clut_cfg,
                    DISPLAY_FRAME_LAYER_2);
                if (err == FSP_SUCCESS)
                {
                    g_rf_overlay_operation_line_event = line_event;
                    g_rf_overlay_state = UI_RF_OVERLAY_CLUT0_WAIT;
                }
                else if (err != FSP_ERR_INVALID_UPDATE_TIMING)
                {
                    ui_rf_waterfall_overlay_fail(err);
                }
            }
            break;

        case UI_RF_OVERLAY_CLUT0_WAIT:
            if (!line_advanced ||
                line_event == g_rf_overlay_operation_line_event ||
                !rf_ui_waterfall_overlay_prepare_frame(&g_rf_overlay_frame))
            {
                break;
            }
            {
                const fsp_err_t err = ui_rf_waterfall_overlay_layer_submit(
                    &g_rf_overlay_frame);
                if (err == FSP_SUCCESS)
                {
                    rf_ui_waterfall_overlay_frame_submitted(
                        g_rf_overlay_frame.generation);
                    g_rf_overlay_pending_generation =
                        g_rf_overlay_frame.generation;
                    g_rf_overlay_operation_line_event = line_event;
                    g_rf_overlay_present_pending = true;
                    g_rf_overlay_pven_wait_recorded = false;
                    g_rf_overlay_state = UI_RF_OVERLAY_ENABLE_WAIT;
                    g_rf_overlay_enable_clean_vsyncs = 0U;
                    g_rf_overlay_enable_underflow_tolerated = false;
                    g_display_diag.overlay_enable_clean_vsyncs = 0U;
                    g_display_diag.overlay_updates++;
                }
                else if (err != FSP_ERR_INVALID_UPDATE_TIMING)
                {
                    ui_rf_waterfall_overlay_fail(err);
                }
            }
            break;

        case UI_RF_OVERLAY_ENABLE_WAIT:
            if (g_rf_overlay_failed || g_rf_overlay_present_pending ||
                !line_advanced)
            {
                break;
            }
            if (layer2_underflow)
            {
                g_rf_overlay_enable_clean_vsyncs = 0U;
                g_display_diag.overlay_enable_clean_vsyncs = 0U;
                break;
            }
            g_rf_overlay_enable_clean_vsyncs++;
            g_display_diag.overlay_enable_clean_vsyncs =
                g_rf_overlay_enable_clean_vsyncs;
            if (g_rf_overlay_enable_clean_vsyncs >=
                UI_RF_OVERLAY_ENABLE_CLEAN_VSYNCS)
            {
                g_display_diag.overlay_enabled = 1U;
                rf_ui_waterfall_overlay_set_enabled(true);
                g_rf_overlay_state = UI_RF_OVERLAY_ACTIVE;
                ui_rf_waterfall_overlay_monitor_start(line_event);
            }
            break;

        case UI_RF_OVERLAY_ACTIVE:
            if (g_rf_overlay_failed || g_rf_overlay_present_pending ||
                !line_advanced || !allow_present) break;
            if (!rf_ui_waterfall_overlay_prepare_frame(&g_rf_overlay_frame))
            {
                break;
            }
            {
                const fsp_err_t err = ui_rf_waterfall_overlay_layer_submit(
                    &g_rf_overlay_frame);
                if (err == FSP_SUCCESS)
                {
                    rf_ui_waterfall_overlay_frame_submitted(
                        g_rf_overlay_frame.generation);
                    g_rf_overlay_pending_generation =
                        g_rf_overlay_frame.generation;
                    g_rf_overlay_operation_line_event = line_event;
                    g_rf_overlay_present_pending = true;
                    g_rf_overlay_pven_wait_recorded = false;
                    g_display_diag.overlay_updates++;
                }
                else if (err != FSP_ERR_INVALID_UPDATE_TIMING)
                {
                    ui_rf_waterfall_overlay_fail(err);
                }
            }
            break;

        case UI_RF_OVERLAY_DISABLE_WAIT:
            if (line_event != g_rf_overlay_operation_line_event)
            {
                g_display_diag.overlay_enabled = 0U;
                rf_ui_waterfall_overlay_set_enabled(false);
                g_rf_overlay_state = UI_RF_OVERLAY_OFF;
                g_display_diag.overlay_fallback_ready = 0U;
                g_display_diag.overlay_fallback_completions++;
            }
            break;

        case UI_RF_OVERLAY_OFF:
        default:
            break;
    }
    g_display_diag.overlay_state = (uint32_t)g_rf_overlay_state;
}

static bool ui_waterfall_overlay_present(void)
{
    uint8_t * const address = ui_waterfall_overlay_address(UI_WATERFALL_OVERLAY_CHANNEL);
    fsp_err_t err;

    if (!g_waterfall_overlay_enabled || (address == NULL))
    {
        return false;
    }
    if ((((uintptr_t)address) & 63U) != 0U)
    {
        g_display_diag.overlay_errors++;
        g_display_diag.overlay_last_error = (uint32_t)FSP_ERR_INVALID_ALIGNMENT;
        return false;
    }
    if (g_waterfall_overlay_pending)
    {
        return false;
    }

    do
    {
        __DSB();
        err = R_GLCDC_BufferChange(&g_display_ctrl,
                                   address,
                                   DISPLAY_FRAME_LAYER_2);
    } while (FSP_ERR_INVALID_UPDATE_TIMING == err);
    if (FSP_SUCCESS != err)
    {
        g_display_diag.overlay_errors++;
        g_display_diag.overlay_last_error = (uint32_t)err;
        return false;
    }
    g_waterfall_overlay_line_event = g_display_diag.glcdc_line_events;
    g_waterfall_overlay_pending = true;
    g_display_diag.overlay_updates++;
    return true;
}

static void ui_waterfall_overlay_set_enabled(bool enabled)
{
    uint8_t * const address = ui_waterfall_overlay_address(UI_WATERFALL_OVERLAY_CHANNEL);
    fsp_err_t err;

    if (enabled == g_waterfall_overlay_enabled)
    {
        return;
    }

    if (!enabled)
    {
        do
        {
            __DSB();
            err = R_GLCDC_BufferChange(&g_display_ctrl,
                                       NULL,
                                       DISPLAY_FRAME_LAYER_2);
        } while (FSP_ERR_INVALID_UPDATE_TIMING == err);
        if (FSP_SUCCESS != err)
        {
            g_display_diag.overlay_errors++;
            g_display_diag.overlay_last_error = (uint32_t)err;
            return;
        }
        g_waterfall_overlay_enabled = false;
        g_waterfall_overlay_pending = false;
        g_display_diag.overlay_enabled = 0U;
        return;
    }

    if ((address == NULL) ||
        ((((uintptr_t) address) & 63U) != 0U) ||
        ((UI_WATERFALL_STRIDE_BYTES & 63U) != 0U))
    {
        g_display_diag.overlay_errors++;
        g_display_diag.overlay_last_error = (uint32_t) FSP_ERR_INVALID_ALIGNMENT;
        return;
    }

    memset(&g_waterfall_overlay_cfg, 0, sizeof(g_waterfall_overlay_cfg));
    g_waterfall_overlay_cfg.input.p_base = (uint32_t *) address;
    g_waterfall_overlay_cfg.input.hsize = UI_WATERFALL_WIDTH;
    g_waterfall_overlay_cfg.input.vsize = UI_WATERFALL_HEIGHT;
    /* FSP's hstride is expressed in pixels (despite the generic API field
     * name); 896 RGB565 pixels are 1792 bytes, an exact 64-byte burst span. */
    g_waterfall_overlay_cfg.input.hstride = UI_WATERFALL_STORAGE_WIDTH;
    g_waterfall_overlay_cfg.input.format = DISPLAY_IN_FORMAT_16BITS_RGB565;
    g_waterfall_overlay_cfg.input.line_descending_enable = false;
    g_waterfall_overlay_cfg.input.lines_repeat_enable = false;
    g_waterfall_overlay_cfg.input.lines_repeat_times = 0U;
    g_waterfall_overlay_cfg.layer.coordinate.x = (int16_t)UI_WATERFALL_OVERLAY_X;
    g_waterfall_overlay_cfg.layer.coordinate.y = (int16_t)UI_WATERFALL_OVERLAY_Y;
    g_waterfall_overlay_cfg.layer.fade_control = DISPLAY_FADE_CONTROL_NONE;
    g_waterfall_overlay_cfg.layer.fade_speed = 0U;

    do
    {
        __DSB();
        err = R_GLCDC_LayerChange(&g_display_ctrl,
                                  &g_waterfall_overlay_cfg,
                                  DISPLAY_FRAME_LAYER_2);
    } while (FSP_ERR_INVALID_UPDATE_TIMING == err);
    if (FSP_SUCCESS != err)
    {
        g_display_diag.overlay_errors++;
        g_display_diag.overlay_last_error = (uint32_t)err;
        return;
    }
    g_waterfall_overlay_enabled = true;
    g_waterfall_overlay_pending = false;
    g_display_diag.overlay_enabled = 1U;
}

static void ui_waterfall_image_source_update(uint32_t channel)
{
    if ((channel >= UI_CHANNEL_COUNT) || (g_waterfall_canvas[channel] == NULL))
    {
        return;
    }
    g_waterfall_image[channel].data =
        (const uint8_t *)&g_waterfall_buffer[channel][g_waterfall_ring_head[channel]];
    if (!((channel == UI_WATERFALL_OVERLAY_CHANNEL) &&
          g_waterfall_overlay_enabled &&
          (g_active_page == UI_PAGE_RECOGNITION)))
    {
        lv_obj_invalidate(g_waterfall_canvas[channel]);
    }
}

static void ui_tile_queue_reset(void)
{
    g_tile_queue_head = 0U;
    g_tile_queue_count = 0U;
}

static void ui_tile_queue_push(const ra8p1_display_tile_payload_t * tile)
{
    uint32_t write_index;
    if (tile == NULL)
    {
        return;
    }
    if (g_tile_queue_count >= UI_TILE_QUEUE_CAPACITY)
    {
        /* Keep the newest complete blocks.  Dropping an old block is visible
         * in the diagnostic counter, never silently converted to a fake column. */
        g_tile_queue_head = (g_tile_queue_head + 1U) % UI_TILE_QUEUE_CAPACITY;
        g_tile_queue_count--;
        g_ui_waterfall_tiles_dropped++;
    }
    write_index = (g_tile_queue_head + g_tile_queue_count) % UI_TILE_QUEUE_CAPACITY;
    g_tile_queue[write_index] = *tile;
    g_tile_queue_count++;
}

static void ui_update_waterfall_columns(void)
{
    const uint32_t generation_start_cycles = DWT->CYCCNT;
    const uint32_t tick_ms = g_lvgl_tick_ms;
    uint32_t eligible_tiles = 0U;
    uint32_t column_count;
    uint32_t center_index;
    uint32_t channel;
    uint32_t y;

    ui_waterfall_overlay_poll();

    /* Keep consuming complete tiles even while the monitor page is visible.
     * The ring is the real history; deferring it until the recognition page
     * would overflow the four-slot IPC queue and turn valid blocks into
     * drops.  A paused recognition page still intentionally freezes content. */
    if (((g_active_page == UI_PAGE_RECOGNITION) && !g_waterfall_running))
    {
        return;
    }
    if (g_waterfall_overlay_enabled && g_waterfall_overlay_pending)
    {
        return;
    }

    if (!g_live_signal_valid || (g_tile_queue_count == 0U))
    {
        return;
    }

    while ((g_tile_queue_count != 0U) &&
           ((g_tile_queue[g_tile_queue_head].session_id !=
             g_live_signal_frame.session_id) ||
            (g_tile_queue[g_tile_queue_head].center_index >= RA8P1_CENTER_COUNT)))
    {
        g_tile_queue_head = (g_tile_queue_head + 1U) % UI_TILE_QUEUE_CAPACITY;
        g_tile_queue_count--;
        g_ui_waterfall_tiles_dropped++;
    }
    if (g_tile_queue_count == 0U)
    {
        return;
    }
    center_index = g_tile_queue[g_tile_queue_head].center_index;

    /* Partial-window tiles are sequenced independently from completed NPU
     * frames.  Session validation and the tile seqlock already guarantee that
     * every eligible block is coherent and belongs to the current stream. */
    for (uint32_t index = 0U; index < g_tile_queue_count; ++index)
    {
        const uint32_t queue_index = (g_tile_queue_head + index) % UI_TILE_QUEUE_CAPACITY;
        const ra8p1_display_tile_payload_t * tile = &g_tile_queue[queue_index];
        if ((tile->session_id == g_live_signal_frame.session_id) &&
            (tile->center_index == center_index))
        {
            eligible_tiles++;
        }
        else
        {
            break;
        }
    }
    if (eligible_tiles == 0U)
    {
        return;
    }
    /* Protocol v7 carries one 192-bin novel time row per shared-memory slot. */
    eligible_tiles = 1U;
    column_count = 1U;

    for (channel = 0U; channel < UI_CHANNEL_COUNT; channel++)
    {
        if (channel != (center_index % UI_CHANNEL_COUNT))
        {
            continue;
        }

        const uint32_t old_head = g_waterfall_ring_head[channel];
        for (y = 0U; y < UI_WATERFALL_HEIGHT; y++)
        {
            uint16_t * const row =
                &g_waterfall_buffer[channel][y * UI_WATERFALL_STORAGE_WIDTH];
            const uint32_t frequency_bin =
                ((UI_WATERFALL_HEIGHT - 1U - y) *
                 (RA8P1_DISPLAY_TILE_WIDTH - 1U)) /
                (UI_WATERFALL_HEIGHT - 1U);
            uint32_t output_offset = 0U;
            for (uint32_t tile_index = 0U; tile_index < eligible_tiles; ++tile_index)
            {
                const uint32_t queue_index =
                    (g_tile_queue_head + tile_index) % UI_TILE_QUEUE_CAPACITY;
                const ra8p1_display_tile_payload_t * tile = &g_tile_queue[queue_index];
                const uint8_t level = tile->levels[frequency_bin];
                uint32_t output_column = old_head + output_offset;
                if (output_column >= UI_WATERFALL_RING_WIDTH)
                {
                    output_column -= UI_WATERFALL_RING_WIDTH;
                }
                row[output_column] = ui_heat_color_fast(level);
                row[output_column + UI_WATERFALL_RING_WIDTH] = row[output_column];
                output_offset++;
            }
        }
        g_waterfall_ring_head[channel] =
            (old_head + column_count) % UI_WATERFALL_RING_WIDTH;
    }

    {
        const uint32_t last_index = (g_tile_queue_head + eligible_tiles - 1U) %
                                     UI_TILE_QUEUE_CAPACITY;
        g_waterfall_rendered_session = g_tile_queue[last_index].session_id;
        g_waterfall_rendered_sequence = g_tile_queue[last_index].sequence;
    }
    g_tile_queue_head = (g_tile_queue_head + eligible_tiles) % UI_TILE_QUEUE_CAPACITY;
    g_tile_queue_count -= eligible_tiles;

    {
        const uint32_t generation_cycles = DWT->CYCCNT - generation_start_cycles;
        g_ui_waterfall_gen_cycles += generation_cycles;
        if (generation_cycles > g_ui_waterfall_gen_max_cycles)
        {
            g_ui_waterfall_gen_max_cycles = generation_cycles;
        }
        g_ui_waterfall_columns_generated += column_count;
        g_ui_waterfall_tiles_consumed += eligible_tiles;
    }
    if (g_active_page == UI_PAGE_RECOGNITION)
    {
        if (g_waterfall_overlay_enabled)
        {
            /* Layer2 owns the waterfall rectangle.  Submit every complete
             * block immediately; the pending flag prevents writes while the
            * previous pointer is still being consumed by GLCDC. */
            g_waterfall_visual_dirty = false;
            (void) ui_waterfall_overlay_present();
            if ((g_last_waterfall_update_tick == 0U) ||
                ((tick_ms - g_last_waterfall_update_tick) >= UI_WATERFALL_PRESENT_PERIOD_MS))
            {
                ui_update_waterfall_boxes();
                g_last_waterfall_update_tick = tick_ms;
            }
        }
        else
        {
            /* Software fallback: keep consuming tiles promptly and submit at
             * the LVGL refresh cadence. Dirty checking avoids empty redraws. */
            g_waterfall_visual_dirty = true;
            if ((g_last_waterfall_update_tick == 0U) ||
                ((tick_ms - g_last_waterfall_update_tick) >= UI_WATERFALL_PRESENT_PERIOD_MS))
            {
                for (channel = 0U; channel < UI_CHANNEL_COUNT; ++channel)
                {
                    if ((g_tile_center_valid_mask & (1UL << channel)) != 0U)
                    {
                        ui_waterfall_image_source_update(channel);
                    }
                }
                ui_update_waterfall_boxes();
                g_last_waterfall_update_tick = tick_ms;
                g_waterfall_visual_dirty = false;
                if (g_content_measurement_enabled)
                {
                    g_content_generation_pending = true;
                }
            }
        }
    }
}

static void ui_seed_live_waterfalls(void)
{
    if ((g_active_page != UI_PAGE_RECOGNITION) || !g_live_signal_valid)
    {
        return;
    }
    if (g_waterfall_rendered_session == g_live_signal_frame.session_id)
    {
        ui_update_waterfall_boxes();
        g_waterfall_visual_dirty = false;
        return;
    }
    for (uint32_t channel = 0U; channel < UI_CHANNEL_COUNT; ++channel)
    {
        memset(g_waterfall_buffer[channel], 0,
               sizeof(g_waterfall_buffer[channel]));
        g_waterfall_ring_head[channel] = 0U;
        ui_waterfall_image_source_update(channel);
    }
    g_waterfall_rendered_session = g_live_signal_frame.session_id;
    g_waterfall_rendered_sequence = 0U;
    g_waterfall_visual_dirty = false;
}

static void ui_update_spectrum_channel(uint32_t channel)
{
    if ((g_active_page != UI_PAGE_MONITOR) || (channel >= UI_CHANNEL_COUNT))
    {
        return;
    }

    const uint32_t generation_start_cycles = DWT->CYCCNT;
    ui_draw_spectrum(channel);
    /* Present the 450 x 118 spectrum as one coherent data block. */
    if (g_spectrum_canvas[channel] != NULL)
    {
        lv_obj_invalidate(g_spectrum_canvas[channel]);
    }
    const uint32_t generation_cycles = DWT->CYCCNT - generation_start_cycles;
    g_ui_spectrum_gen_cycles += generation_cycles;
    if (generation_cycles > g_ui_spectrum_gen_max_cycles)
    {
        g_ui_spectrum_gen_max_cycles = generation_cycles;
    }
    g_ui_spectrum_redraws++;
}

static void ui_update_spectra(void)
{
    const uint32_t all_channels_mask = (1UL << UI_CHANNEL_COUNT) - 1U;
    const uint32_t target_mask = (g_center_valid_mask != 0U) ?
        (g_center_valid_mask & all_channels_mask) :
        (g_live_signal_valid ? 0U : all_channels_mask);
    const uint32_t stale_mask = g_spectrum_rendered_mask & ~target_mask;
    const uint32_t render_mask = (g_spectrum_dirty_mask & target_mask) |
                                 stale_mask;
    uint32_t rendered_channels = 0U;
    uint32_t channel;
    for (channel = 0U; channel < UI_CHANNEL_COUNT; channel++)
    {
        if ((render_mask & (1UL << channel)) == 0U)
        {
            continue;
        }
        ui_update_spectrum_channel(channel);
        rendered_channels++;
    }
    g_spectrum_rendered_mask = target_mask;
    g_spectrum_dirty_mask &= ~render_mask;
    if (rendered_channels != 0U)
    {
        if (g_content_measurement_enabled)
        {
            g_content_generation_pending = true;
        }
        ui_visibility_content_prepared();
    }
}

static lv_obj_t * ui_create_label(lv_obj_t * parent,
                                  const char * text,
                                  const lv_font_t * font,
                                  lv_color_t color,
                                  int32_t x,
                                  int32_t y)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static void ui_set_container_style(lv_obj_t * object, lv_color_t color)
{
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t * ui_create_card(lv_obj_t * parent, uint32_t x, uint32_t y)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, UI_CARD_WIDTH, UI_CARD_HEIGHT);
    lv_obj_set_pos(card, (int32_t) x, (int32_t) y);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void ui_set_card_header(lv_obj_t * card, uint32_t channel)
{
    const ui_channel_info_t * info = &g_channels[channel];
    lv_obj_t * title = ui_create_label(card, info->id, &lv_font_montserrat_16, UI_COLOR_TEXT, 12, 7);
    lv_obj_t * band = ui_create_label(card, info->band, &lv_font_montserrat_14, UI_COLOR_MUTED, 58, 9);
    lv_obj_t * center = lv_label_create(card);
    lv_label_set_text_fmt(center, "%lu.%03lu GHz",
                          (unsigned long) (info->center_mhz / 1000U),
                          (unsigned long) (info->center_mhz % 1000U));
    lv_obj_set_style_text_font(center, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(center, UI_COLOR_TEXT, 0);
    lv_obj_set_width(center, 160);
    lv_obj_set_style_text_align(center, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(center, 166, 9);
    g_channel_center_label[channel] = center;
    FSP_PARAMETER_NOT_USED(title);
    FSP_PARAMETER_NOT_USED(band);

    lv_obj_t * occupancy = lv_label_create(card);
    lv_label_set_text(occupancy, "OCC --");
    lv_obj_set_style_text_font(occupancy, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(occupancy,
                                (info->occupancy >= 70U) ? UI_COLOR_DANGER :
                                ((info->occupancy >= 35U) ? UI_COLOR_WARNING : UI_COLOR_SUCCESS), 0);
    lv_obj_set_width(occupancy, 64);
    lv_obj_set_style_text_align(occupancy, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(occupancy, 415, 7);
    g_channel_occupancy_label[channel] = occupancy;
}

static void ui_set_spectrum_axes(lv_obj_t * card, uint32_t channel)
{
    FSP_PARAMETER_NOT_USED(channel);
    lv_obj_t * y_top = ui_create_label(card, "255", &lv_font_montserrat_14, UI_COLOR_AXIS, 2, 41);
    lv_obj_t * y_mid = ui_create_label(card, "128", &lv_font_montserrat_14, UI_COLOR_AXIS, 2, 91);
    lv_obj_t * y_bottom = ui_create_label(card, "0", &lv_font_montserrat_14, UI_COLOR_AXIS, 15, 141);
    lv_obj_t * x_low = lv_label_create(card);
    lv_obj_t * x_mid = lv_label_create(card);
    lv_obj_t * x_high = lv_label_create(card);
    lv_label_set_text(x_low, "-SPAN/2");
    lv_label_set_text(x_mid, "CENTER");
    lv_label_set_text(x_high, "+SPAN/2");
    lv_obj_set_style_text_font(x_low, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(x_mid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(x_high, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(x_low, UI_COLOR_AXIS, 0);
    lv_obj_set_style_text_color(x_mid, UI_COLOR_AXIS, 0);
    lv_obj_set_style_text_color(x_high, UI_COLOR_AXIS, 0);
    lv_obj_set_pos(x_low, 30, 164);
    lv_obj_set_pos(x_mid, 218, 164);
    lv_obj_set_pos(x_high, 422, 164);
    FSP_PARAMETER_NOT_USED(y_top);
    FSP_PARAMETER_NOT_USED(y_mid);
    FSP_PARAMETER_NOT_USED(y_bottom);
}

static void ui_set_waterfall_axes(lv_obj_t * card, uint32_t channel)
{
    FSP_PARAMETER_NOT_USED(channel);
    lv_obj_t * y_top = lv_label_create(card);
    lv_obj_t * y_mid = lv_label_create(card);
    lv_obj_t * y_bottom = lv_label_create(card);
    lv_label_set_text(y_top, "HIGH");
    lv_label_set_text(y_mid, "CTR");
    lv_label_set_text(y_bottom, "LOW");
    lv_obj_set_style_text_font(y_top, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(y_mid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_font(y_bottom, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(y_top, UI_COLOR_AXIS, 0);
    lv_obj_set_style_text_color(y_mid, UI_COLOR_AXIS, 0);
    lv_obj_set_style_text_color(y_bottom, UI_COLOR_AXIS, 0);
    lv_obj_set_width(y_top, 28);
    lv_obj_set_width(y_mid, 28);
    lv_obj_set_width(y_bottom, 28);
    lv_obj_set_style_text_align(y_top, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(y_mid, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(y_bottom, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(y_top, 0, 37);
    lv_obj_set_pos(y_mid, 0, 103);
    lv_obj_set_pos(y_bottom, 0, 169);

    lv_obj_t * t0 = ui_create_label(card, "OLDER", &lv_font_montserrat_14, UI_COLOR_AXIS, 30, 190);
    lv_obj_t * t5 = ui_create_label(card, "HISTORY", &lv_font_montserrat_14, UI_COLOR_AXIS, 226, 190);
    lv_obj_t * t10 = ui_create_label(card, "NEW", &lv_font_montserrat_14, UI_COLOR_AXIS, 446, 190);
    FSP_PARAMETER_NOT_USED(t0);
    FSP_PARAMETER_NOT_USED(t5);
    FSP_PARAMETER_NOT_USED(t10);
}

static void ui_create_spectrum_card(uint32_t channel)
{
    const uint32_t x = (channel & 1U) ? UI_CARD_X1 : UI_CARD_X0;
    const uint32_t y = (channel >= 2U) ? UI_CARD_Y1 : UI_CARD_Y0;
    lv_obj_t * card = ui_create_card(g_monitor_page, x, y);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, ui_waterfall_page_event, LV_EVENT_CLICKED, NULL);
    ui_set_card_header(card, channel);
    g_spectrum_canvas[channel] = lv_canvas_create(card);
    lv_canvas_set_buffer(g_spectrum_canvas[channel],
                         g_spectrum_buffer[channel],
                         UI_SPECTRUM_WIDTH,
                         UI_SPECTRUM_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(g_spectrum_canvas[channel], UI_PLOT_X, UI_SPECTRUM_PLOT_Y);
    lv_obj_set_style_border_color(g_spectrum_canvas[channel], UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(g_spectrum_canvas[channel], 1, 0);
    ui_set_spectrum_axes(card, channel);
    (void) ui_create_label(card, "LIVE SPECTRUM", &lv_font_montserrat_14, UI_COLOR_MUTED, 12, 198);
    lv_obj_t * meta = lv_label_create(card);
    lv_label_set_text(meta, "WAITING FOR IQ");
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(meta, UI_COLOR_MUTED, 0);
    lv_obj_set_pos(meta, 226, 198);
    g_channel_meta_label[channel] = meta;
}

static void ui_create_waterfall_card(uint32_t channel)
{
    const uint32_t x = (channel & 1U) ? UI_CARD_X1 : UI_CARD_X0;
    const uint32_t y = (channel >= 2U) ? UI_CARD_Y1 : UI_CARD_Y0;
    lv_obj_t * card = ui_create_card(g_recognition_page, x, y);
    lv_obj_t * title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s  |  %lu.%03lu GHz",
                          g_channels[channel].id,
                          (unsigned long) (g_channels[channel].center_mhz / 1000U),
                          (unsigned long) (g_channels[channel].center_mhz % 1000U));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_pos(title, 12, 7);
    g_waterfall_title_label[channel] = title;
    lv_obj_t * band = ui_create_label(card, g_channels[channel].band, &lv_font_montserrat_14, UI_COLOR_MUTED, 401, 9);
    FSP_PARAMETER_NOT_USED(band);

    memset(&g_waterfall_image[channel], 0, sizeof(g_waterfall_image[channel]));
    g_waterfall_image[channel].header.magic = LV_IMAGE_HEADER_MAGIC;
    g_waterfall_image[channel].header.cf = LV_COLOR_FORMAT_RGB565;
    g_waterfall_image[channel].header.w = UI_WATERFALL_WIDTH;
    g_waterfall_image[channel].header.h = UI_WATERFALL_HEIGHT;
    g_waterfall_image[channel].header.stride = UI_WATERFALL_STRIDE_BYTES;
    g_waterfall_image[channel].data_size =
        UI_WATERFALL_STRIDE_BYTES * UI_WATERFALL_HEIGHT;
    g_waterfall_image[channel].data =
        (const uint8_t *)&g_waterfall_buffer[channel][g_waterfall_ring_head[channel]];
    g_waterfall_canvas[channel] = lv_image_create(card);
    lv_image_set_src(g_waterfall_canvas[channel], &g_waterfall_image[channel]);
    lv_obj_set_size(g_waterfall_canvas[channel], UI_WATERFALL_WIDTH, UI_WATERFALL_HEIGHT);
    lv_obj_set_pos(g_waterfall_canvas[channel], UI_PLOT_X, UI_WATERFALL_PLOT_Y);
    lv_obj_set_style_border_color(g_waterfall_canvas[channel], UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(g_waterfall_canvas[channel], 1, 0);
    for (uint32_t box_index = 0U; box_index < RA8P1_DISPLAY_MAX_BOXES; ++box_index)
    {
        lv_obj_t * box = lv_obj_create(card);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 1, 1);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(box, UI_COLOR_WARNING, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        g_waterfall_boxes[channel][box_index] = box;
    }
    ui_set_waterfall_axes(card, channel);

}

static void ui_update_nav_styles(void)
{
    const bool monitor = (g_active_page == UI_PAGE_MONITOR);
    lv_obj_set_style_bg_color(g_monitor_nav, monitor ? UI_COLOR_CARD : UI_COLOR_HEADER, 0);
    lv_obj_set_style_bg_color(g_recognition_nav, monitor ? UI_COLOR_HEADER : UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(g_monitor_nav_label, monitor ? UI_COLOR_TEXT : UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_color(g_recognition_nav_label, monitor ? UI_COLOR_MUTED : UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(g_monitor_nav_line, monitor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(g_recognition_nav_line, monitor ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

static void ui_update_header_status(void)
{
    const bool paused = (g_active_page == UI_PAGE_RECOGNITION) && !g_waterfall_running;
    const bool synthetic = g_live_signal_valid &&
        ((g_live_signal_frame.flags & RA8P1_DISPLAY_FLAG_SYNTHETIC) != 0U);
    const bool discontinuity = g_live_signal_valid &&
        ((g_live_signal_frame.flags & RA8P1_DISPLAY_FLAG_DISCONTINUITY) != 0U);
    const uint32_t model_flags = ui_cached_model_flags();
    const bool placeholder =
        (model_flags & RA8P1_MODEL_FLAG_PLACEHOLDER) != 0U;
    const bool preprocess_placeholder =
        (model_flags & RA8P1_MODEL_FLAG_PREPROCESS_PLACEHOLDER) != 0U;
    const char * text = !g_live_signal_valid ? "WAIT" :
                        (paused ? "PAUSE" :
                         (discontinuity ? "GAP" : (synthetic ? "SIM" : "LIVE")));
    const lv_color_t color = !g_live_signal_valid ? UI_COLOR_MUTED :
                             ((paused || discontinuity || synthetic) ?
                              UI_COLOR_WARNING : UI_COLOR_SUCCESS);
    if ((g_status_label == NULL) || (g_status_dot == NULL))
    {
        return;
    }
    lv_label_set_text(g_status_label, text);
    lv_obj_set_style_text_color(g_status_label, color, 0);
    lv_obj_set_style_bg_color(g_status_dot, color, 0);
    if (g_demo_badge_label != NULL)
    {
        lv_label_set_text(g_demo_badge_label, synthetic ? "SIM / NPU DEMO" :
                          (placeholder ? "NPU PLACEHOLDER" :
                           (preprocess_placeholder ? "PREPROCESS DEMO" : "MODEL ACTIVE")));
    }
}

static uint32_t ui_cycles_to_ms_x100(uint32_t cycles)
{
    return (uint32_t)((((uint64_t)cycles * 100U) +
                       (UI_CPU0_CYCLES_PER_MS / 2U)) /
                      UI_CPU0_CYCLES_PER_MS);
}

static uint32_t ui_score_percent(int32_t score_q15)
{
    uint32_t score;
    if (score_q15 <= 0)
    {
        return 0U;
    }
    score = (uint32_t)score_q15;
    if (score > 65535U)
    {
        score = 65535U;
    }
    return (uint32_t)((((uint64_t)score * 100U) + 32767U) / 65535U);
}

static uint32_t ui_presence_q15_fused(uint32_t class_index)
{
    uint32_t best = 0U;
    if (class_index >= UI_CLASS_COUNT)
    {
        return 0U;
    }
    for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
    {
        const ra8p1_display_frame_t * frame = ui_center_frame(center);
        if (frame != NULL)
        {
            const uint32_t value = frame->analysis.presence_q15[class_index];
            if (value > best)
            {
                best = value;
            }
        }
    }
    return best;
}

static uint32_t ui_presence_percent(uint32_t presence_q15)
{
    if (presence_q15 > RA8P1_PROBABILITY_ONE_Q15)
    {
        presence_q15 = RA8P1_PROBABILITY_ONE_Q15;
    }
    return (uint32_t)((((uint64_t)presence_q15 * 100U) +
                       (RA8P1_PROBABILITY_ONE_Q15 / 2U)) /
                       RA8P1_PROBABILITY_ONE_Q15);
}

static int8_t ui_flow_level_to_dbfs(uint8_t level)
{
    return (int8_t)(-120 + (int32_t)(((uint32_t)level * 120U + 127U) / 255U));
}

static void ui_flow_update_channel_metrics(uint32_t center_index,
                                           const ra8p1_display_frame_t * frame)
{
    uint32_t histogram[32] = {0U};
    uint32_t cumulative = 0U;
    uint32_t noise_bucket = 0U;
    uint32_t occupied = 0U;
    uint32_t peak_level = 0U;
    rf_ui_channel_metrics_t metrics;

    if ((frame == NULL) || (center_index >= RF_UI_CHANNEL_COUNT))
    {
        return;
    }

    for (uint32_t bin = 0U; bin < RA8P1_DISPLAY_SPECTRUM_BINS; ++bin)
    {
        const uint32_t level = frame->spectrum[0][bin];
        histogram[level >> 3U]++;
        if (level > peak_level)
        {
            peak_level = level;
        }
    }
    for (noise_bucket = 0U; noise_bucket < 32U; ++noise_bucket)
    {
        cumulative += histogram[noise_bucket];
        if (cumulative >= (RA8P1_DISPLAY_SPECTRUM_BINS / 4U))
        {
            break;
        }
    }

    const uint32_t threshold = ((noise_bucket + 3U) < 32U) ?
                               ((noise_bucket + 3U) << 3U) : 255U;
    for (uint32_t bin = 0U; bin < RA8P1_DISPLAY_SPECTRUM_BINS; ++bin)
    {
        if (frame->spectrum[0][bin] >= threshold)
        {
            occupied++;
        }
    }

    const uint32_t noise_level = ((noise_bucket << 3U) + 4U < 256U) ?
                                 ((noise_bucket << 3U) + 4U) : 255U;
    metrics.peak_dbfs = ui_flow_level_to_dbfs((uint8_t)peak_level);
    metrics.noise_floor_dbfs = ui_flow_level_to_dbfs((uint8_t)noise_level);
    metrics.occupancy_percent = (uint8_t)((occupied * 100U +
                                          (RA8P1_DISPLAY_SPECTRUM_BINS / 2U)) /
                                         RA8P1_DISPLAY_SPECTRUM_BINS);
    metrics.age_ms = 0U;
    (void) rf_ui_update_channel_metrics(center_index, &metrics);
}

static uint8_t ui_flow_frame_confidence(const ra8p1_display_frame_t * frame)
{
    uint32_t confidence = 0U;
    if (!ui_frame_npu_output_valid(frame))
    {
        return 0U;
    }

    for (uint32_t class_index = 0U; class_index < RF_UI_DETECTION_COUNT; ++class_index)
    {
        const uint32_t candidate = ui_presence_percent(frame->analysis.presence_q15[class_index]);
        if (candidate > confidence)
        {
            confidence = candidate;
        }
    }
    if (confidence == 0U)
    {
        confidence = ui_score_percent(frame->analysis.npu_score_q15);
    }
    return (uint8_t)((confidence > 100U) ? 100U : confidence);
}

static uint8_t ui_flow_object_to_detection_index(uint8_t object_id)
{
    static const uint8_t mapping[RF_V13_OBJECT_COUNT] =
    {
        0U, /* DJI */
        2U, /* AT9S */
        3U, /* T12 */
        1U  /* XIAOBAWANG */
    };
    return (object_id < RF_V13_OBJECT_COUNT) ? mapping[object_id] : UINT8_MAX;
}

static uint8_t ui_flow_detection_to_object_index(uint32_t detection_index)
{
    static const uint8_t mapping[RF_UI_DETECTION_COUNT] =
    {
        RF_V13_OBJECT_DJI,
        RF_V13_OBJECT_XIAOBAWANG,
        RF_V13_OBJECT_AT9S,
        RF_V13_OBJECT_T12
    };
    return (detection_index < RF_UI_DETECTION_COUNT) ?
           mapping[detection_index] : UINT8_MAX;
}

static uint8_t ui_flow_fusion_strength_percent(
    const rf_v27_activity_view_t * state)
{
    int32_t energy;
    uint32_t percent;
    if ((state == NULL) ||
        (state->activity_state == RF_V27_ACTIVITY_NO_RF_OBSERVED))
    {
        return 0U;
    }
    energy = state->on_evidence_q12;
    if (energy <= 0)
    {
        return 0U;
    }
    if (energy > RF_V13_ENERGY_MAX_Q12)
    {
        energy = RF_V13_ENERGY_MAX_Q12;
    }
    percent = ((uint32_t)energy * 100U +
               (RF_V13_ENERGY_MAX_Q12 / 2U)) /
              RF_V13_ENERGY_MAX_Q12;
    return (uint8_t)((percent > 100U) ? 100U : percent);
}

static void ui_flow_update_detections(void)
{
    rf_v27_activity_service_proof_t snapshot;
    rf_v27_activity_service_snapshot(&snapshot);

    for (uint32_t detection_index = 0U;
         detection_index < RF_UI_DETECTION_COUNT;
         ++detection_index)
    {
        const uint8_t object_id =
            ui_flow_detection_to_object_index(detection_index);
        rf_v27_activity_view_t state;
        const bool state_ready = rf_v27_activity_fusion_get(
            &snapshot.fusion,
            (rf_v13_object_id_t)object_id,
            &state) != 0;
        rf_ui_detection_t detection =
        {
            .state = RF_UI_DETECTION_INACTIVE,
            .confidence_percent = 0U,
            .channel_index = 0U
        };

        if (state_ready)
        {
            if (state.activity_state == RF_V27_ACTIVITY_WORKING)
            {
                detection.state = RF_UI_DETECTION_ACTIVE;
            }
            detection.confidence_percent =
                ui_flow_fusion_strength_percent(&state);
            if (state.last_positive_center_slot < RF_UI_CHANNEL_COUNT)
            {
                detection.channel_index =
                    state.last_positive_center_slot;
            }
        }
        (void)rf_ui_update_detection(detection_index, &detection);
    }
}

static bool ui_flow_rf_window_complete(uint32_t center,
                                       uint32_t session_id,
                                       uint32_t window_sequence)
{
    if (center >= RA8P1_CENTER_COUNT)
    {
        return false;
    }
    for (uint32_t slot = 0U; slot < UI_TILE_LOGICAL_RUN_SLOTS; ++slot)
    {
        const ui_tile_logical_run_t * run =
            &g_tile_logical_runs[center][slot];
        if (run->valid &&
            (run->session_id == session_id) &&
            (run->window_sequence == window_sequence) &&
            (run->last_time_start == (RA8P1_DISPLAY_TILE_HEIGHT - 1U)))
        {
            return true;
        }
    }
    return false;
}

static void ui_flow_try_commit_rf_box_batch(uint32_t center,
                                            ui_flow_rf_box_batch_t * batch)
{
    if ((batch == NULL) || !batch->pending ||
        !ui_flow_rf_window_complete(center,
                                    batch->session_id,
                                    batch->window_sequence))
    {
        return;
    }
    if (rf_ui_update_rf_boxes(center,
                              batch->boxes,
                              batch->count,
                              batch->session_id,
                              batch->window_sequence))
    {
        batch->pending = false;
    }
}

static ui_flow_rf_box_batch_t * ui_flow_rf_box_batch_for_frame(
    uint32_t center,
    uint32_t session_id,
    uint32_t window_sequence)
{
    ui_flow_rf_box_batch_t * replacement = NULL;
    for (uint32_t slot = 0U; slot < UI_TILE_LOGICAL_RUN_SLOTS; ++slot)
    {
        ui_flow_rf_box_batch_t * batch =
            &g_flow_rf_box_batches[center][slot];
        if (batch->pending &&
            (batch->session_id == session_id) &&
            (batch->window_sequence == window_sequence))
        {
            return batch;
        }
        if (!batch->pending && (replacement == NULL))
        {
            replacement = batch;
        }
    }
    if (replacement == NULL)
    {
        replacement = &g_flow_rf_box_batches[center]
            [window_sequence & (UI_TILE_LOGICAL_RUN_SLOTS - 1U)];
    }
    return replacement;
}

static void ui_flow_prepare_rf_boxes(const ra8p1_display_frame_t * frame)
{
    uint32_t center;
    ui_flow_rf_box_batch_t * batch;
    if (frame == NULL)
    {
        return;
    }
    center = ui_frame_center_index(frame);
    if (center >= RA8P1_CENTER_COUNT)
    {
        return;
    }
    batch = ui_flow_rf_box_batch_for_frame(
        center, frame->session_id, frame->analysis.window_sequence);
    memset(batch, 0, sizeof(*batch));
    batch->pending = true;
    batch->session_id = frame->session_id;
    batch->window_sequence = frame->analysis.window_sequence;

    if (ui_frame_npu_output_valid(frame))
    {
        for (uint32_t index = 0U;
             (index < frame->analysis.box_count) &&
             (batch->count < RF_UI_MAX_RF_BOXES);
             ++index)
        {
            const ra8p1_detection_box_t * source =
                &frame->analysis.boxes[index];
            const uint8_t source_flags =
                (uint8_t)(source->metadata >>
                          RA8P1_DISPLAY_BOX_FLAGS_SHIFT);
            const uint8_t detection_index =
                ui_flow_object_to_detection_index(source->class_id);
            rf_ui_rf_box_t * destination;
            if (((source_flags &
                  RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID) == 0U) ||
                (detection_index >= RF_UI_DETECTION_COUNT))
            {
                continue;
            }
            destination = &batch->boxes[batch->count++];
            destination->frequency_start_q8 =
                source->frequency_start_q8;
            destination->time_start_q8 = source->time_start_q8;
            destination->frequency_span_q8 = source->frequency_span_q8;
            destination->time_span_q8 = source->time_span_q8;
            destination->detection_index = detection_index;
            destination->confidence_percent = (uint8_t)(
                ((uint32_t)source->score * 100U + 127U) / 255U);
            destination->flags = source_flags;
            destination->reserved = 0U;
        }
    }
    ui_flow_try_commit_rf_box_batch(center, batch);
}

static void ui_flow_commit_rf_boxes_for_tile(
    const ra8p1_display_tile_payload_t * tile)
{
    if ((tile == NULL) ||
        (tile->center_index >= RA8P1_CENTER_COUNT) ||
        (tile->novel_time_start != (RA8P1_DISPLAY_TILE_HEIGHT - 1U)))
    {
        return;
    }
    for (uint32_t slot = 0U; slot < UI_TILE_LOGICAL_RUN_SLOTS; ++slot)
    {
        ui_flow_rf_box_batch_t * batch =
            &g_flow_rf_box_batches[tile->center_index][slot];
        if (batch->pending &&
            (batch->session_id == tile->session_id) &&
            (batch->window_sequence == tile->window_sequence))
        {
            ui_flow_try_commit_rf_box_batch(tile->center_index, batch);
        }
    }
}

void lvgl_app_activity_update(void)
{
    if (UI_SINGLE_FLOW_ENABLED)
    {
        ui_flow_update_detections();
    }
}

void lvgl_app_activity_round_update(
    const rf_v27_activity_round_decision_t * decision)
{
    rf_ui_fusion_round_t ui_round;
    if (!UI_SINGLE_FLOW_ENABLED || (decision == NULL))
    {
        return;
    }

    memset(&ui_round, 0, sizeof(ui_round));
    ui_round.message_sequence = decision->message_sequence;
    ui_round.round_index = decision->round_index;
    memcpy(ui_round.session_id,
           decision->display_session_id,
           sizeof(ui_round.session_id));
    memcpy(ui_round.window_sequence,
           decision->display_window_sequence,
           sizeof(ui_round.window_sequence));
    ui_round.identity_mask = decision->display_identity_mask;
    ui_round.identity_conflict_mask =
        decision->display_identity_conflict_mask;
    if ((decision->flags & RF_V27_ROUND_DECISION_HAS_MESSAGE) != 0U)
    {
        ui_round.flags |= RF_UI_FUSION_ROUND_HAS_MESSAGE;
    }
    if ((decision->flags & RF_V27_ROUND_DECISION_OUTPUT_VALID) != 0U)
    {
        ui_round.flags |= RF_UI_FUSION_ROUND_OUTPUT_VALID;
    }
    if ((decision->flags & RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET) != 0U)
    {
        ui_round.flags |= RF_UI_FUSION_ROUND_CPU0_RESET;
    }

    for (uint32_t object = 0U; object < RF_V13_OBJECT_COUNT; ++object)
    {
        const uint8_t detection = ui_flow_object_to_detection_index(
            (uint8_t)object);
        if (detection < RF_UI_DETECTION_COUNT)
        {
            ui_round.activity_state[detection] =
                decision->object_activity_state[object];
        }
    }
    rf_ui_apply_fusion_round(&ui_round);
}

static uint16_t ui_flow_scan_rate_x10(void)
{
    const uint32_t rate_x100 = (g_inference_rate_x100 != 0U) ?
                               g_inference_rate_x100 : g_window_rate_x100;
    const uint32_t rate_x10 = (rate_x100 + 5U) / 10U;
    return (uint16_t)((rate_x10 > UINT16_MAX) ? UINT16_MAX : rate_x10);
}

static bool ui_flow_append_waterfall_tile(const ra8p1_display_tile_payload_t * tile)
{
    const uint32_t generation_start_cycles = DWT->CYCCNT;
    bool appended = false;

    if (rf_ui_update_waterfall_rows(tile->center_index,
                                    tile->levels,
                                    RA8P1_DISPLAY_TILE_ROW_BYTES,
                                    1U,
                                    RA8P1_DISPLAY_TILE_ROW_BYTES))
    {
        const uint32_t generation_cycles = DWT->CYCCNT - generation_start_cycles;
        g_ui_waterfall_gen_cycles += generation_cycles;
        if (generation_cycles > g_ui_waterfall_gen_max_cycles)
        {
            g_ui_waterfall_gen_max_cycles = generation_cycles;
        }
        g_ui_waterfall_columns_generated++;
        g_ui_waterfall_tiles_consumed++;
        g_waterfall_rendered_session = tile->session_id;
        g_waterfall_rendered_sequence = tile->sequence;
        appended = true;
    }
    else
    {
        g_ui_waterfall_tiles_dropped++;
    }
    return appended;
}

static void ui_update_class_strip(void)
{
    const bool output_valid = ui_any_npu_output_valid();
    const uint32_t model_flags = ui_cached_model_flags();
    const bool placeholder = (model_flags & RA8P1_MODEL_FLAG_PLACEHOLDER) != 0U;
    for (uint32_t index = 0U; index < UI_CLASS_COUNT; ++index)
    {
        const uint32_t presence_q15 = ui_presence_q15_fused(index);
        const uint32_t score = ui_presence_percent(presence_q15);
        const bool active = output_valid &&
                            (presence_q15 >= RA8P1_PROBABILITY_HALF_Q15);
        if (g_class_state_label[index] != NULL)
        {
            lv_label_set_text(g_class_state_label[index],
                              !output_valid ? "NO OUTPUT" :
                              (placeholder ? (active ? "RAW HIGH" : "RAW LOW") :
                               (active ? "PRESENT" : "CLEAR")));
        }
        if (g_class_score_label[index] != NULL)
        {
            if (output_valid)
            {
                lv_label_set_text_fmt(g_class_score_label[index], "%lu%%", (unsigned long)score);
            }
            else
            {
                lv_label_set_text(g_class_score_label[index], "--");
            }
        }
        if (g_class_tag[index] != NULL)
        {
            lv_obj_set_style_opa(g_class_tag[index], active ? LV_OPA_COVER : (lv_opa_t)150U, 0);
        }
    }
}

static void ui_update_live_text(void)
{
    uint32_t sample_rate_hz;
    uint32_t stft_ms_x100;
    uint32_t npu_ms_x100;
    uint32_t end_to_end_ms_x100;
    uint32_t selected_class;
    const char * class_name;
    const char * npu_state;

    if (!g_live_signal_valid)
    {
        if (g_header_subtitle != NULL)
        {
            lv_label_set_text(g_header_subtitle,
                              (g_active_page == UI_PAGE_MONITOR) ?
                              "WAITING FOR IQ  |  FFT 1024  |  SPECTRUM PAGE" :
                              "NPU PLACEHOLDER  |  WAITING FOR WINDOW");
        }
        if ((g_active_page == UI_PAGE_MONITOR) &&
            (g_monitor_status_label != NULL) &&
            g_live_telemetry_valid)
        {
            lv_label_set_text_fmt(g_monitor_status_label,
                                  "PIPELINE %lu  |  %lu.%03lu Mb/s  |  QUEUE %lu  |  DROPS %lu  |  WAITING FOR WINDOW",
                                  (unsigned long)g_live_telemetry.pipeline_state,
                                  (unsigned long)(g_live_telemetry.iq_payload_mbps_x1000 / 1000U),
                                  (unsigned long)(g_live_telemetry.iq_payload_mbps_x1000 % 1000U),
                                  (unsigned long)g_live_telemetry.ring_high_watermark,
                                  (unsigned long)g_live_telemetry.ingress_drops);
        }
        if ((g_active_page == UI_PAGE_RECOGNITION) && (g_analysis_status_label != NULL))
        {
            lv_label_set_text(g_analysis_status_label,
                              "NPU PLACEHOLDER  |  WAITING FOR WINDOW\n"
                              "STFT --  |  NPU --  |  END-TO-END --\n"
                              "NO ANALYSIS FRAME");
        }
        ui_update_header_status();
        return;
    }

    sample_rate_hz = (g_live_signal_frame.analysis.source_sample_rate_hz != 0U) ?
                     g_live_signal_frame.analysis.source_sample_rate_hz :
                     g_live_signal_frame.sample_rate_hz;
    stft_ms_x100 = ui_cycles_to_ms_x100(g_live_signal_frame.analysis.stft_cycles);
    npu_ms_x100 = ui_cycles_to_ms_x100(g_live_signal_frame.analysis.npu_cycles);
    end_to_end_ms_x100 = ui_cycles_to_ms_x100(g_live_signal_frame.analysis.end_to_end_cycles);
    selected_class = g_live_signal_frame.analysis.npu_class;
    npu_state = ui_npu_output_valid() ? "OUTPUT READY" : "NO OUTPUT";
    class_name = (ui_npu_output_valid() && (selected_class < UI_CLASS_COUNT)) ?
                 g_drones[selected_class].short_name : "NONE";

    for (uint32_t channel = 0U; channel < UI_CHANNEL_COUNT; ++channel)
    {
        const ra8p1_display_frame_t * frame = ui_center_frame(channel);
        const bool present = (frame != NULL);
        const uint64_t center_hz = (frame != NULL) ?
            (((uint64_t)frame->analysis.center_frequency_high << 32U) |
             frame->analysis.center_frequency_low) : 0ULL;
        const uint32_t center_mhz = (uint32_t)(center_hz / 1000000ULL);
        const uint32_t occupancy = ui_channel_occupancy_percent(channel);
        if ((g_active_page == UI_PAGE_MONITOR) && (g_channel_center_label[channel] != NULL))
        {
            if (present && (center_mhz != 0U))
            {
                lv_label_set_text_fmt(g_channel_center_label[channel], "%lu.%03lu GHz",
                                      (unsigned long)(center_mhz / 1000U),
                                      (unsigned long)(center_mhz % 1000U));
            }
            else if (!present)
            {
                lv_label_set_text(g_channel_center_label[channel], "INACTIVE");
            }
            else
            {
                lv_label_set_text(g_channel_center_label[channel], "CENTER UNKNOWN");
            }
        }
        if ((g_active_page == UI_PAGE_MONITOR) && (g_channel_meta_label[channel] != NULL))
        {
            if (present)
            {
                lv_label_set_text_fmt(g_channel_meta_label[channel], "PEAK BIN %lu   PWR %08lX",
                                      (unsigned long)frame->peak_bin[0],
                                      (unsigned long)frame->peak_power_q16[0]);
            }
            else
            {
                lv_label_set_text(g_channel_meta_label[channel], "NO STREAM");
            }
        }
        if ((g_active_page == UI_PAGE_MONITOR) &&
            (g_channel_occupancy_label[channel] != NULL))
        {
            if (occupancy == UINT32_MAX)
            {
                lv_label_set_text(g_channel_occupancy_label[channel], "OCC --");
                lv_obj_set_style_text_color(g_channel_occupancy_label[channel], UI_COLOR_MUTED, 0);
            }
            else
            {
                lv_label_set_text_fmt(g_channel_occupancy_label[channel], "OCC %lu%%",
                                      (unsigned long)occupancy);
                lv_obj_set_style_text_color(g_channel_occupancy_label[channel],
                                            (occupancy >= 70U) ? UI_COLOR_DANGER :
                                            ((occupancy >= 35U) ?
                                             UI_COLOR_WARNING : UI_COLOR_SUCCESS), 0);
            }
        }
        if ((g_active_page == UI_PAGE_RECOGNITION) && (g_waterfall_title_label[channel] != NULL))
        {
            if (present && (center_mhz != 0U))
            {
                lv_label_set_text_fmt(g_waterfall_title_label[channel],
                                      "%s  |  %lu.%03lu GHz  |  HISTORY",
                                      g_channels[channel].id,
                                      (unsigned long)(center_mhz / 1000U),
                                      (unsigned long)(center_mhz % 1000U));
            }
            else
            {
                lv_label_set_text_fmt(g_waterfall_title_label[channel], "%s  |  %s",
                                      g_channels[channel].id,
                                      present ? "SIM / CENTER UNKNOWN" : "NO STREAM");
            }
        }
    }

    if ((g_active_page == UI_PAGE_MONITOR) && (g_monitor_status_label != NULL))
    {
        lv_label_set_text_fmt(g_monitor_status_label,
                              "%lu.%03lu MS/s  |  FFT %lu  |  WINDOW %lu SAMPLES  |  QUEUE %lu  |  DROPS %lu  |  %lu-BIT IQ",
                              (unsigned long)(sample_rate_hz / 1000000U),
                              (unsigned long)((sample_rate_hz / 1000U) % 1000U),
                              (unsigned long)g_live_signal_frame.fft_size,
                              (unsigned long)g_live_signal_frame.analysis.window_sample_count,
                              (unsigned long)g_live_signal_frame.analysis.queue_depth,
                              (unsigned long)g_live_signal_frame.analysis.ingress_drops,
                              (unsigned long)g_live_signal_frame.analysis.valid_bits);
    }
    if ((g_active_page == UI_PAGE_RECOGNITION) && (g_analysis_status_label != NULL))
    {
        const uint32_t box_count = (g_live_signal_frame.analysis.box_count > RA8P1_DISPLAY_MAX_BOXES) ?
                                   RA8P1_DISPLAY_MAX_BOXES : g_live_signal_frame.analysis.box_count;
        lv_label_set_text_fmt(g_analysis_status_label,
                              "NPU PLACEHOLDER  |  %s  |  CLASS %s  |  SCORE %lu%%  |  BOXES %lu\n"
                              "STFT %lu.%02lu ms  |  NPU %lu.%02lu ms  |  END-TO-END %lu.%02lu ms\n"
                              "WINDOW #%lu  |  WIN %lu.%02lu Hz  |  AI %lu.%02lu Hz  |  Q %lu  |  DROP %lu",
                              npu_state,
                              class_name,
                              (unsigned long)(ui_npu_output_valid() ?
                                              ui_score_percent(g_live_signal_frame.analysis.npu_score_q15) : 0U),
                              (unsigned long)(ui_npu_output_valid() ? box_count : 0U),
                              (unsigned long)(stft_ms_x100 / 100U),
                              (unsigned long)(stft_ms_x100 % 100U),
                              (unsigned long)(npu_ms_x100 / 100U),
                              (unsigned long)(npu_ms_x100 % 100U),
                              (unsigned long)(end_to_end_ms_x100 / 100U),
                              (unsigned long)(end_to_end_ms_x100 % 100U),
                              (unsigned long)g_live_signal_frame.analysis.window_sequence,
                              (unsigned long)(g_window_rate_x100 / 100U),
                              (unsigned long)(g_window_rate_x100 % 100U),
                              (unsigned long)(g_inference_rate_x100 / 100U),
                              (unsigned long)(g_inference_rate_x100 % 100U),
                              (unsigned long)g_live_signal_frame.analysis.queue_depth,
                              (unsigned long)g_live_signal_frame.analysis.ingress_drops);
    }

    if (g_active_page == UI_PAGE_MONITOR)
    {
        uint32_t active_centers = 0U;
        for (uint32_t center = 0U; center < UI_CHANNEL_COUNT; ++center)
        {
            if (ui_center_frame(center) != NULL)
            {
                active_centers++;
            }
        }
        lv_label_set_text_fmt(g_header_subtitle,
                              "%lu CENTER%s  |  %lu.%03lu MS/s  |  FFT %lu",
                              (unsigned long)active_centers,
                              (active_centers == 1U) ? "" : "S",
                              (unsigned long)(sample_rate_hz / 1000000U),
                              (unsigned long)((sample_rate_hz / 1000U) % 1000U),
                              (unsigned long)g_live_signal_frame.fft_size);
    }
    else
    {
        lv_label_set_text_fmt(g_header_subtitle,
                              "STFT %lu.%02lu  |  NPU %lu.%02lu  |  E2E %lu.%02lu ms",
                              (unsigned long)(stft_ms_x100 / 100U),
                              (unsigned long)(stft_ms_x100 % 100U),
                              (unsigned long)(npu_ms_x100 / 100U),
                              (unsigned long)(npu_ms_x100 % 100U),
                              (unsigned long)(end_to_end_ms_x100 / 100U),
                              (unsigned long)(end_to_end_ms_x100 % 100U));
    }

    if (g_active_page == UI_PAGE_RECOGNITION)
    {
        ui_update_class_strip();
    }
    ui_update_header_status();
}

static void ui_set_active_page(ui_page_t page)
{
    const bool changed = (g_active_page != page);
    g_active_page = page;

    if (page == UI_PAGE_MONITOR)
    {
        ui_waterfall_overlay_set_enabled(false);
        lv_obj_remove_flag(g_monitor_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_recognition_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_drone_status_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_run_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_header_title, "SPECTRUM MONITOR");
        if (g_spectrum_content_dirty)
        {
            g_spectrum_dirty_mask |= ((1UL << UI_CHANNEL_COUNT) - 1U);
            ui_update_spectra();
            g_spectrum_content_dirty = (g_spectrum_dirty_mask != 0U);
            g_last_spectrum_update_tick = g_lvgl_tick_ms;
        }
    }
    else
    {
        lv_obj_add_flag(g_monitor_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_recognition_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_drone_status_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_run_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_header_title, "WATERFALL + INFERENCE");
        ui_seed_live_waterfalls();
        /* Keep the panel on the proven single-layer GLCDC path.  The optional
         * Layer 2 overlay competes for SDRAM bandwidth and can underflow while
         * CPU0 is publishing analysis results, so the waterfall stays in the
         * LVGL software-rendered framebuffer. */
        ui_waterfall_overlay_set_enabled(false);
        ui_draw_mask_preview();
        g_last_mask_update_tick = g_lvgl_tick_ms;
        g_mask_dirty = false;
    }

    ui_update_live_text();
    g_live_text_dirty = false;
    g_last_text_update_tick = g_lvgl_tick_ms;
    ui_update_header_status();
    ui_update_nav_styles();
    if (changed)
    {
        lv_obj_invalidate((page == UI_PAGE_MONITOR) ? g_monitor_page : g_recognition_page);
    }
}

static void ui_waterfall_page_event(lv_event_t * event)
{
    if (LV_EVENT_CLICKED == lv_event_get_code(event))
    {
        ui_set_active_page(UI_PAGE_RECOGNITION);
    }
}

static void ui_monitor_page_event(lv_event_t * event)
{
    if (LV_EVENT_CLICKED == lv_event_get_code(event))
    {
        ui_set_active_page(UI_PAGE_MONITOR);
    }
}

static void ui_run_button_event(lv_event_t * event)
{
    if (LV_EVENT_CLICKED != lv_event_get_code(event))
    {
        return;
    }
    g_waterfall_running = !g_waterfall_running;
    lv_label_set_text(g_run_button_label, g_waterfall_running ? "II" : ">");
    ui_update_header_status();
}

static lv_obj_t * ui_create_nav_button(lv_obj_t * screen,
                                       int32_t x,
                                       const char * text,
                                       lv_event_cb_t callback,
                                       lv_obj_t ** line_out,
                                       lv_obj_t ** label_out)
{
    lv_obj_t * button = lv_button_create(screen);
    lv_obj_set_size(button, UI_SCREEN_WIDTH / 2U, UI_FOOTER_HEIGHT);
    lv_obj_set_pos(button, x, UI_SCREEN_HEIGHT - UI_FOOTER_HEIGHT);
    lv_obj_set_style_bg_color(button, UI_COLOR_HEADER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t * line = lv_obj_create(button);
    lv_obj_set_size(line, 286, 3);
    lv_obj_set_pos(line, 113, 0);
    lv_obj_set_style_bg_color(line, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_MUTED, 0);
    lv_obj_center(label);
    lv_obj_move_foreground(label);
    *line_out = line;
    *label_out = label;
    return button;
}

static void ui_create_drone_statuses(lv_obj_t * header)
{
    uint32_t index;
    g_drone_status_strip = lv_obj_create(header);
    lv_obj_set_size(g_drone_status_strip, 528, 52);
    lv_obj_set_pos(g_drone_status_strip, 238, 0);
    lv_obj_set_style_bg_opa(g_drone_status_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_drone_status_strip, 0, 0);
    lv_obj_set_style_radius(g_drone_status_strip, 0, 0);
    lv_obj_set_style_pad_all(g_drone_status_strip, 0, 0);
    lv_obj_remove_flag(g_drone_status_strip, LV_OBJ_FLAG_SCROLLABLE);

    for (index = 0U; index < UI_CLASS_COUNT; index++)
    {
        const ui_drone_info_t * info = &g_drones[index];
        const lv_opa_t opacity = (lv_opa_t) (140U + ((uint32_t) info->confidence * 115U / 100U));
        lv_obj_t * tag = lv_obj_create(g_drone_status_strip);
        lv_obj_set_size(tag, 124, 36);
        lv_obj_set_pos(tag, (int32_t) (index * 130U), 8);
        lv_obj_set_style_bg_color(tag, UI_COLOR_CARD_ALT, 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tag, lv_color_hex(info->color), 0);
        lv_obj_set_style_border_width(tag, 1, 0);
        lv_obj_set_style_radius(tag, 4, 0);
        lv_obj_set_style_pad_all(tag, 0, 0);
        lv_obj_set_style_opa(tag, opacity, 0);
        lv_obj_remove_flag(tag, LV_OBJ_FLAG_SCROLLABLE);
        g_class_tag[index] = tag;

        lv_obj_t * bar = lv_obj_create(tag);
        lv_obj_set_size(bar, 3, 22);
        lv_obj_set_pos(bar, 6, 7);
        lv_obj_set_style_bg_color(bar, lv_color_hex(info->color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * name = ui_create_label(tag, info->short_name, &lv_font_montserrat_14,
                                          UI_COLOR_TEXT, 15, 2);
        lv_obj_set_width(name, 78);
        lv_obj_t * state = ui_create_label(tag, info->state, &lv_font_montserrat_14,
                                           UI_COLOR_MUTED, 15, 19);
        lv_obj_set_width(state, 88);
        g_class_state_label[index] = state;
        lv_obj_t * confidence = lv_label_create(tag);
        lv_label_set_text_fmt(confidence, "%u%%", (unsigned) info->confidence);
        lv_obj_set_style_text_font(confidence, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(confidence, lv_color_hex(info->color), 0);
        lv_obj_set_width(confidence, 38);
        lv_obj_set_style_text_align(confidence, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(confidence, 84, 10);
        g_class_score_label[index] = confidence;
    }
}

static void ui_create_header(lv_obj_t * screen)
{
    lv_obj_t * header = lv_obj_create(screen);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, UI_HEADER_HEIGHT);
    lv_obj_set_pos(header, 0, 0);
    ui_set_container_style(header, UI_COLOR_HEADER);
    lv_obj_set_style_border_color(header, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    g_header_title = ui_create_label(header, "SPECTRUM MONITOR", &lv_font_montserrat_20,
                                     UI_COLOR_TEXT, 12, 3);
    g_header_subtitle = ui_create_label(header, "WAITING FOR IQ  |  FFT 1024  |  SPECTRUM PAGE",
                                        &lv_font_montserrat_14, UI_COLOR_MUTED, 12, 29);

    g_status_dot = lv_obj_create(header);
    lv_obj_set_size(g_status_dot, 8, 8);
    lv_obj_set_pos(g_status_dot, 704, 22);
    lv_obj_set_style_bg_color(g_status_dot, UI_COLOR_SUCCESS, 0);
    lv_obj_set_style_bg_opa(g_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_status_dot, 0, 0);
    lv_obj_set_style_radius(g_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(g_status_dot, LV_OBJ_FLAG_CLICKABLE);

    g_status_label = ui_create_label(header, "WAIT", &lv_font_montserrat_14,
                                     UI_COLOR_MUTED, 718, 17);
    g_fps_label = lv_label_create(header);
    lv_label_set_text(g_fps_label, "0.00 FPS");
    lv_obj_set_style_text_font(g_fps_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_fps_label, UI_COLOR_MUTED, 0);
    lv_obj_set_width(g_fps_label, 82);
    lv_obj_set_style_text_align(g_fps_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_fps_label, 758, 17);

    g_demo_badge = lv_obj_create(header);
    lv_obj_set_size(g_demo_badge, 126, 24);
    lv_obj_set_pos(g_demo_badge, 842, 14);
    lv_obj_set_style_bg_color(g_demo_badge, lv_color_hex(0x282317U), 0);
    lv_obj_set_style_bg_opa(g_demo_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_demo_badge, lv_color_hex(0x66522BU), 0);
    lv_obj_set_style_border_width(g_demo_badge, 1, 0);
    lv_obj_set_style_radius(g_demo_badge, 4, 0);
    lv_obj_set_style_pad_all(g_demo_badge, 0, 0);
    lv_obj_remove_flag(g_demo_badge, LV_OBJ_FLAG_CLICKABLE);
    g_demo_badge_label = ui_create_label(g_demo_badge, "NPU PLACEHOLDER",
                                         &lv_font_montserrat_14, UI_COLOR_WARNING, 5, 4);

    g_run_button = lv_button_create(header);
    lv_obj_set_size(g_run_button, 40, 40);
    lv_obj_set_pos(g_run_button, 976, 6);
    lv_obj_set_style_bg_color(g_run_button, UI_COLOR_CARD_ALT, 0);
    lv_obj_set_style_bg_color(g_run_button, lv_color_hex(0x27413CU), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(g_run_button, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(g_run_button, 1, 0);
    lv_obj_set_style_radius(g_run_button, 4, 0);
    lv_obj_set_style_pad_all(g_run_button, 0, 0);
    lv_obj_add_event_cb(g_run_button, ui_run_button_event, LV_EVENT_CLICKED, NULL);
    g_run_button_label = lv_label_create(g_run_button);
    lv_label_set_text(g_run_button_label, "II");
    lv_obj_set_style_text_font(g_run_button_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_run_button_label, UI_COLOR_TEXT, 0);
    lv_obj_center(g_run_button_label);

    ui_create_drone_statuses(header);
}

static void ui_create(void)
{
    lv_obj_t * screen = lv_screen_active();
    ui_set_container_style(screen, UI_COLOR_BACKGROUND);

    ui_create_header(screen);

    g_monitor_page = lv_obj_create(screen);
    lv_obj_set_size(g_monitor_page, UI_SCREEN_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_pos(g_monitor_page, 0, UI_HEADER_HEIGHT);
    ui_set_container_style(g_monitor_page, UI_COLOR_BACKGROUND);

    g_recognition_page = lv_obj_create(screen);
    lv_obj_set_size(g_recognition_page, UI_SCREEN_WIDTH, UI_CONTENT_HEIGHT);
    lv_obj_set_pos(g_recognition_page, 0, UI_HEADER_HEIGHT);
    ui_set_container_style(g_recognition_page, UI_COLOR_BACKGROUND);
    lv_obj_add_flag(g_recognition_page, LV_OBJ_FLAG_HIDDEN);

    uint32_t channel;
    for (channel = 0U; channel < UI_CHANNEL_COUNT; channel++)
    {
        ui_create_spectrum_card(channel);
        ui_create_waterfall_card(channel);
    }

    g_monitor_status_label = NULL;
    g_mask_canvas = NULL;
    g_analysis_status_label = NULL;
    /* The compact diagnostics occupy the second row in the legacy two-card
     * layout.  Four fixed-center cards use that row, so their diagnostics stay
     * in the header, class strip, and per-card overlays. */
    if (UI_CHANNEL_COUNT <= 2U)
    {
        g_monitor_status_label = ui_create_label(g_monitor_page,
                                                 "WAITING FOR IQ  |  FFT 1024  |  QUEUE 0  |  DROPS 0",
                                                 &lv_font_montserrat_14,
                                                 UI_COLOR_MUTED,
                                                 14,
                                                 264);
        lv_obj_set_width(g_monitor_status_label, UI_SCREEN_WIDTH - 28U);

        g_mask_canvas = lv_canvas_create(g_recognition_page);
        lv_canvas_set_buffer(g_mask_canvas,
                             g_mask_buffer,
                             UI_MASK_PREVIEW_WIDTH,
                             UI_MASK_PREVIEW_HEIGHT,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(g_mask_canvas, 14, 258);
        lv_obj_set_style_border_color(g_mask_canvas, UI_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(g_mask_canvas, 1, 0);
        g_analysis_status_label = ui_create_label(g_recognition_page,
                                                  "NPU PLACEHOLDER  |  WAITING FOR WINDOW",
                                                  &lv_font_montserrat_14,
                                                  UI_COLOR_MUTED,
                                                  288,
                                                  262);
        lv_obj_set_width(g_analysis_status_label, UI_SCREEN_WIDTH - 302U);

        (void) ui_create_label(g_recognition_page,
                               "MASK 32 x 16  |  BOXES / CLASS OUTPUT",
                               &lv_font_montserrat_14,
                               UI_COLOR_AXIS,
                               14,
                               391);
    }

    g_monitor_nav = ui_create_nav_button(screen, 0, "SPECTRUM",
                                          ui_monitor_page_event, &g_monitor_nav_line,
                                          &g_monitor_nav_label);
    g_recognition_nav = ui_create_nav_button(screen, UI_SCREEN_WIDTH / 2U,
                                             "WATERFALL + AI",
                                             ui_waterfall_page_event, &g_recognition_nav_line,
                                             &g_recognition_nav_label);
    FSP_PARAMETER_NOT_USED(g_monitor_nav);
    FSP_PARAMETER_NOT_USED(g_recognition_nav);

    ui_set_active_page(UI_PAGE_MONITOR);
}

static void ui_data_timer_callback(lv_timer_t * timer)
{
    FSP_PARAMETER_NOT_USED(timer);
    g_signal_phase++;
}

static void ui_fps_update(lv_timer_t * timer)
{
    FSP_PARAMETER_NOT_USED(timer);
    const uint32_t tick_ms = g_lvgl_tick_ms;
    const uint32_t frame_count = g_presented_frame_count;
    const uint32_t content_frame_count = g_ui_content_frame_count;
    const uint32_t elapsed_ms = tick_ms - g_fps_last_tick_ms;
    const uint32_t content_elapsed_ms = tick_ms - g_content_last_tick_ms;
    const uint32_t elapsed_frames = frame_count - g_fps_last_frame_count;
    const uint32_t elapsed_content_frames = content_frame_count -
                                             g_content_last_frame_count;
    const uint32_t elapsed_underflows = g_display_diag.glcdc_underflows -
                                        g_fps_last_underflow_count;
    if (0U == elapsed_ms)
    {
        return;
    }

    g_presented_fps_millihz = (uint32_t)((((uint64_t)elapsed_frames * 1000000U) +
                                           (elapsed_ms / 2U)) / elapsed_ms);
    g_content_fps_millihz = (content_elapsed_ms == 0U) ? 0U :
        (uint32_t)((((uint64_t)elapsed_content_frames * 1000000U) +
                    (content_elapsed_ms / 2U)) / content_elapsed_ms);
    g_underflow_rate_millihz = (uint32_t)((((uint64_t)elapsed_underflows * 1000000U) +
                                            (elapsed_ms / 2U)) / elapsed_ms);
    if (g_fps_label != NULL)
    {
        const uint32_t fps_x100 = (g_content_fps_millihz + 5U) / 10U;
        lv_label_set_text_fmt(g_fps_label, "%lu.%02lu FPS",
                              (unsigned long) (fps_x100 / 100U),
                              (unsigned long) (fps_x100 % 100U));
    }

    if (g_lvgl_refresh_count > 0U)
    {
        g_display_diag.lvgl_refresh_avg_cycles =
            (uint32_t) (g_lvgl_refresh_cycles_total / g_lvgl_refresh_count);
        g_display_diag.lvgl_refresh_max_cycles = g_lvgl_refresh_max_cycles;
    }
    if (g_lvgl_flush_wait_count > 0U)
    {
        g_display_diag.lvgl_flush_wait_avg_cycles =
            (uint32_t) (g_lvgl_flush_wait_cycles_total / g_lvgl_flush_wait_count);
        g_display_diag.lvgl_flush_wait_max_cycles = g_lvgl_flush_wait_max_cycles;
    }
    if (UI_SINGLE_FLOW_ENABLED)
    {
        const uint32_t panel_millihz =
            (g_display_diag.measured_refresh_millihz != 0U) ?
            g_display_diag.measured_refresh_millihz : g_display_diag.refresh_millihz;
        const uint32_t render_max_us = (SystemCoreClock == 0U) ? 0U :
            (uint32_t)((((uint64_t)g_lvgl_refresh_max_cycles * 1000000U) +
                        (SystemCoreClock / 2U)) / SystemCoreClock);
        rf_ui_set_render_metrics(panel_millihz,
                                 g_presented_fps_millihz,
                                 render_max_us,
                                 g_display_diag.glcdc_underflows);
    }
    g_display_diag.lvgl_profile_updates++;
    g_lvgl_refresh_cycles_total = 0U;
    g_lvgl_refresh_max_cycles = 0U;
    g_lvgl_refresh_count = 0U;
    g_lvgl_flush_wait_cycles_total = 0U;
    g_lvgl_flush_wait_max_cycles = 0U;
    g_lvgl_flush_wait_count = 0U;
    g_fps_last_tick_ms = tick_ms;
    g_fps_last_frame_count = frame_count;
    g_content_last_tick_ms = tick_ms;
    g_content_last_frame_count = content_frame_count;
    g_fps_last_underflow_count = g_display_diag.glcdc_underflows;
}

static void lvgl_display_event_callback(lv_event_t * event)
{
    if (0U == g_display_diag.fps_counter_enabled)
    {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (LV_EVENT_REFR_START == code)
    {
        g_lvgl_refresh_start_cycles = DWT->CYCCNT;
    }
    else if (LV_EVENT_REFR_READY == code)
    {
        const uint32_t cycles = DWT->CYCCNT - g_lvgl_refresh_start_cycles;
        g_lvgl_refresh_cycles_total += cycles;
        g_lvgl_refresh_count++;
        if (cycles > g_lvgl_refresh_max_cycles)
        {
            g_lvgl_refresh_max_cycles = cycles;
        }
    }
}

void lvgl_app_signal_update(const ra8p1_display_frame_t * frame)
{
    uint32_t center_index;
    if ((frame == NULL) ||
        (frame->magic != RA8P1_DISPLAY_STREAM_MAGIC) ||
        (frame->version != RA8P1_DISPLAY_STREAM_VERSION) ||
        (frame->size != sizeof(*frame)) ||
        ((frame->session_id == g_live_signal_frame.session_id) &&
         ((int32_t) (frame->sequence - g_live_signal_frame.sequence) <= 0)))
    {
        return;
    }
    center_index = ui_frame_center_index(frame);
    if (center_index < RA8P1_CENTER_COUNT)
    {
        if (((g_center_valid_mask & (1UL << center_index)) == 0U) ||
            (g_center_frames[center_index].session_id != frame->session_id) ||
            ((int32_t)(frame->sequence - g_center_frames[center_index].sequence) > 0))
        {
            g_center_frames[center_index] = *frame;
            g_center_valid_mask |= (1UL << center_index);
            g_spectrum_dirty_mask |= (1UL << center_index);
        }
    }
    if (!g_result_rate_valid)
    {
        g_result_rate_valid = true;
        g_result_rate_publish_tick = frame->publish_tick;
        g_result_rate_frame_count = 0U;
        g_result_rate_inference_count = frame->analysis.npu_inference_count;
    }
    else
    {
        const uint32_t elapsed_ms =
            frame->publish_tick - g_result_rate_publish_tick;
        const uint32_t inferences =
            frame->analysis.npu_inference_count -
            g_result_rate_inference_count;
        if ((elapsed_ms < 0x80000000UL) &&
            (inferences < 0x80000000UL))
        {
            g_result_rate_frame_count++;
            if (elapsed_ms >= 250U)
            {
                g_window_rate_x100 = (uint32_t)(
                    (((uint64_t)g_result_rate_frame_count * 100000U) +
                     (elapsed_ms / 2U)) / elapsed_ms);
                g_inference_rate_x100 = (uint32_t)(
                    (((uint64_t)inferences * 100000U) +
                     (elapsed_ms / 2U)) / elapsed_ms);
                g_result_rate_publish_tick = frame->publish_tick;
                g_result_rate_frame_count = 0U;
                g_result_rate_inference_count =
                    frame->analysis.npu_inference_count;
            }
        }
        else
        {
            g_result_rate_publish_tick = frame->publish_tick;
            g_result_rate_frame_count = 0U;
            g_result_rate_inference_count =
                frame->analysis.npu_inference_count;
            g_window_rate_x100 = 0U;
            g_inference_rate_x100 = 0U;
        }
    }
    g_live_signal_frame = *frame;
    g_live_signal_valid = (frame->channel_mask != 0U);
    g_visibility_frame_pending = g_live_signal_valid;
    g_visibility_flush_armed = false;
    g_visibility_vsync_pending = false;
    g_visibility_frame_presented = false;
    g_visibility_session_id = frame->session_id;
    g_visibility_sequence = frame->sequence;
    g_visibility_window_sequence = frame->analysis.window_sequence;
    g_spectrum_content_dirty = (g_spectrum_dirty_mask != 0U);
    g_mask_dirty = true;
    if (UI_SINGLE_FLOW_ENABLED)
    {
        uint32_t visibility_center = center_index;
        const bool placeholder =
            ((ui_cached_model_flags() & RA8P1_MODEL_FLAG_PLACEHOLDER) != 0U) ||
            ((frame->flags & RA8P1_DISPLAY_FLAG_MODEL_PLACEHOLDER) != 0U);

        if (center_index < RF_UI_CHANNEL_COUNT)
        {
            /* Keep the large reference-UI views on the operator-selected
             * channel.  The CPU0 scan rotates through four centers much more
             * quickly than a human can follow; switching the full spectrum
             * and waterfall image on every result makes the panel appear to
             * flash even though GLCDC is not underflowing.  The call below
             * only caches every center's newest real spectrum, while
             * rf_ui_mark_channel_result() keeps the four selector cards live.
             * The selected spectrum is coalesced in lvgl_app_step(). */
            (void) rf_ui_update_spectrum_window(
                center_index,
                &frame->spectrum[0][0],
                RA8P1_DISPLAY_SPECTRUM_BINS,
                frame->session_id,
                frame->analysis.window_sequence);
            ui_flow_update_channel_metrics(center_index, frame);
        }
        else
        {
            visibility_center = rf_ui_get_selected_channel();
        }

        rf_ui_set_model_placeholder(placeholder);
        ui_flow_update_detections();
        ui_flow_prepare_rf_boxes(frame);
        rf_ui_set_scan_rate_x10(ui_flow_scan_rate_x10());
        rf_ui_mark_channel_result(visibility_center,
                                  frame->analysis.window_sequence,
                                  ui_flow_frame_confidence(frame));
        if (center_index != rf_ui_get_selected_channel())
        {
            /* SCAN results for other centers still change a real selector
             * pulse/alert.  Arm that small invalidation immediately instead
             * of waiting for a selected-channel spectrum rasterization. */
            ui_visibility_content_prepared();
        }
        g_live_text_dirty = false;
    }
    else
    {
        /* Keep frame ingestion cheap; labels are coalesced by the data timer so
         * high-rate analysis results do not invalidate the whole header on every
         * mailbox transaction. */
        g_live_text_dirty = true;
    }
}

void lvgl_app_signal_reset(void)
{
    memset(&g_live_signal_frame, 0, sizeof(g_live_signal_frame));
    g_live_signal_valid = false;
    g_spectrum_content_dirty = false;
    g_spectrum_dirty_mask = 0U;
    g_spectrum_present_valid = false;
    g_mask_dirty = true;
    g_live_text_dirty = true;
    g_waterfall_rendered_session = 0U;
    g_waterfall_rendered_sequence = 0U;
    ui_tile_queue_reset();
    g_tile_last_received_session = 0U;
    g_tile_last_received_sequence = 0U;
    g_tile_session_center_mask = 0U;
    g_tile_center_valid_mask = 0U;
    memset(g_tile_center_last_sequence, 0, sizeof(g_tile_center_last_sequence));
    ui_tile_logical_history_reset();
    g_result_rate_valid = false;
    g_result_rate_publish_tick = 0U;
    g_result_rate_frame_count = 0U;
    g_result_rate_inference_count = 0U;
    g_window_rate_x100 = 0U;
    g_inference_rate_x100 = 0U;
    ui_visibility_reset();
    if (UI_SINGLE_FLOW_ENABLED)
    {
        static const uint8_t empty_spectrum[RF_UI_SPECTRUM_BINS] = {0U};
        const rf_ui_channel_metrics_t empty_metrics =
        {
            .peak_dbfs = -120,
            .noise_floor_dbfs = -120,
            .occupancy_percent = 0U,
            .age_ms = 0U
        };
        for (uint32_t center = 0U; center < RF_UI_CHANNEL_COUNT; ++center)
        {
            (void) rf_ui_update_spectrum(center,
                                         empty_spectrum,
                                         RF_UI_SPECTRUM_BINS);
            (void) rf_ui_update_channel_metrics(center, &empty_metrics);
        }
        rf_ui_reset_rf_box_fusion();
        for (uint32_t class_index = 0U;
             class_index < RF_UI_DETECTION_COUNT;
             ++class_index)
        {
            const rf_ui_detection_t detection =
            {
                .state = RF_UI_DETECTION_INACTIVE,
                .confidence_percent = 0U,
                .channel_index = (uint8_t)(class_index % RF_UI_CHANNEL_COUNT)
            };
            (void) rf_ui_update_detection(class_index, &detection);
        }
        rf_ui_set_model_placeholder(true);
        rf_ui_set_scan_rate_x10(0U);
    }
    else
    {
        ui_hide_waterfall_boxes();
    }
}

bool lvgl_app_frame_presented(const ra8p1_display_frame_t *frame)
{
    return (g_lvgl_display != NULL) &&
           g_visibility_frame_presented &&
           ui_frame_identity_matches(frame,
                                     g_visibility_presented_session_id,
                                     g_visibility_presented_sequence,
                                     g_visibility_presented_window_sequence);
}

void lvgl_app_tile_update(const ra8p1_display_tile_payload_t *tile)
{
    uint32_t tick_ms;
    uint32_t gap_columns = 0U;
    uint32_t unknown_history_mask = 0U;
    uint32_t current_center_mask;
    bool reset_unknown_history = false;
    if ((tile == NULL) ||
        (tile->magic != RA8P1_DISPLAY_TILE_MAGIC) ||
        (tile->version != RA8P1_DISPLAY_TILE_VERSION) ||
        (tile->size != sizeof(*tile)) ||
        (tile->center_index >= RA8P1_CENTER_COUNT) ||
        (tile->novel_time_start >= RA8P1_DISPLAY_TILE_HEIGHT) ||
        (tile->novel_time_count != 1U) ||
        (tile->width_height != ((RA8P1_DISPLAY_TILE_WIDTH << 16U) |
                                RA8P1_DISPLAY_TILE_HEIGHT)))
    {
        return;
    }
    current_center_mask = 1UL << tile->center_index;
    g_tile_center_valid_mask |= (1UL << tile->center_index);
    g_tile_center_last_sequence[tile->center_index] = tile->sequence;
    tick_ms = g_lvgl_tick_ms;
    if (!g_tile_rate_valid)
    {
        g_tile_rate_valid = true;
        g_tile_rate_tick_ms = tick_ms;
        g_tile_rate_count = 0U;
    }
    else
    {
        const uint32_t elapsed_ms = tick_ms - g_tile_rate_tick_ms;
        if (elapsed_ms < 0x80000000UL)
        {
            g_tile_rate_count++;
            if (elapsed_ms >= 250U)
            {
                g_tile_rate_x100 = (uint32_t)(
                    (((uint64_t)g_tile_rate_count * 100000U) +
                     (elapsed_ms / 2U)) / elapsed_ms);
                g_tile_rate_tick_ms = tick_ms;
                g_tile_rate_count = 0U;
                rf_ui_set_global_tile_rate_millihz(g_tile_rate_x100 * 10U);
            }
        }
        else
        {
            g_tile_rate_tick_ms = tick_ms;
            g_tile_rate_count = 0U;
            g_tile_rate_x100 = 0U;
            rf_ui_set_global_tile_rate_millihz(0U);
        }
    }
    if (g_tile_last_received_session != tile->session_id)
    {
        g_tile_last_received_session = tile->session_id;
        g_tile_session_center_mask = current_center_mask;
        if (((tile->sequence & 1U) == 0U) &&
            (tile->sequence >= 2U) &&
            (tile->sequence < 0x80000000UL))
        {
            gap_columns = (tile->sequence >> 1U) - 1U;
            g_ui_waterfall_tiles_dropped += gap_columns;
        }
        g_tile_last_received_sequence = tile->sequence;
    }
    else
    {
        /* A planned four-frequency scan legitimately changes center inside
         * one transport session. Keep every center's retained history unless
         * a real sequence gap or source discontinuity is observed below. */
        g_tile_session_center_mask |= current_center_mask;
        const uint32_t sequence_delta = tile->sequence -
                                        g_tile_last_received_sequence;
        if ((sequence_delta != 0U) &&
            (sequence_delta < 0x80000000UL) &&
            ((sequence_delta & 1U) == 0U))
        {
            const uint32_t published_delta = sequence_delta >> 1U;
            if (published_delta > 1U)
            {
                gap_columns = published_delta - 1U;
                g_ui_waterfall_tiles_dropped += gap_columns;
            }
            g_tile_last_received_sequence = tile->sequence;
        }
    }
    if ((gap_columns != 0U) &&
        ((g_tile_session_center_mask & (g_tile_session_center_mask - 1U)) != 0U))
    {
        unknown_history_mask = g_tile_session_center_mask;
    }
    reset_unknown_history =
        ui_tile_history_reset_required(tile, gap_columns);
    if (UI_SINGLE_FLOW_ENABLED)
    {
        if (reset_unknown_history)
        {
            /* A source discontinuity or retry makes prior RF duration
             * unavailable. Clear the complete retained history before
             * appending the first real row from the new logical run below. */
            unknown_history_mask |= current_center_mask;
        }
        if (unknown_history_mask != 0U)
        {
            for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
            {
                if ((unknown_history_mask & (1UL << center)) != 0U)
                {
                    (void) rf_ui_append_waterfall_gap_columns(
                        center,
                        RF_UI_WATERFALL_HISTORY_COLS);
                }
            }
        }
        else if (gap_columns != 0U)
        {
            /* Preserve RF time even when shared-ring slots were overwritten.
             * The UI API caps physical writes to one retained history. */
            (void) rf_ui_append_waterfall_gap_columns(tile->center_index,
                                                       gap_columns);
        }
        const bool appended = ui_flow_append_waterfall_tile(tile);
        if (appended &&
            (tile->novel_time_start ==
             (RA8P1_DISPLAY_TILE_HEIGHT - 1U)))
        {
            (void)rf_ui_note_complete_window(
                tile->center_index,
                tile->session_id,
                tile->window_sequence,
                tile->sequence);
        }
        /* Publish the exact completed-window waterfall anchor before a frame
         * box batch is released. This keeps delayed boxes attached to their
         * own retained columns during pause/review. */
        ui_flow_commit_rf_boxes_for_tile(tile);
    }
    else
    {
        ui_tile_queue_push(tile);
    }
}

void lvgl_app_telemetry_update(const ra8p1_system_telemetry_t * telemetry)
{
    if (telemetry != NULL)
    {
        const bool waiting_status_changed = !g_live_telemetry_valid ||
            (telemetry->pipeline_state != g_live_telemetry.pipeline_state) ||
            (telemetry->iq_payload_mbps_x1000 != g_live_telemetry.iq_payload_mbps_x1000) ||
            (telemetry->ring_high_watermark != g_live_telemetry.ring_high_watermark) ||
            (telemetry->ingress_drops != g_live_telemetry.ingress_drops) ||
            (telemetry->command_sequence != g_live_telemetry.command_sequence) ||
            (telemetry->command_status != g_live_telemetry.command_status) ||
            (telemetry->model_flags != g_live_telemetry.model_flags);
        g_live_telemetry = *telemetry;
        g_live_telemetry_valid = true;
        if (UI_SINGLE_FLOW_ENABLED)
        {
            const bool placeholder =
                ((ui_cached_model_flags() & RA8P1_MODEL_FLAG_PLACEHOLDER) != 0U) ||
                (g_live_signal_valid &&
                 ((g_live_signal_frame.flags &
                   RA8P1_DISPLAY_FLAG_MODEL_PLACEHOLDER) != 0U));
            rf_ui_set_model_placeholder(placeholder);
        }
        /* Once display frames are live, their 10 ms-window results own the
         * labels. Periodic telemetry must not force redundant 10 Hz redraws. */
        if (!g_live_signal_valid && waiting_status_changed)
        {
            g_live_text_dirty = true;
        }
    }
}

void lvgl_app_wifi_status_update(
    const ra8p1_wifi_status_mailbox_t *wifi_status)
{
    if (wifi_status == NULL)
    {
        return;
    }
    rf_ui_set_wifi_status(
        wifi_status->connection_state == RA8P1_WIFI_CONNECTED,
        wifi_status->connection_state == RA8P1_WIFI_CONNECTING,
        wifi_status->ssid);
}

static void lvgl_wait_for_next_line_event(uint32_t line_event)
{
    while (g_display_diag.glcdc_line_events == line_event)
    {
        __WFE();
    }
}

static void lvgl_flush_callback(lv_display_t * display, const lv_area_t * area, uint8_t * pixel_map)
{
    if (0U == g_display_diag.running)
    {
        /* Compose the initial frame while GLCDC is stopped.  The pixels remain
         * in framebuffer 0; the video start gate below makes them visible only
         * after the complete LVGL refresh has finished. */
        FSP_PARAMETER_NOT_USED(area);
        FSP_PARAMETER_NOT_USED(pixel_map);
        if (lv_display_flush_is_last(display))
        {
            g_display_diag.startup_initial_frame_ready = 1U;
        }
        lv_display_flush_ready(display);
        return;
    }

    if (lv_display_deferred_is_active(display) &&
        !lv_display_deferred_is_commit(display))
    {
        const uint32_t bytes = (uint32_t)lv_area_get_size(area) *
                               sizeof(uint16_t);
        g_display_diag.lvgl_deferred_flushes++;
        if (bytes > g_display_diag.lvgl_deferred_max_area_bytes)
        {
            g_display_diag.lvgl_deferred_max_area_bytes = bytes;
        }
        /* The chunk is complete in the off-screen direct buffer.  Mark the
         * LVGL flush ready without handing that buffer to GLCDC. */
        FSP_PARAMETER_NOT_USED(pixel_map);
        lv_display_flush_ready(display);
        return;
    }

    FSP_PARAMETER_NOT_USED(area);
    if (!lv_display_flush_is_last(display))
    {
        return;
    }

    g_flush_pending = false;
    g_flush_content_pending = false;
    fsp_err_t err = FSP_ERR_INVALID_UPDATE_TIMING;

    for (uint32_t retry = 0U; retry <= UI_GLCDC_SUBMIT_RETRY_LIMIT; ++retry)
    {
        uint32_t submit_line_event;
        const uint32_t primask = __get_PRIMASK();

        __DSB();
        __disable_irq();
        /* Capture the event before BufferChange.  Recording it afterwards can
         * miss the VSync which accepted this buffer and make LVGL wait an
         * extra frame before it may reuse the direct render buffer. */
        submit_line_event = g_display_diag.glcdc_line_events;
        err = R_GLCDC_BufferChange(&g_display_ctrl, pixel_map, DISPLAY_FRAME_LAYER_1);
        if (FSP_SUCCESS == err)
        {
            if (lv_display_deferred_is_commit(display))
            {
                g_display_diag.lvgl_deferred_commits++;
            }
            g_flush_line_event = submit_line_event;
            g_flush_pending = true;
            if (g_visibility_flush_armed)
            {
                g_visibility_vsync_session_id = g_visibility_session_id;
                g_visibility_vsync_sequence = g_visibility_sequence;
                g_visibility_vsync_window_sequence =
                    g_visibility_window_sequence;
                g_visibility_vsync_pending = true;
                g_visibility_flush_armed = false;
            }
            if (g_content_measurement_enabled && g_content_generation_pending)
            {
                g_flush_content_pending = true;
                g_content_generation_pending = false;
            }
            g_display_diag.animation_frames++;
            g_display_diag.animation_buffer_changes++;
            g_display_diag.animation_last_line_event = submit_line_event;
            __DMB();
        }

        if (0U == primask)
        {
            __enable_irq();
        }

        if (FSP_SUCCESS == err)
        {
            return;
        }
        if (FSP_ERR_INVALID_UPDATE_TIMING != err)
        {
            break;
        }

        lvgl_wait_for_next_line_event(submit_line_event);
    }

    g_content_generation_pending = false;
    if (lv_display_deferred_is_active(display))
    {
        g_display_diag.lvgl_deferred_aborts++;
        lv_display_deferred_abort(display);
    }
    else
    {
        /* A normal failed submission still owns LVGL's flush token. */
        lv_display_flush_ready(display);
    }
    ui_visibility_retry_after_flush_failure();
    g_display_diag.animation_last_error = (uint32_t) err;
    g_display_diag.animation_buffer_errors++;
}

static void lvgl_flush_wait_callback(lv_display_t * display)
{
    if (lv_display_flush_is_last(display) && g_flush_pending)
    {
        const uint32_t wait_start_cycles = DWT->CYCCNT;
        display_underflow_context_enter(DISPLAY_UNDERFLOW_CONTEXT_FLUSH_WAIT);
        while (g_flush_line_event == g_display_diag.glcdc_line_events)
        {
            __WFE();
        }
        display_underflow_context_leave(DISPLAY_UNDERFLOW_CONTEXT_FLUSH_WAIT);

        if (0U != g_display_diag.fps_counter_enabled)
        {
            const uint32_t cycles = DWT->CYCCNT - wait_start_cycles;
            g_lvgl_flush_wait_cycles_total += cycles;
            g_lvgl_flush_wait_count++;
            if (cycles > g_lvgl_flush_wait_max_cycles)
            {
                g_lvgl_flush_wait_max_cycles = cycles;
            }
        }

        g_presented_frame_count++;
        if (g_flush_content_pending)
        {
            g_ui_content_frame_count++;
            g_flush_content_pending = false;
        }
        if (g_visibility_vsync_pending)
        {
            g_visibility_presented_session_id = g_visibility_vsync_session_id;
            g_visibility_presented_sequence = g_visibility_vsync_sequence;
            g_visibility_presented_window_sequence =
                g_visibility_vsync_window_sequence;
            g_visibility_vsync_pending = false;
            g_visibility_frame_presented = true;
        }
        g_flush_pending = false;
    }
}

static void ui_track_deferred_resync(void)
{
    const bool active = lv_display_deferred_is_resyncing(g_lvgl_display);
    if(active && !g_deferred_resync_observed) {
        g_display_diag.lvgl_deferred_resync_starts++;
    }
    else if(!active && g_deferred_resync_observed) {
        g_display_diag.lvgl_deferred_resync_completions++;
    }
    g_deferred_resync_observed = active;
}

static bool ui_deferred_resync_step(void)
{
    if(!lv_display_deferred_is_resyncing(g_lvgl_display)) return false;

    const uint32_t bytes = lv_display_deferred_resync_step(
        g_lvgl_display, UI_DEFERRED_RESYNC_MAX_BYTES);
    g_display_diag.lvgl_deferred_resync_last_bytes = bytes;
    if(bytes != 0U) {
        g_display_diag.lvgl_deferred_resync_chunks++;
        g_display_diag.lvgl_deferred_resync_total_bytes += bytes;
        if(bytes > g_display_diag.lvgl_deferred_resync_max_bytes) {
            g_display_diag.lvgl_deferred_resync_max_bytes = bytes;
        }
    }
    ui_track_deferred_resync();
    return true;
}

static uint32_t ui_lvgl_refresh_underflow_context(void)
{
    uint32_t context = DISPLAY_UNDERFLOW_CONTEXT_LVGL_REFRESH;
    if(lv_display_deferred_is_commit(g_lvgl_display)) {
        context |= DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_COMMIT;
    }
    else if(lv_display_deferred_is_active(g_lvgl_display)) {
        context |= DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_DRAW;
    }
    else {
        context |= DISPLAY_UNDERFLOW_CONTEXT_NORMAL_REFRESH;
    }
    return context;
}

static void lvgl_touch_read_callback(lv_indev_t * input, lv_indev_data_t * data)
{
    FSP_PARAMETER_NOT_USED(input);
    gt911_sample_t sample;
    const uint32_t poll_tick = g_lvgl_tick_ms;
    if (g_lvgl_app_input_diag.poll_calls != 0U)
    {
        const uint32_t interval = poll_tick -
                                  g_lvgl_app_input_diag.last_poll_tick_ms;
        if (interval > g_lvgl_app_input_diag.max_poll_interval_ms)
        {
            g_lvgl_app_input_diag.max_poll_interval_ms = interval;
        }
    }
    g_lvgl_app_input_diag.poll_calls++;
    g_lvgl_app_input_diag.last_poll_tick_ms = poll_tick;

    const fsp_err_t err = g_touch_available ?
                          gt911_touch_poll(&sample) : FSP_ERR_NOT_OPEN;
    g_lvgl_app_input_diag.last_error = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        g_lvgl_app_input_diag.poll_errors++;
    }
    else if (sample.updated)
    {
        const bool was_pressed = g_touch_pressed;
        g_lvgl_app_input_diag.sample_updates++;
        if (sample.count > 0U)
        {
            g_touch_pressed = true;
            g_touch_x = sample.points[0].x;
            g_touch_y = sample.points[0].y;
            g_lvgl_app_input_diag.pressed_samples++;
            if (!was_pressed)
            {
                g_lvgl_app_input_diag.press_transitions++;
                g_lvgl_app_input_diag.last_press_tick_ms = poll_tick;
            }
        }
        else
        {
            g_touch_pressed = false;
            if (was_pressed)
            {
                g_lvgl_app_input_diag.release_transitions++;
                g_lvgl_app_input_diag.last_release_tick_ms = poll_tick;
            }
        }
    }
    g_lvgl_app_input_diag.last_state = g_touch_pressed ? 1U : 0U;
    g_lvgl_app_input_diag.last_x = g_touch_x;
    g_lvgl_app_input_diag.last_y = g_touch_y;
    data->point.x = (lv_coord_t) g_touch_x;
    data->point.y = (lv_coord_t) g_touch_y;
    data->state = g_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void lvgl_touch_poll_step(void)
{
    if (NULL == g_touch_input)
    {
        return;
    }

    const uint32_t now = g_lvgl_tick_ms;
    const uint32_t interval = now - g_touch_owner_poll_tick;
    if (interval < UI_TOUCH_POLL_PERIOD_MS)
    {
        return;
    }
    if (interval > (UI_TOUCH_POLL_PERIOD_MS * 2U))
    {
        g_lvgl_app_input_diag.late_polls++;
    }

    g_touch_owner_poll_tick = now;
    g_lvgl_app_input_diag.owner_reads++;
    lv_indev_read(g_touch_input);
}

static uint32_t lvgl_tick_get_callback(void)
{
    return g_lvgl_tick_ms;
}

void SysTick_Handler(void);

void SysTick_Handler(void)
{
    g_lvgl_tick_ms++;
}

fsp_err_t lvgl_app_init(bool touch_available)
{
    if ((0U == g_display_diag.running) &&
        !display_bringup_ready_for_first_frame())
    {
        return FSP_ERR_NOT_OPEN;
    }

    memset((void *) &g_lvgl_app_input_diag, 0,
           sizeof(g_lvgl_app_input_diag));
    g_lvgl_app_input_diag.magic = LVGL_APP_INPUT_DIAG_MAGIC;
    g_lvgl_app_input_diag.version = LVGL_APP_INPUT_DIAG_VERSION;
    g_lvgl_app_input_diag.poll_period_ms = UI_TOUCH_POLL_PERIOD_MS;
    g_touch_available = touch_available;
    g_touch_input = NULL;
    g_touch_pressed = false;
    g_touch_x = 0U;
    g_touch_y = 0U;
    g_touch_owner_poll_tick = 0U;
    g_waterfall_overlay_enabled = false;
    g_waterfall_overlay_pending = false;
    g_waterfall_overlay_line_event = 0U;
    g_flush_pending = false;
    g_flush_content_pending = false;
    g_sdram_work_line_event = g_display_diag.glcdc_line_events;
    g_deferred_resync_observed = false;
    g_content_generation_pending = false;
    g_content_measurement_enabled = false;
    g_presented_frame_count = 0U;
    g_ui_content_frame_count = 0U;
    g_lvgl_tick_ms = 0U;
    g_active_page = UI_PAGE_MONITOR;
    g_waterfall_running = true;
    g_live_text_dirty = false;
    g_waterfall_rendered_session = 0U;
    g_waterfall_rendered_sequence = 0U;
    ui_tile_queue_reset();
    g_tile_last_received_session = 0U;
    g_tile_last_received_sequence = 0U;
    g_tile_session_center_mask = 0U;
    g_tile_center_valid_mask = 0U;
    memset(g_tile_center_last_sequence, 0, sizeof(g_tile_center_last_sequence));
    ui_tile_logical_history_reset();
    g_ui_waterfall_columns_generated = 0U;
    g_ui_waterfall_tiles_consumed = 0U;
    g_ui_waterfall_tiles_dropped = 0U;
    g_result_rate_valid = false;
    g_result_rate_publish_tick = 0U;
    g_result_rate_frame_count = 0U;
    g_result_rate_inference_count = 0U;
    g_window_rate_x100 = 0U;
    g_inference_rate_x100 = 0U;
    g_tile_rate_valid = false;
    g_tile_rate_tick_ms = 0U;
    g_tile_rate_count = 0U;
    g_tile_rate_x100 = 0U;
    g_spectrum_content_dirty = false;
    g_spectrum_dirty_mask = (1UL << UI_CHANNEL_COUNT) - 1U;
    g_last_spectrum_update_tick = 0U;
    g_spectrum_present_valid = false;
    g_last_waterfall_update_tick = 0U;
    g_waterfall_present_valid = false;
    g_waterfall_visual_dirty = false;
    g_spectrum_rendered_mask = 0U;
    g_signal_phase = 0U;
    g_mask_dirty = false;
    g_last_mask_update_tick = 0U;
    g_last_text_update_tick = 0U;
    g_fps_last_underflow_count = g_display_diag.glcdc_underflows;
    g_presented_fps_millihz = 0U;
    g_content_fps_millihz = 0U;
    g_underflow_rate_millihz = 0U;
    memset(&g_live_signal_frame, 0, sizeof(g_live_signal_frame));
    memset(g_center_frames, 0, sizeof(g_center_frames));
    g_center_valid_mask = 0U;
    g_live_signal_valid = false;
    ui_visibility_reset();
    memset(&g_live_telemetry, 0, sizeof(g_live_telemetry));
    g_live_telemetry_valid = false;

    lv_init();
    if (0U == SysTick_Config(SystemCoreClock / 1000U))
    {
        lv_tick_set_cb(lvgl_tick_get_callback);
    }
    else
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    g_lvgl_display = lv_display_create(UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    if (NULL == g_lvgl_display)
    {
        return FSP_ERR_OUT_OF_MEMORY;
    }
    lv_timer_t * const display_refresh_timer = lv_display_get_refr_timer(g_lvgl_display);
    if (NULL == display_refresh_timer)
    {
        return FSP_ERR_NOT_FOUND;
    }
    lv_timer_set_period(display_refresh_timer, UI_DISPLAY_REFRESH_PERIOD_MS);
    lv_display_set_color_format(g_lvgl_display, LV_COLOR_FORMAT_RGB565);
    lv_display_add_event_cb(g_lvgl_display, lvgl_display_event_callback, LV_EVENT_ALL, NULL);
    lv_display_set_flush_cb(g_lvgl_display, lvgl_flush_callback);
    lv_display_set_flush_wait_cb(g_lvgl_display, lvgl_flush_wait_callback);
    lv_display_set_buffers_with_stride(g_lvgl_display,
                                       /* Compose startup with one buffer so no
                                        * full-screen peer-buffer sync can occur
                                        * before the panel has a valid frame. */
                                       &fb_background[0][0],
                                       NULL,
                                       DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * DISPLAY_VSIZE_INPUT0,
                                       DISPLAY_BUFFER_STRIDE_BYTES_INPUT0,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);

    if (g_touch_available)
    {
        lv_indev_t * touch = lv_indev_create();
        if (NULL == touch)
        {
            return FSP_ERR_OUT_OF_MEMORY;
        }
        lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch, lvgl_touch_read_callback);
        lv_timer_t * const touch_read_timer = lv_indev_get_read_timer(touch);
        if (NULL == touch_read_timer)
        {
            return FSP_ERR_NOT_FOUND;
        }
        /* Input is read explicitly at the start of the LVGL owner step.  This
         * keeps a touch sample ahead of bounded SDRAM/display work without
         * calling LVGL from the GT911 or VSync interrupt context. */
        lv_timer_pause(touch_read_timer);
        g_touch_input = touch;
    }

    if (UI_SINGLE_FLOW_ENABLED)
    {
        rf_ui_create();
        rf_ui_set_external_spectrum_mode(true);
        rf_ui_set_model_placeholder(true);
        rf_ui_set_scan_rate_x10(0U);
        rf_ui_set_render_metrics(
            (g_display_diag.measured_refresh_millihz != 0U) ?
            g_display_diag.measured_refresh_millihz : g_display_diag.refresh_millihz,
            0U,
            0U,
            g_display_diag.glcdc_underflows);
        ui_rf_waterfall_overlay_init();
    }
    else
    {
        ui_init_heat_color_lut();
        ui_init_waterfalls();
        ui_update_spectra();
        ui_create();
        ui_update_spectra();
        ui_draw_mask_preview();
        ui_update_live_text();
    }
    g_fps_last_tick_ms = g_lvgl_tick_ms;
    g_fps_last_frame_count = g_presented_frame_count;

    if (!UI_SINGLE_FLOW_ENABLED)
    {
        g_data_timer = lv_timer_create(ui_data_timer_callback, UI_DATA_PERIOD_MS, NULL);
        if (NULL == g_data_timer)
        {
            return FSP_ERR_OUT_OF_MEMORY;
        }
        lv_timer_ready(g_data_timer);
    }
    lv_timer_ready(display_refresh_timer);
    const uint32_t initial_refresh_context =
        ui_lvgl_refresh_underflow_context();
    display_underflow_context_enter(initial_refresh_context);
    (void) lv_timer_handler();
    display_underflow_context_leave(initial_refresh_context);
    if (0U == g_display_diag.startup_initial_frame_ready)
    {
        return FSP_ERR_NOT_INITIALIZED;
    }

    const size_t framebuffer_bytes =
        (size_t)DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * DISPLAY_VSIZE_INPUT0;
    memcpy(&fb_background[1][0], &fb_background[0][0], framebuffer_bytes);
    __DSB();
    /* Both buffers contain the same complete frame.  Restore direct double
     * buffering before starting scanout so subsequent frames stay tear-free. */
    lv_display_set_buffers_with_stride(g_lvgl_display,
                                       &fb_background[1][0],
                                       &fb_background[0][0],
                                       DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * DISPLAY_VSIZE_INPUT0,
                                       DISPLAY_BUFFER_STRIDE_BYTES_INPUT0,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
    const fsp_err_t video_error = display_video_start();
    if (FSP_SUCCESS != video_error)
    {
        return video_error;
    }
    /* Ignore the initial composition.  From this point onward the counter
     * measures only a real spectrum or waterfall canvas that reaches the
     * panel through a successful final flush. */
    g_content_last_tick_ms = g_lvgl_tick_ms;
    g_content_last_frame_count = 0U;
    g_ui_content_frame_count = 0U;
    g_content_generation_pending = false;
    g_flush_content_pending = false;
    g_content_measurement_enabled = true;
    if (NULL != g_touch_input)
    {
        g_touch_owner_poll_tick = g_lvgl_tick_ms - UI_TOUCH_POLL_PERIOD_MS;
    }
    return FSP_SUCCESS;
}

void lvgl_app_step(uint32_t elapsed_ms)
{
    FSP_PARAMETER_NOT_USED(elapsed_ms);

    lvgl_touch_poll_step();

    if (UI_SINGLE_FLOW_ENABLED)
    {
        bool work_slot_used = false;
        bool resync_completed_this_step = false;
        const uint32_t line_event = g_display_diag.glcdc_line_events;
        const bool have_work_slot = line_event != g_sdram_work_line_event;
        rf_ui_runtime_monitor_step();
        ui_track_deferred_resync();

        if (have_work_slot)
        {
            g_sdram_work_line_event = line_event;
            g_display_diag.lvgl_sdram_work_slots++;
            const bool resync_was_active =
                lv_display_deferred_is_resyncing(g_lvgl_display);
            display_underflow_context_enter(
                DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_RESYNC);
            work_slot_used = ui_deferred_resync_step();
            display_underflow_context_leave(
                DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_RESYNC);
            resync_completed_this_step = resync_was_active &&
                !lv_display_deferred_is_resyncing(g_lvgl_display);
            if (resync_completed_this_step)
            {
                g_display_diag.lvgl_refresh_skips_after_resync++;
            }
        }

        if (have_work_slot && !work_slot_used &&
            !rf_ui_channel_switch_busy())
        {
            display_underflow_context_enter(
                DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT);
            work_slot_used = rf_ui_box_fusion_step();
            display_underflow_context_leave(
                DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT);
        }

        bool channel_switch_committed = false;
        if (have_work_slot && !work_slot_used)
        {
            const bool busy_before = rf_ui_channel_switch_busy();
            const bool cycle_budget_available =
                (g_display_diag.fps_counter_enabled != 0U) &&
                (SystemCoreClock != 0U) &&
                ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
                ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
            const uint32_t switch_start_cycles = DWT->CYCCNT;
            const uint32_t switch_budget_cycles = (uint32_t)(
                ((uint64_t)SystemCoreClock * UI_SDRAM_WORK_BUDGET_US) /
                1000000U);
            const uint32_t switch_guard_cycles = (uint32_t)(
                ((uint64_t)SystemCoreClock * UI_SDRAM_WORK_GUARD_US) /
                1000000U);
            uint32_t switch_steps = 0U;
            display_underflow_context_enter(
                DISPLAY_UNDERFLOW_CONTEXT_CHANNEL_SWITCH);
            do
            {
                channel_switch_committed =
                    rf_ui_channel_switch_step() ||
                    channel_switch_committed;
                switch_steps++;
                if (channel_switch_committed ||
                    !rf_ui_channel_switch_busy() ||
                    !cycle_budget_available)
                {
                    break;
                }
                const uint32_t elapsed_cycles =
                    DWT->CYCCNT - switch_start_cycles;
                if (((uint64_t)elapsed_cycles + switch_guard_cycles) >=
                    switch_budget_cycles)
                {
                    break;
                }
            }
            while (switch_steps <
                   UI_CHANNEL_SWITCH_MAX_STEPS_PER_VSYNC);
            display_underflow_context_leave(
                DISPLAY_UNDERFLOW_CONTEXT_CHANNEL_SWITCH);
            work_slot_used = busy_before || channel_switch_committed ||
                             rf_ui_channel_switch_busy();
        }
        if (channel_switch_committed)
        {
            /* Both image descriptors, channel labels, RF boxes and selector
             * state were changed in one LVGL-owner transaction. The existing
             * final flush still hands the framebuffer to GLCDC at VSync. */
            g_spectrum_present_valid = true;
            g_waterfall_present_valid = true;
            g_last_spectrum_update_tick = g_lvgl_tick_ms;
            g_last_waterfall_update_tick = g_lvgl_tick_ms;
            if (g_content_measurement_enabled)
            {
                g_content_generation_pending = true;
            }
            if (g_live_signal_valid &&
                (ui_frame_center_index(&g_live_signal_frame) ==
                 rf_ui_get_selected_channel()))
            {
                ui_visibility_content_prepared();
            }
        }
        /* Layer 2 is driven only from this owner thread. A channel-switch
         * frame is submitted here before LVGL's matching final flush, so both
         * GLCDC layers latch at the same VSync. */
        ui_rf_waterfall_overlay_step(
            channel_switch_committed || !rf_ui_channel_switch_busy());
        if (have_work_slot && !work_slot_used &&
            (!g_spectrum_present_valid ||
             ((g_lvgl_tick_ms - g_last_spectrum_update_tick) >=
               UI_SPECTRUM_UPDATE_PERIOD_MS)))
        {
            const uint32_t spectrum_start_cycles = DWT->CYCCNT;
            display_underflow_context_enter(
                DISPLAY_UNDERFLOW_CONTEXT_SPECTRUM_PRESENT);
            if (rf_ui_present_spectrum())
            {
                const uint32_t spectrum_cycles =
                    DWT->CYCCNT - spectrum_start_cycles;
                g_ui_spectrum_gen_cycles += spectrum_cycles;
                if (spectrum_cycles > g_ui_spectrum_gen_max_cycles)
                {
                    g_ui_spectrum_gen_max_cycles = spectrum_cycles;
                }
                g_ui_spectrum_redraws++;
                work_slot_used = true;
                g_spectrum_present_valid = true;
                g_last_spectrum_update_tick = g_lvgl_tick_ms;
                if (g_content_measurement_enabled)
                {
                    g_content_generation_pending = true;
                }

                /* Only associate the flush with a CPU0 result when the
                 * rasterized source is that result's selected center. */
                if (ui_frame_center_index(&g_live_signal_frame) ==
                    rf_ui_get_selected_channel())
                {
                    ui_visibility_content_prepared();
                }
            }
            display_underflow_context_leave(
                DISPLAY_UNDERFLOW_CONTEXT_SPECTRUM_PRESENT);
        }
        if (have_work_slot && !work_slot_used &&
            (!g_waterfall_present_valid ||
             ((g_lvgl_tick_ms - g_last_waterfall_update_tick) >=
              UI_WATERFALL_PRESENT_PERIOD_MS) ||
             rf_ui_waterfall_build_busy()))
        {
            const bool busy_before = rf_ui_waterfall_build_busy();
            display_underflow_context_enter(
                DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT);
            const bool presented = rf_ui_present_waterfall();
            display_underflow_context_leave(
                DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT);
            work_slot_used = busy_before || presented ||
                             rf_ui_waterfall_build_busy();
            if (presented)
            {
                g_waterfall_present_valid = true;
                g_last_waterfall_update_tick = g_lvgl_tick_ms;
                if (g_content_measurement_enabled)
                {
                    g_content_generation_pending = true;
                }
            }
        }
        if ((g_lvgl_tick_ms - g_fps_last_tick_ms) >= 1000U)
        {
            ui_fps_update(NULL);
        }
        /* Catch a touch sample that became due during bounded SDRAM work.
         * The period guard makes this a no-op on normal short owner steps. */
        lvgl_touch_poll_step();
        if (!resync_completed_this_step)
        {
            const uint32_t refresh_context =
                ui_lvgl_refresh_underflow_context();
            display_underflow_context_enter(refresh_context);
            (void) lv_timer_handler();
            display_underflow_context_leave(refresh_context);
        }
        ui_track_deferred_resync();
        return;
    }

    ui_poll_glcdc_layer2_underflow();

    /* Commit all data-driven invalidations before LVGL's refresh timer runs.
     * Keeping one analysis window in one refresh prevents a spectrum, its
     * labels, and its waterfall block from becoming separate buffer swaps. */
    ui_update_waterfall_columns();
    if (g_active_page == UI_PAGE_RECOGNITION)
    {
        if ((g_mask_dirty || ui_visibility_render_required()) &&
            ((g_last_mask_update_tick == 0U) ||
             ((g_lvgl_tick_ms - g_last_mask_update_tick) >= UI_MASK_UPDATE_PERIOD_MS) ||
             ui_visibility_render_required()))
        {
            ui_draw_mask_preview();
            g_last_mask_update_tick = g_lvgl_tick_ms;
            g_mask_dirty = false;
            ui_visibility_content_prepared();
        }
    }
    else if ((g_spectrum_content_dirty || ui_visibility_render_required()) &&
             ((g_last_spectrum_update_tick == 0U) ||
              ((g_lvgl_tick_ms - g_last_spectrum_update_tick) >=
               UI_SPECTRUM_UPDATE_PERIOD_MS) ||
              ui_visibility_render_required()))
    {
        if (ui_visibility_render_required() &&
            (g_live_signal_frame.analysis.center_index < UI_CHANNEL_COUNT))
        {
            g_spectrum_dirty_mask |=
                (1UL << g_live_signal_frame.analysis.center_index);
        }
        ui_update_spectra();
        g_spectrum_content_dirty = (g_spectrum_dirty_mask != 0U);
        g_last_spectrum_update_tick = g_lvgl_tick_ms;
    }

    if (g_live_text_dirty &&
        ((g_lvgl_tick_ms - g_last_text_update_tick) >= UI_TEXT_UPDATE_PERIOD_MS))
    {
        ui_update_live_text();
        g_live_text_dirty = false;
        g_last_text_update_tick = g_lvgl_tick_ms;
    }

    /* With a GLCDC overlay, the waterfall content is presented independently
     * of LVGL.  Updating this label once per second is low-rate bookkeeping;
     * the content counter itself is advanced only after the overlay line
     * event. */
    if ((g_content_generation_pending ||
         (g_waterfall_overlay_enabled && (g_active_page == UI_PAGE_RECOGNITION))) &&
        ((g_lvgl_tick_ms - g_fps_last_tick_ms) >= 1000U))
    {
        ui_fps_update(NULL);
    }
    const uint32_t refresh_context = ui_lvgl_refresh_underflow_context();
    display_underflow_context_enter(refresh_context);
    (void) lv_timer_handler();
    display_underflow_context_leave(refresh_context);
}

void lvgl_app_runtime_metrics_get(lvgl_app_runtime_metrics_t *metrics)
{
    if (metrics == NULL)
    {
        return;
    }
    __DMB();
    metrics->tick_ms = g_lvgl_tick_ms;
    metrics->presented_frame_count = g_presented_frame_count;
    metrics->presented_fps_millihz = g_presented_fps_millihz;
    metrics->content_frame_count = g_ui_content_frame_count;
    metrics->content_fps_millihz = g_content_fps_millihz;
    metrics->glcdc_underflow_rate_millihz = g_underflow_rate_millihz;
    metrics->window_rate_millihz = g_window_rate_x100 * 10U;
    metrics->inference_rate_millihz = g_inference_rate_x100 * 10U;
    metrics->tile_rate_millihz = g_tile_rate_x100 * 10U;
    metrics->waterfall_columns_generated = g_ui_waterfall_columns_generated;
    metrics->waterfall_tiles_consumed = g_ui_waterfall_tiles_consumed;
    metrics->waterfall_tiles_dropped = g_ui_waterfall_tiles_dropped;
    __DMB();
}

uint32_t lvgl_app_center_valid_mask(void)
{
    return g_center_valid_mask;
}

bool lvgl_app_center_frame_get(uint32_t center_index, ra8p1_display_frame_t *frame)
{
    if ((frame == NULL) || (center_index >= RA8P1_CENTER_COUNT) ||
        ((g_center_valid_mask & (1UL << center_index)) == 0U))
    {
        return false;
    }
    *frame = g_center_frames[center_index];
    return true;
}
