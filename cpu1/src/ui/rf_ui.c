#include "rf_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal_data.h"
#include "lvgl.h"

#include "display_bringup.h"
#include "framework/display_app.h"
#include "rf_demo_data.h"
#include "rf_device_thumbnails.h"
#include "rf_ui_fonts.h"

#if RF_UI_CHANNEL_COUNT != RF_DEMO_CHANNEL_COUNT || \
    RF_UI_DETECTION_COUNT != RF_DEMO_CLASS_COUNT
#error "rf_ui channel and detection dimensions must match metadata"
#endif

#define RF_SCREEN_WIDTH 1024
#define RF_SCREEN_HEIGHT 600

#define RF_HEADER_HEIGHT 54
#define RF_TARGET_STRIP_Y 54
#define RF_TARGET_STRIP_HEIGHT 58
#define RF_TARGET_CARD_WIDTH 256
#define RF_TARGET_THUMB_WIDTH 52
#define RF_TARGET_THUMB_HEIGHT 46
#define RF_MODE_X 260
#define RF_MODE_Y 5
#define RF_MODE_WIDTH 91
#define RF_MODE_HEIGHT 44
#define RF_MODE_GAP 2
#define RF_TRANSPORT_X 452
#define RF_TRANSPORT_Y 5
#define RF_TRANSPORT_WIDTH 564
#define RF_TRANSPORT_HEIGHT 44
#define RF_LIVE_BUTTON_X 0
#define RF_LIVE_BUTTON_WIDTH 48
#define RF_HISTORY_OLDER_X 48
#define RF_HISTORY_BUTTON_WIDTH 36
#define RF_HISTORY_TIMELINE_X 84
#define RF_HISTORY_TIMELINE_WIDTH 392
#define RF_HISTORY_NEWER_X 476
#define RF_SOURCE_BADGE_X 512
#define RF_SOURCE_BADGE_WIDTH 52
#define RF_WATERFALL_Y 112
#define RF_WATERFALL_HEIGHT 312
#define RF_SPECTRUM_Y 424
#define RF_SPECTRUM_HEIGHT 104
#define RF_BOTTOM_Y 528
#define RF_BOTTOM_HEIGHT 48
#define RF_METRICS_Y 576
#define RF_METRICS_HEIGHT 24
#define RF_PANEL_X 0
#define RF_PANEL_WIDTH 864
#define RF_CHANNEL_DECK_X 0
#define RF_CHANNEL_DECK_WIDTH 864
#define RF_CHANNEL_CARD_WIDTH 216
#define RF_SIDEBAR_X 864
#define RF_SIDEBAR_Y 112
#define RF_SIDEBAR_WIDTH 160
#define RF_SIDEBAR_HEIGHT 464
#define RF_PLOT_X 64
#define RF_TOUCH_TARGET 44

#define RF_SPECTRUM_TEXTURE_WIDTH 400u
#define RF_SPECTRUM_TEXTURE_HEIGHT 40u
#define RF_SPECTRUM_TEXTURE_STRIDE_PIXELS 416u
#define RF_SPECTRUM_TEXTURE_STRIDE_BYTES \
    (RF_SPECTRUM_TEXTURE_STRIDE_PIXELS * sizeof(uint16_t))
#define RF_SPECTRUM_BASE_ROW_WRITE_BYTES \
    (RF_SPECTRUM_TEXTURE_STRIDE_BYTES + \
     (9u * sizeof(uint16_t)))
#define RF_SPECTRUM_HORIZONTAL_DIVIDER_WRITE_BYTES \
    (RF_SPECTRUM_TEXTURE_WIDTH * sizeof(uint16_t))
#define RF_SPECTRUM_MAX_BASE_CHUNK_DIVIDERS 4u
#define RF_SPECTRUM_MAX_TRACE_SEGMENT_WRITE_BYTES 282u
#define RF_SPECTRUM_PEAK_MAX_PIXELS 13u
#define RF_SPECTRUM_DISPLAY_WIDTH 800
#define RF_SPECTRUM_DISPLAY_HEIGHT 66

#define RF_WATERFALL_DISPLAY_WIDTH 800
#define RF_WATERFALL_DISPLAY_HEIGHT 252
#define RF_WATERFALL_RENDER_STORAGE_WIDTH \
    (RF_WATERFALL_DISPLAY_WIDTH * 2u)
#define RF_WATERFALL_RENDER_STRIDE_BYTES \
    (RF_WATERFALL_RENDER_STORAGE_WIDTH * sizeof(uint16_t))
#define RF_WATERFALL_VISIBLE_ROW_BYTES \
    (RF_WATERFALL_DISPLAY_WIDTH * sizeof(uint16_t))
#define RF_WATERFALL_CLUT_PIXELS_PER_COLUMN \
    (RF_WATERFALL_DISPLAY_WIDTH / RF_UI_WATERFALL_COLS)
#define RF_WATERFALL_CLUT_RING_WIDTH \
    (RF_UI_WATERFALL_HISTORY_COLS * RF_WATERFALL_CLUT_PIXELS_PER_COLUMN)
#define RF_WATERFALL_CLUT_STORAGE_WIDTH (RF_WATERFALL_CLUT_RING_WIDTH * 2u)
#define RF_WATERFALL_CLUT_RING_BYTES (RF_WATERFALL_CLUT_RING_WIDTH / 2u)
#define RF_WATERFALL_CLUT_STRIDE_BYTES (RF_WATERFALL_CLUT_STORAGE_WIDTH / 2u)
#define RF_WATERFALL_CLUT_ALIGNMENT_PIXELS \
    (64u * 8u / RF_UI_WATERFALL_OVERLAY_BITS_PER_PIXEL)
#define RF_WATERFALL_CLUT_PHASE_COUNT 2u
#define RF_WATERFALL_CLUT_PHASE_OFFSET_PIXELS 64u
#define RF_WATERFALL_CLUT_COLUMN_WRITE_BYTES \
    ((((RF_WATERFALL_CLUT_PIXELS_PER_COLUMN + 1u) / 2u)) * 2u * \
     RF_WATERFALL_CLUT_PHASE_COUNT)
#define RF_WATERFALL_CLUT_BUILD_ROW_WRITE_BYTES \
    ((RF_UI_WATERFALL_HISTORY_COLS * \
      ((RF_WATERFALL_CLUT_PIXELS_PER_COLUMN + 1u) / 2u) + \
      RF_WATERFALL_CLUT_RING_BYTES) * RF_WATERFALL_CLUT_PHASE_COUNT)
#define RF_WATERFALL_OVERLAY_Y (RF_WATERFALL_Y + 36u)
#define RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC 16u
#define RF_WATERFALL_OVERLAY_DEFAULT_RATE_X10 65u
#define RF_WATERFALL_OVERLAY_RATE_DENOMINATOR 10000u
#define RF_WATERFALL_OVERLAY_CATCHUP_TARGET_PIXELS 160u
#define RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK 11u
#define RF_WATERFALL_OVERLAY_CATCHUP_MAX_ROWS_PER_TICK 64u
#define RF_WATERFALL_CLUT_HEAT_FIRST 1u
#define RF_WATERFALL_CLUT_HEAT_LAST 9u
#define RF_WATERFALL_CLUT_BOX_FIRST 10u
#define RF_WATERFALL_CLUT_GAP_A 14u
#define RF_WATERFALL_CLUT_GAP_B 15u
#define RF_CHANNEL_HALF_BANDWIDTH_MHZ 28u
#define RF_WATERFALL_RF_WINDOW_SAMPLES 590336u
#define RF_WATERFALL_RF_SAMPLE_RATE_HZ 60000000u
#define RF_WATERFALL_RF_ROWS_PER_WINDOW 16u
#define RF_WATERFALL_FAST_FREQ_BINS 192u
#define RF_WATERFALL_PAN_PRESENT_PERIOD_MS 33u
#define RF_WATERFALL_HISTORY_STEP_COLS 24u
#define RF_WATERFALL_BOX_HISTORY_MAX_WRITE_BYTES \
    (((2u * RF_UI_WATERFALL_FREQ_BINS) + \
      (4u * RF_WATERFALL_RF_ROWS_PER_WINDOW)) * \
     2u * sizeof(uint16_t))
#define RF_WATERFALL_BOX_CLUT_MAX_WRITE_BYTES \
    (((4u * RF_WATERFALL_RF_ROWS_PER_WINDOW * \
       RF_WATERFALL_CLUT_PIXELS_PER_COLUMN) + \
      (4u * RF_WATERFALL_DISPLAY_HEIGHT)) * \
     RF_WATERFALL_CLUT_PHASE_COUNT * 2u)
#define RF_CHANNEL_SOURCE_COUNT 2u
#define RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK 10u
#define RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK 20u
#define RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK 32u
#define RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK 20u
#define RF_CHANNEL_SWITCH_SPECTRUM_SEGMENTS_PER_TICK 112u
#define RF_CHANNEL_SWITCH_METADATA_STAGE_COUNT 4u
#define RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES (32u * 1024u)
#define RF_CHANNEL_SWITCH_MAX_WRITE_BYTES (32u * 1024u)
#define RF_UI_PENDING_BOX_BATCH_CAPACITY (RF_UI_CHANNEL_COUNT * 2u)
#define RF_UI_FUSION_DECISION_CACHE_CAPACITY (RF_UI_CHANNEL_COUNT * 2u)
#define RF_UI_WINDOW_ANCHOR_CAPACITY (RF_UI_CHANNEL_COUNT * 2u)

#define RF_COLOR_SCREEN 0x050708u
#define RF_COLOR_HEADER 0x0B0F11u
#define RF_COLOR_PANEL 0x101518u
#define RF_COLOR_PANEL_ALT 0x151C20u
#define RF_COLOR_PLOT 0x020405u
#define RF_COLOR_BORDER 0x334047u
#define RF_COLOR_DIVIDER 0x252F34u
#define RF_COLOR_TEXT 0xF4F7F8u
#define RF_COLOR_MUTED 0xA8B6BCu
#define RF_COLOR_AXIS 0x91A2AAu
#define RF_COLOR_PRIMARY 0x36D6C0u
#define RF_COLOR_PRIMARY_SOFT 0x12332Eu
#define RF_COLOR_AMBER 0xF2B84Bu
#define RF_COLOR_ORANGE 0xFF9A57u
#define RF_COLOR_GREEN 0x79D3B2u
#define RF_COLOR_RED 0xFF6B5Fu
#define RF_COLOR_GAP_A 0x0B1114u
#define RF_COLOR_GAP_B 0x121A1Eu
#define RF_COLOR_PRESSED 0x243038u
#define RF_COLOR_GREEN_SOFT 0x132B25u
#define RF_COLOR_AMBER_SOFT 0x302411u
#define RF_COLOR_ORANGE_SOFT 0x321D14u
#define RF_COLOR_ON_PRIMARY 0x031311u
#define RF_COLOR_TARGET_1 0x42A5F5u
#define RF_COLOR_TARGET_2 0xB8BEC4u
#define RF_COLOR_TARGET_3 0xFF7A59u
#define RF_COLOR_TARGET_4 0x5DD39Eu

static const uint32_t g_target_accent_colors[RF_UI_DETECTION_COUNT] = {
    RF_COLOR_TARGET_1,
    RF_COLOR_TARGET_2,
    RF_COLOR_TARGET_3,
    RF_COLOR_TARGET_4,
};

static const lv_image_dsc_t * const
    g_target_strip_assets[RF_UI_DETECTION_COUNT] = {
        &rf_device_dji_mini_3_pro_strip,
        &rf_device_xiaobawang_strip,
        &rf_device_radiolink_at9s_strip,
        &rf_device_yunzhuo_t12_strip,
    };

static const lv_image_dsc_t * const
    g_target_detail_assets[RF_UI_DETECTION_COUNT] = {
        &rf_device_dji_mini_3_pro_detail,
        &rf_device_xiaobawang_detail,
        &rf_device_radiolink_at9s_detail,
        &rf_device_yunzhuo_t12_detail,
    };

#define RF_UI_SDRAM_NOINIT \
    BSP_ALIGN_VARIABLE(64) \
    BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".sdram_noinit")

enum {
    RF_METRIC_PANEL = 0,
    RF_METRIC_PRESENTED,
    RF_METRIC_RENDER_MAX,
    RF_METRIC_UNDERFLOW,
    RF_METRIC_COUNT,
};

enum {
    RF_ACQUISITION_SCAN = 0,
    RF_ACQUISITION_FOCUS,
    RF_ACQUISITION_MODE_COUNT,
};

enum {
    RF_CHANNEL_METRIC_PEAK = 0,
    RF_CHANNEL_METRIC_NOISE,
    RF_CHANNEL_METRIC_OCCUPANCY,
    RF_CHANNEL_METRIC_AGE,
    RF_CHANNEL_METRIC_COUNT,
};

enum {
    RF_HISTORY_OLDER = 0,
    RF_HISTORY_LIVE,
    RF_HISTORY_NEWER,
    RF_HISTORY_CONTROL_COUNT,
};

typedef struct {
    uint32_t panel_millihz;
    uint32_t presented_millihz;
    uint32_t render_max_us;
    uint32_t underflow_count;
    bool valid;
} render_metrics_t;

typedef struct {
    rf_ui_page_t page;
    bool running;
    bool external_spectrum_mode;
    bool model_placeholder;
    bool detection_ready;
    bool selector_pulse[RF_DEMO_CHANNEL_COUNT];
    bool spectrum_dirty[RF_DEMO_CHANNEL_COUNT];
    bool waterfall_dirty[RF_DEMO_CHANNEL_COUNT];
    bool rf_boxes_dirty[RF_DEMO_CHANNEL_COUNT];
    bool focus_mode;
    uint8_t pending_channel;
    uint8_t committed_channel;
    uint16_t waterfall_pan_columns;
    uint16_t waterfall_rendered_pan_columns;
    int32_t waterfall_drag_accumulator;
    uint32_t waterfall_pan_present_tick;

    lv_obj_t * screen;
    lv_obj_t * live_button;
    lv_obj_t * transport;
    lv_obj_t * live_icon;
    lv_obj_t * live_dot;
    lv_obj_t * live_label;
    lv_obj_t * transport_meta_label;
    lv_obj_t * transport_time_label;
    lv_obj_t * scan_rate_label;
    lv_obj_t * source_badge;
    lv_obj_t * source_badge_label;
    lv_obj_t * header_status_label;
    lv_obj_t * compare_button;
    lv_obj_t * compare_label;
    lv_obj_t * detection_status_label;
    lv_obj_t * performance_labels[RF_METRIC_COUNT];

    lv_obj_t * target_buttons[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_image_frames[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_images[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_number_labels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_name_labels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_channel_labels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_confidence_labels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_state_labels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * target_bars[RF_DEMO_CLASS_COUNT];

    lv_obj_t * selector_buttons[RF_DEMO_CHANNEL_COUNT];
    lv_obj_t * selector_titles[RF_DEMO_CHANNEL_COUNT];
    lv_obj_t * selector_frequencies[RF_DEMO_CHANNEL_COUNT];
    lv_obj_t * selector_counts[RF_DEMO_CHANNEL_COUNT];
    lv_obj_t * selector_bars[RF_DEMO_CHANNEL_COUNT];
    lv_obj_t * acquisition_buttons[RF_ACQUISITION_MODE_COUNT];
    lv_obj_t * acquisition_labels[RF_ACQUISITION_MODE_COUNT];

    lv_obj_t * alert_banner;
    lv_obj_t * alert_prefix;
    lv_obj_t * alert_drone_label;
    lv_obj_t * alert_idle_label;
    lv_obj_t * alert_details;
    lv_obj_t * alert_badge;
    lv_obj_t * alert_confidence_fill;
    lv_obj_t * detail_title_panel;
    lv_obj_t * detail_ranges_panel;
    lv_obj_t * detail_metric_panels[3];
    lv_obj_t * detail_confidence_panel;
    lv_obj_t * detail_preview_panel;
    lv_obj_t * detail_image_frame;
    lv_obj_t * detail_state_label;
    lv_obj_t * detail_class_label;
    lv_obj_t * detail_age_label;
    lv_obj_t * detail_source_label;
    lv_obj_t * detail_image;
    lv_obj_t * detail_empty_label;
    lv_obj_t * detail_range_name_labels[2];
    lv_obj_t * detail_range_value_labels[2];
    int8_t selected_detection_index;

    lv_obj_t * side_channel_label;
    lv_obj_t * side_frequency_label;
    lv_obj_t * side_metric_labels[RF_CHANNEL_METRIC_COUNT];
    lv_obj_t * side_refresh_label;
    lv_obj_t * history_slider;
    lv_obj_t * history_buttons[RF_HISTORY_CONTROL_COUNT];
    lv_obj_t * history_labels[RF_HISTORY_CONTROL_COUNT];

    lv_obj_t * compare_overlay;
    lv_obj_t * compare_cards[RF_DEMO_CLASS_COUNT];
    lv_obj_t * compare_names[RF_DEMO_CLASS_COUNT];
    lv_obj_t * compare_channels[RF_DEMO_CLASS_COUNT];
    lv_obj_t * compare_states[RF_DEMO_CLASS_COUNT];
    lv_obj_t * compare_confidences[RF_DEMO_CLASS_COUNT];
    bool history_slider_updating;
    int8_t pending_detection_index;

    lv_obj_t * selected_channel_label;
    lv_obj_t * selected_metric_labels[RF_CHANNEL_METRIC_COUNT];
    lv_obj_t * spectrum_image;
    lv_obj_t * spectrum_frequency_labels[5];
    lv_obj_t * waterfall_channel_label;
    lv_obj_t * waterfall_history_label;
    lv_obj_t * waterfall_image;
    lv_obj_t * waterfall_frequency_labels[5];
    lv_obj_t * waterfall_time_labels[5];
    lv_obj_t * waterfall_rf_boxes[RF_UI_MAX_RF_BOXES];
    lv_obj_t * spectrum_rf_boxes[RF_UI_MAX_RF_BOXES];
} rf_ui_state_t;

static rf_ui_state_t g_ui;
static render_metrics_t g_render_metrics;
volatile rf_ui_channel_switch_diag_t g_rf_ui_channel_switch_diag = {
    .magic = RF_UI_CHANNEL_SWITCH_DIAG_MAGIC,
    .version = RF_UI_CHANNEL_SWITCH_DIAG_VERSION,
    .state = RF_UI_CHANNEL_SWITCH_IDLE,
    .pending_channel = RF_UI_CHANNEL_NONE,
    .committed_channel = RF_UI_CHANNEL_NONE,
    .build_channel = RF_UI_CHANNEL_NONE,
};
volatile rf_ui_channel_soak_t g_rf_ui_channel_soak = {
    .magic = RF_UI_CHANNEL_SOAK_MAGIC,
    .version = RF_UI_CHANNEL_SOAK_VERSION,
    .next_channel = RF_UI_CHANNEL_NONE,
    .last_requested_channel = RF_UI_CHANNEL_NONE,
};
volatile rf_ui_runtime_monitor_t g_rf_ui_runtime_monitor = {
    .magic = RF_UI_RUNTIME_MONITOR_MAGIC,
    .version = RF_UI_RUNTIME_MONITOR_VERSION,
};
volatile rf_ui_input_diag_t g_rf_ui_input_diag = {
    .magic = RF_UI_INPUT_DIAG_MAGIC,
    .version = RF_UI_INPUT_DIAG_VERSION,
};
static uint16_t g_scan_rate_x10 = 82u;
static uint8_t g_spectrum_data[RF_UI_CHANNEL_COUNT][RF_UI_SPECTRUM_BINS];
static rf_ui_channel_metrics_t g_channel_metrics[RF_DEMO_CHANNEL_COUNT];
static rf_ui_detection_t g_detections[RF_DEMO_CLASS_COUNT];

typedef struct {
    bool valid;
    uint32_t revision;
    uint32_t session_id;
    uint32_t window_sequence;
} rf_ui_spectrum_identity_t;

typedef struct {
    bool valid;
    bool spectrum_snapshot_valid;
    uint16_t reserved;
    uint32_t revision;
    uint32_t spectrum_revision;
    uint32_t session_id;
    uint32_t window_sequence;
    uint32_t transport_sequence;
} rf_ui_complete_window_t;

static rf_ui_spectrum_identity_t g_spectrum_identity[RF_UI_CHANNEL_COUNT];
static rf_ui_complete_window_t g_complete_windows[RF_UI_CHANNEL_COUNT];
static uint8_t
    g_complete_spectrum_data[RF_UI_CHANNEL_COUNT][RF_UI_SPECTRUM_BINS];

typedef struct {
    rf_ui_rf_box_t boxes[RF_UI_MAX_RF_BOXES];
    uint32_t observation_generation[RF_UI_MAX_RF_BOXES];
    uint64_t anchor_end_columns[RF_UI_MAX_RF_BOXES];
    uint8_t count;
    uint8_t reserved[7];
} rf_ui_rf_box_batch_t;

typedef struct {
    bool valid;
    uint8_t channel;
    uint8_t count;
    bool queued;
    uint32_t session_id;
    uint32_t window_sequence;
    uint32_t staged_decision_generation;
    uint64_t anchor_end_column;
    rf_ui_rf_box_t boxes[RF_UI_MAX_RF_BOXES];
} rf_ui_pending_box_batch_t;

typedef struct {
    bool valid;
    uint8_t processed_channel_mask;
    uint16_t reserved;
    uint32_t generation;
    rf_ui_fusion_round_t round;
} rf_ui_fusion_decision_cache_t;

typedef struct {
    bool valid;
    uint8_t channel;
    uint16_t reserved;
    uint32_t session_id;
    uint32_t window_sequence;
    uint64_t waterfall_end_column;
} rf_ui_window_anchor_t;

static rf_ui_rf_box_batch_t g_rf_box_batches[RF_UI_CHANNEL_COUNT];
static rf_ui_rf_box_batch_t g_rf_box_pause_snapshot;
static rf_ui_rf_box_batch_t g_spectrum_rf_box_batches[RF_UI_CHANNEL_COUNT];
static rf_ui_rf_box_batch_t g_spectrum_rf_box_pause_snapshot;
static rf_ui_pending_box_batch_t
    g_pending_box_batches[RF_UI_PENDING_BOX_BATCH_CAPACITY];
static rf_ui_fusion_decision_cache_t
    g_fusion_decision_cache[RF_UI_FUSION_DECISION_CACHE_CAPACITY];
static rf_ui_window_anchor_t
    g_window_anchors[RF_UI_WINDOW_ANCHOR_CAPACITY];
static uint32_t g_window_anchor_write_index;
static uint32_t g_fusion_decision_generation;
static bool g_latest_box_window_valid[RF_UI_CHANNEL_COUNT];
static uint32_t g_latest_box_session_id[RF_UI_CHANNEL_COUNT];
static uint32_t g_latest_box_window_sequence[RF_UI_CHANNEL_COUNT];
static uint32_t
    g_last_detail_round_index[RF_UI_CHANNEL_COUNT][RF_UI_DETECTION_COUNT];
static uint8_t g_last_detail_round_valid_mask[RF_UI_CHANNEL_COUNT];
static uint32_t g_rf_box_observation_generation;
static uint64_t g_waterfall_total_columns[RF_UI_CHANNEL_COUNT];
static uint64_t g_waterfall_presented_columns[RF_UI_CHANNEL_COUNT];
static uint64_t g_waterfall_pause_total_columns;

static bool rf_box_window_anchor_find(uint32_t channel,
                                      uint32_t session_id,
                                      uint32_t window_sequence,
                                      uint64_t * waterfall_end_column)
{
    if(channel >= RF_UI_CHANNEL_COUNT || waterfall_end_column == NULL) {
        return false;
    }
    for(uint32_t index = 0U; index < RF_UI_WINDOW_ANCHOR_CAPACITY; ++index) {
        const rf_ui_window_anchor_t * const anchor = &g_window_anchors[index];
        if(anchor->valid && anchor->channel == channel &&
           anchor->session_id == session_id &&
           anchor->window_sequence == window_sequence) {
            *waterfall_end_column = anchor->waterfall_end_column;
            return true;
        }
    }
    return false;
}

static void rf_box_window_anchor_record(uint32_t channel,
                                        uint32_t session_id,
                                        uint32_t window_sequence,
                                        uint64_t waterfall_end_column)
{
    uint64_t ignored;
    if(rf_box_window_anchor_find(channel, session_id, window_sequence,
                                 &ignored)) return;

    rf_ui_window_anchor_t * const anchor =
        &g_window_anchors[g_window_anchor_write_index %
                          RF_UI_WINDOW_ANCHOR_CAPACITY];
    g_window_anchor_write_index++;
    *anchor = (rf_ui_window_anchor_t) {
        .valid = true,
        .channel = (uint8_t)channel,
        .session_id = session_id,
        .window_sequence = window_sequence,
        .waterfall_end_column = waterfall_end_column,
    };
}

static void rf_box_window_anchors_clear_channel(uint32_t channel)
{
    for(uint32_t index = 0U; index < RF_UI_WINDOW_ANCHOR_CAPACITY; ++index) {
        if(g_window_anchors[index].valid &&
           g_window_anchors[index].channel == channel) {
            g_window_anchors[index].valid = false;
        }
    }
}

/* One large spectrum source is enough: selecting a channel rerasterizes this
 * texture, and Dave2D scales it 2x horizontally into the visible plot. The
 * padded stride keeps every software-rendered row 64-byte aligned. */
typedef uint16_t rf_ui_spectrum_pixels_t[RF_SPECTRUM_TEXTURE_HEIGHT]
                                        [RF_SPECTRUM_TEXTURE_STRIDE_PIXELS];
static rf_ui_spectrum_pixels_t g_spectrum_pixels[RF_CHANNEL_SOURCE_COUNT]
    RF_UI_SDRAM_NOINIT;
static lv_image_dsc_t g_spectrum_image_dsc[RF_CHANNEL_SOURCE_COUNT];
static uint8_t g_spectrum_active_source;

typedef struct {
    uint16_t rows[RF_UI_WATERFALL_FREQ_BINS][RF_UI_WATERFALL_STORAGE_COLS];
} rf_ui_waterfall_ring_t;

/* Four histories retain 256 exact RF-time rows. The live viewport shows the
 * newest 160 rows; pausing snapshots one channel so ingestion can continue
 * while touch review remains stable. */
static rf_ui_waterfall_ring_t g_waterfall_rings[RF_DEMO_CHANNEL_COUNT]
    RF_UI_SDRAM_NOINIT;
typedef struct {
    uint16_t rows[RF_WATERFALL_DISPLAY_HEIGHT]
                 [RF_WATERFALL_RENDER_STORAGE_WIDTH];
    /* LVGL validates data_size from the offset data pointer. */
    uint16_t readable_tail[RF_WATERFALL_DISPLAY_WIDTH];
} rf_ui_waterfall_rgb565_ring_t;

typedef struct {
    uint8_t rows[RF_WATERFALL_DISPLAY_HEIGHT]
                [RF_WATERFALL_CLUT_STRIDE_BYTES];
} rf_ui_waterfall_clut4_phase_t;

typedef struct {
    rf_ui_waterfall_clut4_phase_t phase[RF_WATERFALL_CLUT_PHASE_COUNT];
} rf_ui_waterfall_clut4_ring_t;

typedef union {
    rf_ui_waterfall_rgb565_ring_t rgb565;
    rf_ui_waterfall_clut4_ring_t clut4;
} rf_ui_waterfall_render_storage_t;

static uint16_t g_waterfall_pause_snapshot[RF_UI_WATERFALL_FREQ_BINS]
                                          [RF_UI_WATERFALL_HISTORY_COLS]
    RF_UI_SDRAM_NOINIT;
static rf_ui_waterfall_render_storage_t
    g_waterfall_render_rings[RF_CHANNEL_SOURCE_COUNT] RF_UI_SDRAM_NOINIT;
static lv_image_dsc_t g_waterfall_image_dsc[RF_CHANNEL_SOURCE_COUNT];
static uint16_t g_waterfall_write_head[RF_DEMO_CHANNEL_COUNT];
static uint16_t g_waterfall_render_write_column;
static uint8_t g_waterfall_active_source;
typedef struct {
    bool valid;
    uint8_t channel;
    uint16_t history_head;
    uint16_t render_write_column;
    uint64_t total_columns;
} rf_ui_waterfall_source_state_t;
static rf_ui_waterfall_source_state_t
    g_waterfall_source_state[RF_CHANNEL_SOURCE_COUNT];
static uint16_t g_waterfall_color_lut[256u];
static uint8_t g_waterfall_clut_lut[256u];
static uint32_t
    g_waterfall_clut_palette[RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS];
static uint16_t g_waterfall_clut_rgb565_keys[
    256u + RF_UI_DETECTION_COUNT + 2u];
static uint8_t g_waterfall_clut_rgb565_values[
    256u + RF_UI_DETECTION_COUNT + 2u];
static uint16_t g_waterfall_clut_rgb565_count;
static uint8_t g_waterfall_source_bin_fast[RF_UI_WATERFALL_FREQ_BINS];
static uint16_t g_waterfall_render_x[RF_UI_WATERFALL_COLS + 1u];
static uint16_t g_waterfall_render_y[RF_UI_WATERFALL_FREQ_BINS + 1u];
static uint8_t g_waterfall_render_source_row[RF_WATERFALL_DISPLAY_HEIGHT];
static bool g_waterfall_lookup_ready;

typedef struct {
    bool requested;
    bool enabled;
    bool failed;
    bool prepared_valid;
    bool awaiting_latch;
    bool visual_dirty;
    bool fallback_rebuilding;
    bool fallback_disable_ready;
    bool boxes_dirty[RF_CHANNEL_SOURCE_COUNT];
    uint8_t display_source;
    uint8_t display_phase;
    uint8_t prepared_source;
    uint8_t prepared_phase;
    uint16_t prepared_pixels;
    uint64_t presented_end_pixels;
    uint64_t prepared_end_pixels;
    uint64_t pace_accumulator;
    uint32_t pace_last_tick;
    uint32_t next_generation;
    uint32_t submitted_generation;
    bool pace_tick_valid;
    rf_ui_waterfall_overlay_frame_t prepared_frame;
} rf_ui_waterfall_overlay_state_t;

static rf_ui_waterfall_overlay_state_t g_waterfall_overlay;

typedef struct {
    bool active;
    uint8_t channel;
    uint8_t source;
    uint16_t render_y;
    uint16_t source_row;
    uint16_t source_head;
    uint16_t target_head;
    uint64_t source_total;
    uint64_t target_total;
} rf_ui_waterfall_overlay_sync_t;

static rf_ui_waterfall_overlay_sync_t g_waterfall_overlay_sync;

typedef struct {
    rf_ui_channel_switch_state_t state;
    uint8_t channel;
    uint8_t source;
    bool waterfall_cache_reused;
    uint16_t spectrum_row;
    uint16_t spectrum_segment;
    uint16_t waterfall_render_y;
    uint16_t waterfall_source_row;
    uint16_t base_write_head;
    uint16_t logical_start;
    uint16_t catchup_source_head;
    uint16_t render_write_column;
    uint32_t request_generation;
    uint32_t required_spectrum_revision;
    uint32_t required_window_revision;
    uint32_t spectrum_revision;
    uint32_t session_id;
    uint32_t window_sequence;
    uint64_t base_total_columns;
    uint64_t caught_up_total_columns;
    uint64_t catchup_target_total_columns;
    uint16_t catchup_target_head;
    int16_t spectrum_x[RF_UI_SPECTRUM_BINS];
    int16_t spectrum_y[RF_UI_SPECTRUM_BINS];
    uint16_t spectrum_peak_index;
    uint8_t spectrum_snapshot[RF_UI_SPECTRUM_BINS];
} rf_ui_channel_build_t;

static rf_ui_channel_build_t g_channel_build;

typedef enum {
    RF_UI_LIVE_BUILD_IDLE = 0,
    RF_UI_LIVE_BUILD_BASE,
    RF_UI_LIVE_BUILD_CATCHUP,
    RF_UI_LIVE_BUILD_READY,
} rf_ui_live_build_state_t;

typedef struct {
    rf_ui_live_build_state_t state;
    uint8_t channel;
    uint8_t source;
    uint16_t waterfall_render_y;
    uint16_t waterfall_source_row;
    uint16_t base_write_head;
    uint16_t logical_start;
    uint16_t catchup_source_head;
    uint16_t render_write_column;
    uint16_t catchup_target_head;
    uint64_t base_total_columns;
    uint64_t caught_up_total_columns;
    uint64_t catchup_target_total_columns;
    uint32_t request_generation;
} rf_ui_live_build_t;

typedef enum {
    RF_UI_RENDER_NONE = 0,
    RF_UI_RENDER_CHANNEL_SWITCH,
    RF_UI_RENDER_LIVE_WATERFALL,
} rf_ui_render_kind_t;

typedef struct {
    rf_ui_render_kind_t kind;
    bool active;
    bool commit_queued;
    uint8_t channel;
    uint8_t source;
    uint8_t previous_waterfall_source;
    uint8_t previous_spectrum_source;
    uint8_t previous_committed_channel;
    uint8_t previous_pending_channel;
    uint16_t render_write_column;
    uint16_t previous_render_write_column;
    int8_t previous_selected_detection_index;
    int8_t previous_pending_detection_index;
    int8_t staged_selected_detection_index;
    uint8_t metadata_stage;
    bool metadata_refresh_pending;
    bool previous_waterfall_dirty;
    bool previous_spectrum_dirty;
    bool previous_rf_boxes_dirty;
    uint64_t previous_presented_columns;
    uint32_t deferred_abort_baseline;
    uint16_t next_row;
    uint16_t spectrum_render_row;
    uint64_t target_total_columns;
} rf_ui_render_txn_t;

static rf_ui_live_build_t g_live_build;
static rf_ui_render_txn_t g_render_txn;

static void render_transaction_abort(void);
static void render_transaction_poll_complete(void);
static bool render_transaction_begin(rf_ui_render_kind_t kind,
                                     uint32_t channel,
                                     uint8_t source,
                                     uint16_t render_write_column,
                                     uint64_t target_total_columns);
static bool render_transaction_step(void);
static bool channel_switch_commit(void);
static bool channel_switch_stage_metadata_step(void);
static bool channel_switch_defer_metadata_refresh(void);
static bool channel_switch_prepare_render(void);
static bool live_build_start(uint32_t channel);
static bool live_build_step(void);
static bool live_build_prepare_catchup(void);
static void live_build_cancel(bool count_cancellation);
static void channel_switch_soak_step(void);
static bool waterfall_overlay_sync_start(uint32_t channel);
static bool waterfall_overlay_sync_step(void);
static void waterfall_overlay_sync_cancel(void);

_Static_assert(sizeof(g_spectrum_pixels[0]) == 0x8200u,
               "single-band spectrum texture size changed");
_Static_assert(sizeof(g_spectrum_pixels) == 0x10400u,
               "dual-source spectrum texture size changed");
_Static_assert(sizeof(rf_ui_rf_box_t) == 8u,
               "RF box UI contract must remain compact");
_Static_assert(sizeof(rf_ui_fusion_round_t) == 48u,
               "RF fusion round UI contract changed");
_Static_assert(sizeof(rf_ui_pending_box_batch_t) == 56u,
               "RF pending box batch layout changed");
_Static_assert(sizeof(rf_ui_fusion_decision_cache_t) == 56u,
               "RF fusion decision cache layout changed");
_Static_assert(RF_UI_DETECTION_COUNT <= RF_UI_MAX_RF_BOXES,
               "one persistent RF box per detection must fit the batch");
_Static_assert(RF_UI_SPECTRUM_BINS == 256u,
               "B2 spectrum drawing contract must remain 256 points");
_Static_assert(sizeof(g_waterfall_rings) == 0xC0000u,
               "four-channel dual-mapped waterfall texture size changed");
_Static_assert(sizeof(g_waterfall_pause_snapshot) == 0x18000u,
               "paused waterfall snapshot size changed");
_Static_assert(sizeof(g_waterfall_render_rings[0]) == 0xC5440u,
               "RGB565/CLUT4 source union size changed");
_Static_assert(sizeof(g_waterfall_render_rings) == 0x18A880u,
               "dual-source waterfall render ring size changed");
_Static_assert(sizeof(g_spectrum_pixels[1]) +
               sizeof(g_waterfall_render_rings[1]) == 0xCD640u,
               "channel-switch SDRAM increment must remain 841280 bytes");
_Static_assert(RF_UI_WATERFALL_HISTORY_COLS >= RF_UI_WATERFALL_COLS,
               "waterfall history must cover the visible viewport");
_Static_assert((RF_SPECTRUM_TEXTURE_STRIDE_BYTES & 63u) == 0u,
               "spectrum rows must remain 64-byte aligned");
_Static_assert(((RF_UI_WATERFALL_STORAGE_COLS * sizeof(uint16_t)) & 63u) == 0u,
               "waterfall rows must remain 64-byte aligned");
_Static_assert((RF_WATERFALL_RENDER_STRIDE_BYTES & 63u) == 0u,
               "waterfall render rows must remain 64-byte aligned");
_Static_assert(sizeof(rf_ui_waterfall_clut4_phase_t) == 0x4EC00u,
               "single CLUT4 phase size changed");
_Static_assert(sizeof(rf_ui_waterfall_clut4_ring_t) == 0x9D800u,
               "CLUT4 history ring size changed");
_Static_assert(sizeof(rf_ui_waterfall_clut4_ring_t) <=
               sizeof(rf_ui_waterfall_rgb565_ring_t),
               "CLUT4 history ring must fit the existing RGB565 source");
_Static_assert((RF_WATERFALL_CLUT_STRIDE_BYTES & 63u) == 0u,
               "CLUT4 Layer 2 rows must remain 64-byte aligned");
_Static_assert((RF_WATERFALL_CLUT_RING_WIDTH & 1u) == 0u,
               "CLUT4 mirror boundary must be byte aligned");
_Static_assert(RF_WATERFALL_CLUT_BOX_FIRST + RF_UI_DETECTION_COUNT ==
               RF_WATERFALL_CLUT_GAP_A,
               "CLUT4 palette must preserve four detection colors");
_Static_assert(RF_WATERFALL_CLUT_GAP_B <
               RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS,
               "CLUT4 palette index exceeds the hardware table");
_Static_assert(RF_WATERFALL_CLUT_ALIGNMENT_PIXELS == 128u,
               "CLUT4 source alignment must remain 128 pixels");
_Static_assert(RF_WATERFALL_CLUT_PHASE_OFFSET_PIXELS *
               RF_WATERFALL_CLUT_PHASE_COUNT ==
               RF_WATERFALL_CLUT_ALIGNMENT_PIXELS,
               "CLUT4 phases must cover each 64-pixel source head");
_Static_assert((RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK *
                RF_WATERFALL_CLUT_BUILD_ROW_WRITE_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one CLUT4 build chunk exceeds the SDRAM write budget");
_Static_assert(RF_UI_MAX_RF_BOXES *
               (RF_WATERFALL_BOX_HISTORY_MAX_WRITE_BYTES +
                RF_WATERFALL_BOX_CLUT_MAX_WRITE_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one RF box batch exceeds the SDRAM write budget");
_Static_assert((RF_WATERFALL_DISPLAY_WIDTH & 1u) == 0u,
               "CLUT4 viewport width must be even");
_Static_assert((RF_WATERFALL_DISPLAY_WIDTH & 31u) == 0u,
               "dual-mapped waterfall width must preserve a 64-byte RGB565 stride");
_Static_assert((RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK *
                RF_WATERFALL_RENDER_STRIDE_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one switch build chunk exceeds the SDRAM write budget");
_Static_assert((RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK *
                RF_WATERFALL_VISIBLE_ROW_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one deferred framebuffer chunk exceeds the SDRAM write budget");
_Static_assert((RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK *
                 RF_SPECTRUM_DISPLAY_WIDTH * sizeof(uint16_t)) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one spectrum render chunk exceeds the SDRAM write budget");
_Static_assert((RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK *
                 RF_SPECTRUM_BASE_ROW_WRITE_BYTES +
                 RF_SPECTRUM_MAX_BASE_CHUNK_DIVIDERS *
                 RF_SPECTRUM_HORIZONTAL_DIVIDER_WRITE_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one spectrum base chunk exceeds the SDRAM write budget");
_Static_assert((RF_CHANNEL_SWITCH_SPECTRUM_SEGMENTS_PER_TICK *
                 RF_SPECTRUM_MAX_TRACE_SEGMENT_WRITE_BYTES) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "one spectrum trace chunk exceeds the SDRAM write budget");
_Static_assert((RF_SPECTRUM_PEAK_MAX_PIXELS * sizeof(uint16_t)) <=
               RF_CHANNEL_SWITCH_MAX_WRITE_BYTES,
               "the spectrum peak exceeds the SDRAM write budget");
_Static_assert((RF_UI_WATERFALL_HISTORY_COLS *
                 RF_WATERFALL_CLUT_COLUMN_WRITE_BYTES) <=
               RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES,
               "one CLUT4 catch-up row exceeds the SDRAM write budget");
_Static_assert((RF_UI_WATERFALL_HISTORY_COLS *
                 RF_WATERFALL_CLUT_PIXELS_PER_COLUMN *
                 sizeof(uint16_t) * 2u) <=
               RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES,
               "one RGB565 catch-up row exceeds the SDRAM write budget");
_Static_assert(RF_UI_WATERFALL_COLS <= RF_WATERFALL_DISPLAY_WIDTH,
               "every RF-time row must remain visible");
_Static_assert((RF_WATERFALL_RF_WINDOW_SAMPLES %
                RF_WATERFALL_RF_ROWS_PER_WINDOW) == 0u,
               "waterfall RF rows must contain an integer sample count");
_Static_assert(RF_UI_WATERFALL_COLS ==
               (10u * RF_WATERFALL_RF_ROWS_PER_WINDOW),
               "waterfall view must cover ten complete RF windows");
_Static_assert((RF_MODE_X +
                (RF_ACQUISITION_MODE_COUNT * RF_MODE_WIDTH) +
                ((RF_ACQUISITION_MODE_COUNT - 1u) * RF_MODE_GAP)) <=
               RF_TRANSPORT_X,
               "acquisition controls overlap transport");
_Static_assert((RF_TRANSPORT_X + RF_TRANSPORT_WIDTH) <= RF_SCREEN_WIDTH,
               "header transport exceeds the screen");
_Static_assert((RF_LIVE_BUTTON_X + RF_LIVE_BUTTON_WIDTH) ==
               RF_HISTORY_OLDER_X,
               "live and history controls must be adjacent");
_Static_assert((RF_HISTORY_OLDER_X + RF_HISTORY_BUTTON_WIDTH) ==
               RF_HISTORY_TIMELINE_X,
               "older button and timeline must be adjacent");
_Static_assert((RF_HISTORY_TIMELINE_X + RF_HISTORY_TIMELINE_WIDTH) ==
               RF_HISTORY_NEWER_X,
               "timeline and newer button must be adjacent");
_Static_assert((RF_HISTORY_NEWER_X + RF_HISTORY_BUTTON_WIDTH) ==
               RF_SOURCE_BADGE_X,
               "newer button and source badge must be adjacent");
_Static_assert((RF_SOURCE_BADGE_X + RF_SOURCE_BADGE_WIDTH) ==
               RF_TRANSPORT_WIDTH,
               "transport children must fill the transport");
_Static_assert((RF_TARGET_CARD_WIDTH * RF_UI_DETECTION_COUNT) == RF_SCREEN_WIDTH,
               "four target cards must fill the target strip");
_Static_assert((RF_CHANNEL_CARD_WIDTH * RF_UI_CHANNEL_COUNT) ==
               RF_CHANNEL_DECK_WIDTH,
               "four channel cards must fill the channel deck");
_Static_assert(RF_CHANNEL_DECK_WIDTH == RF_PANEL_WIDTH,
               "channel controls must fill the analysis width");
_Static_assert((RF_PANEL_WIDTH + RF_SIDEBAR_WIDTH) == RF_SCREEN_WIDTH,
               "daylight analysis and detail columns must fill the screen");
_Static_assert((RF_TARGET_STRIP_Y + RF_TARGET_STRIP_HEIGHT) == RF_WATERFALL_Y,
               "target strip and waterfall must be adjacent");
_Static_assert((RF_WATERFALL_Y + RF_WATERFALL_HEIGHT) == RF_SPECTRUM_Y,
               "daylight waterfall and spectrum panels must be adjacent");
_Static_assert((RF_SPECTRUM_Y + RF_SPECTRUM_HEIGHT) == RF_BOTTOM_Y,
               "spectrum and bottom controls must be adjacent");
_Static_assert((RF_BOTTOM_Y + RF_BOTTOM_HEIGHT) == RF_METRICS_Y,
               "bottom controls and footer must be adjacent");
_Static_assert((RF_METRICS_Y + RF_METRICS_HEIGHT) == RF_SCREEN_HEIGHT,
               "daylight vertical geometry must fill the screen");
_Static_assert((RF_SIDEBAR_Y + RF_SIDEBAR_HEIGHT) == RF_METRICS_Y,
               "daylight detail column must end at the footer");
_Static_assert((RF_PLOT_X + RF_WATERFALL_DISPLAY_WIDTH) <= RF_PANEL_WIDTH,
               "daylight waterfall plot exceeds the analysis column");
_Static_assert((RF_PLOT_X + RF_SPECTRUM_DISPLAY_WIDTH) <= RF_PANEL_WIDTH,
               "daylight spectrum plot exceeds the analysis column");
#if LV_DRAW_BUF_STRIDE_ALIGN != 1
#error "dual-mapped waterfall requires LVGL to preserve the source stride"
#endif

#define color(rgb_) ((lv_color_t) {                                      \
    .blue = (uint8_t)((uint32_t)(rgb_) & 0xFFu),                         \
    .green = (uint8_t)(((uint32_t)(rgb_) >> 8U) & 0xFFu),                \
    .red = (uint8_t)(((uint32_t)(rgb_) >> 16U) & 0xFFu),                 \
})

static void reset_object(lv_obj_t * object)
{
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t * create_box(lv_obj_t * parent, int32_t x, int32_t y,
                             int32_t width, int32_t height,
                             uint32_t background, lv_opa_t background_opa)
{
    lv_obj_t * object = lv_obj_create(parent);
    reset_object(object);
    /* Plain boxes are layout and decoration by default. Every real control
     * below opts into CLICKABLE explicitly, preventing a nested bar or dot
     * from becoming the deepest hit target and swallowing its parent click. */
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color(background), 0);
    lv_obj_set_style_bg_opa(object, background_opa, 0);
    return object;
}

static lv_obj_t * create_label(lv_obj_t * parent, int32_t x, int32_t y,
                               int32_t width, int32_t height,
                               const char * text, const lv_font_t * font,
                               uint32_t text_color, lv_text_align_t alignment)
{
    lv_obj_t * label = lv_label_create(parent);
    reset_object(label);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(text_color), 0);
    lv_obj_set_style_text_align(label, alignment, 0);
    return label;
}

static lv_obj_t * create_panel(int32_t y, int32_t height)
{
    lv_obj_t * panel = create_box(g_ui.screen, RF_PANEL_X, y, RF_PANEL_WIDTH,
                                  height, RF_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_radius(panel, 0, 0);
    return panel;
}

static void set_visible(lv_obj_t * object, bool visible)
{
    if(object == NULL) return;
    if(visible) lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * create_rf_box_overlay(lv_obj_t * parent)
{
    lv_obj_t * object = create_box(parent, 0, 0, 2, 2,
                                   RF_COLOR_PRIMARY, LV_OPA_TRANSP);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(object, 2, 0);
    lv_obj_set_style_border_color(object, color(RF_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 0, 0);
    set_visible(object, false);
    return object;
}

static uint32_t scale_ceil_u32(uint32_t value, uint32_t output_size)
{
    return (value * output_size + RF_UI_RF_COORD_SCALE - 1U) /
           RF_UI_RF_COORD_SCALE;
}

static void style_rf_box_overlay(lv_obj_t * object,
                                 const rf_ui_rf_box_t * box)
{
    const uint32_t rgb = g_target_accent_colors[box->detection_index];
    const lv_opa_t border_opa = (box->confidence_percent >= 50U) ?
                                LV_OPA_COVER : (lv_opa_t)180U;
    const lv_opa_t fill_opa = (lv_opa_t)(18U +
                              ((uint32_t)box->confidence_percent * 24U) / 100U);

    lv_obj_set_style_border_color(object, color(rgb), 0);
    lv_obj_set_style_border_opa(object, border_opa, 0);
    lv_obj_set_style_bg_color(object, color(rgb), 0);
    lv_obj_set_style_bg_opa(object, fill_opa, 0);
}

static void hide_rf_box_overlays(uint32_t index)
{
    set_visible(g_ui.waterfall_rf_boxes[index], false);
    set_visible(g_ui.spectrum_rf_boxes[index], false);
}

static bool rf_box_batch_has_anchor_after(
    const rf_ui_rf_box_batch_t * batch,
    uint64_t total_columns)
{
    if(batch == NULL) return false;
    for(uint32_t index = 0U; index < batch->count; ++index) {
        if(batch->anchor_end_columns[index] > total_columns) return true;
    }
    return false;
}

static void refresh_rf_box_overlays(void)
{
    const bool layer2_waterfall =
        g_waterfall_overlay.requested && !g_waterfall_overlay.failed;
    const rf_ui_rf_box_batch_t * waterfall_batch = g_ui.running ?
        &g_rf_box_batches[g_ui.committed_channel] :
        &g_rf_box_pause_snapshot;
    const rf_ui_rf_box_batch_t * spectrum_batch = g_ui.running ?
        &g_spectrum_rf_box_batches[g_ui.committed_channel] :
        &g_spectrum_rf_box_pause_snapshot;
    uint64_t viewport_end_u64 = g_ui.running ?
        g_waterfall_total_columns[g_ui.committed_channel] :
        g_waterfall_pause_total_columns;

    if(!g_ui.running) {
        const uint64_t pan = g_ui.waterfall_pan_columns;
        viewport_end_u64 = (viewport_end_u64 > pan) ?
                           viewport_end_u64 - pan : 0U;
    }
    const int64_t viewport_end = (int64_t)viewport_end_u64;
    const int64_t viewport_start = viewport_end - RF_UI_WATERFALL_COLS;

    for(uint32_t index = 0U; index < RF_UI_MAX_RF_BOXES; ++index) {
        hide_rf_box_overlays(index);

        /* Spectrum boxes describe only the latest complete RF window. They
         * intentionally do not inherit the persistent detail/waterfall cache. */
        if(index < spectrum_batch->count) {
            const rf_ui_rf_box_t * spectrum_box =
                &spectrum_batch->boxes[index];
            uint32_t spectrum_frequency_end =
                (uint32_t)spectrum_box->frequency_start_q8 +
                spectrum_box->frequency_span_q8;
            if((spectrum_box->flags & RF_UI_RF_BOX_FLAG_VALID) != 0U &&
               spectrum_box->frequency_span_q8 != 0U &&
               spectrum_box->detection_index < RF_UI_DETECTION_COUNT) {
                if(spectrum_frequency_end > RF_UI_RF_COORD_SCALE) {
                    spectrum_frequency_end = RF_UI_RF_COORD_SCALE;
                }
                const uint32_t spectrum_left =
                    ((uint32_t)spectrum_box->frequency_start_q8 *
                     RF_SPECTRUM_DISPLAY_WIDTH) / RF_UI_RF_COORD_SCALE;
                uint32_t spectrum_right = scale_ceil_u32(
                    spectrum_frequency_end, RF_SPECTRUM_DISPLAY_WIDTH);
                if(spectrum_right <= spectrum_left) {
                    spectrum_right = spectrum_left + 2U;
                }
                if(spectrum_right > RF_SPECTRUM_DISPLAY_WIDTH) {
                    spectrum_right = RF_SPECTRUM_DISPLAY_WIDTH;
                }
                lv_obj_set_pos(g_ui.spectrum_rf_boxes[index],
                               RF_PLOT_X + (int32_t)spectrum_left, 20);
                lv_obj_set_size(g_ui.spectrum_rf_boxes[index],
                                (int32_t)(spectrum_right - spectrum_left),
                                RF_SPECTRUM_DISPLAY_HEIGHT);
                style_rf_box_overlay(g_ui.spectrum_rf_boxes[index],
                                     spectrum_box);
                set_visible(g_ui.spectrum_rf_boxes[index], true);
            }
        }

        if(index >= waterfall_batch->count) continue;
        const rf_ui_rf_box_t * box = &waterfall_batch->boxes[index];
        uint32_t frequency_end = (uint32_t)box->frequency_start_q8 +
                                 box->frequency_span_q8;
        uint32_t time_end = (uint32_t)box->time_start_q8 +
                            box->time_span_q8;
        if((box->flags & RF_UI_RF_BOX_FLAG_VALID) == 0U ||
           box->frequency_span_q8 == 0U || box->time_span_q8 == 0U ||
           box->detection_index >= RF_UI_DETECTION_COUNT) {
            continue;
        }
        if(frequency_end > RF_UI_RF_COORD_SCALE) {
            frequency_end = RF_UI_RF_COORD_SCALE;
        }
        if(time_end > RF_UI_RF_COORD_SCALE) {
            time_end = RF_UI_RF_COORD_SCALE;
        }

        if(layer2_waterfall) {
            continue;
        }

        const uint32_t start_column_offset =
            ((uint32_t)box->time_start_q8 *
             RF_WATERFALL_RF_ROWS_PER_WINDOW) / RF_UI_RF_COORD_SCALE;
        uint32_t end_column_offset =
            (time_end * RF_WATERFALL_RF_ROWS_PER_WINDOW +
             RF_UI_RF_COORD_SCALE - 1U) / RF_UI_RF_COORD_SCALE;
        if(end_column_offset <= start_column_offset) {
            end_column_offset = start_column_offset + 1U;
        }
        if(end_column_offset > RF_WATERFALL_RF_ROWS_PER_WINDOW) {
            end_column_offset = RF_WATERFALL_RF_ROWS_PER_WINDOW;
        }
        const uint64_t window_start =
            (waterfall_batch->anchor_end_columns[index] >=
             RF_WATERFALL_RF_ROWS_PER_WINDOW) ?
            waterfall_batch->anchor_end_columns[index] -
                RF_WATERFALL_RF_ROWS_PER_WINDOW : 0U;
        const int64_t box_start =
            (int64_t)(window_start + start_column_offset);
        const int64_t box_end =
            (int64_t)(window_start + end_column_offset);
        if(box_end <= viewport_start || box_start >= viewport_end) {
            set_visible(g_ui.waterfall_rf_boxes[index], false);
            continue;
        }

        int64_t visible_start = box_start - viewport_start;
        int64_t visible_end = box_end - viewport_start;
        if(visible_start < 0) visible_start = 0;
        if(visible_end > RF_UI_WATERFALL_COLS) {
            visible_end = RF_UI_WATERFALL_COLS;
        }
        const uint32_t waterfall_left =
            (uint32_t)((visible_start * RF_WATERFALL_DISPLAY_WIDTH) /
                       RF_UI_WATERFALL_COLS);
        uint32_t waterfall_right = (uint32_t)(
            (visible_end * RF_WATERFALL_DISPLAY_WIDTH +
             RF_UI_WATERFALL_COLS - 1U) / RF_UI_WATERFALL_COLS);
        if(waterfall_right <= waterfall_left) {
            waterfall_right = waterfall_left + 2U;
        }
        if(waterfall_right > RF_WATERFALL_DISPLAY_WIDTH) {
            waterfall_right = RF_WATERFALL_DISPLAY_WIDTH;
        }

        const uint32_t waterfall_top =
            ((RF_UI_RF_COORD_SCALE - frequency_end) *
             RF_WATERFALL_DISPLAY_HEIGHT) / RF_UI_RF_COORD_SCALE;
        uint32_t waterfall_bottom = scale_ceil_u32(
            RF_UI_RF_COORD_SCALE - box->frequency_start_q8,
            RF_WATERFALL_DISPLAY_HEIGHT);
        if(waterfall_bottom <= waterfall_top) {
            waterfall_bottom = waterfall_top + 2U;
        }
        if(waterfall_bottom > RF_WATERFALL_DISPLAY_HEIGHT) {
            waterfall_bottom = RF_WATERFALL_DISPLAY_HEIGHT;
        }

        lv_obj_set_pos(g_ui.waterfall_rf_boxes[index],
                       RF_PLOT_X + (int32_t)waterfall_left,
                       36 + (int32_t)waterfall_top);
        lv_obj_set_size(g_ui.waterfall_rf_boxes[index],
                        (int32_t)(waterfall_right - waterfall_left),
                        (int32_t)(waterfall_bottom - waterfall_top));
        style_rf_box_overlay(g_ui.waterfall_rf_boxes[index], box);
        set_visible(g_ui.waterfall_rf_boxes[index], true);
    }
}

static uint16_t rgb565(uint32_t rgb)
{
    return lv_color_to_u16(color(rgb));
}

static void fill_row(uint16_t * row, uint16_t pixel, uint32_t pixel_count)
{
    const uint32_t paired = (uint32_t) pixel | ((uint32_t) pixel << 16);
    if(pixel_count != 0U && (((uintptr_t)row & 3U) != 0U)) {
        *row++ = pixel;
        pixel_count--;
    }
    while(pixel_count >= 2U) {
        memcpy(row, &paired, sizeof(paired));
        row += 2;
        pixel_count -= 2U;
    }
    if(pixel_count != 0U) *row = pixel;
}

static uint32_t spectrum_put_pixel(rf_ui_spectrum_pixels_t pixels,
                                   int32_t x, int32_t y, uint16_t pixel)
{
    if((uint32_t) x < RF_SPECTRUM_TEXTURE_WIDTH &&
       (uint32_t) y < RF_SPECTRUM_TEXTURE_HEIGHT) {
        pixels[y][x] = pixel;
        return sizeof(uint16_t);
    }
    return 0U;
}

static int32_t integer_abs(int32_t value)
{
    return value < 0 ? -value : value;
}

static uint32_t spectrum_draw_line(rf_ui_spectrum_pixels_t pixels,
                                   int32_t x0, int32_t y0,
                                   int32_t x1, int32_t y1, uint16_t pixel)
{
    const int32_t dx = integer_abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -integer_abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    uint32_t bytes_written = 0U;

    for(;;) {
        bytes_written += spectrum_put_pixel(pixels, x0, y0, pixel);
        bytes_written += spectrum_put_pixel(pixels, x0, y0 + 1, pixel);
        if(x0 == x1 && y0 == y1) break;

        const int32_t twice_error = error * 2;
        if(twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if(twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    return bytes_written;
}

static uint32_t spectrum_fill_column(rf_ui_spectrum_pixels_t pixels,
                                     int32_t x, int32_t y, uint16_t pixel)
{
    if((uint32_t) x >= RF_SPECTRUM_TEXTURE_WIDTH) return 0U;
    if(y < 0) y = 0;
    if(y >= (int32_t) RF_SPECTRUM_TEXTURE_HEIGHT) return 0U;

    for(int32_t row = y; row < (int32_t) RF_SPECTRUM_TEXTURE_HEIGHT; ++row) {
        pixels[row][x] = pixel;
    }
    return ((uint32_t)RF_SPECTRUM_TEXTURE_HEIGHT - (uint32_t)y) *
           sizeof(uint16_t);
}

static uint32_t spectrum_fill_base_rows(rf_ui_spectrum_pixels_t pixels,
                                        uint32_t first_row,
                                        uint32_t row_count)
{
    const uint16_t background = rgb565(RF_COLOR_PLOT);
    const uint16_t divider = rgb565(RF_COLOR_DIVIDER);
    const uint32_t end_row = first_row + row_count;
    uint32_t bytes_written = 0U;

    for(uint32_t row = first_row;
        row < end_row && row < RF_SPECTRUM_TEXTURE_HEIGHT;
        ++row) {
        fill_row(pixels[row], background, RF_SPECTRUM_TEXTURE_STRIDE_PIXELS);
        bytes_written += RF_SPECTRUM_TEXTURE_STRIDE_BYTES;
        for(uint32_t index = 0U; index <= 4U; ++index) {
            const uint32_t divider_y =
                ((RF_SPECTRUM_TEXTURE_HEIGHT - 1U) * index) / 4U;
            if(row == divider_y) {
                fill_row(pixels[row], divider, RF_SPECTRUM_TEXTURE_WIDTH);
                bytes_written += RF_SPECTRUM_TEXTURE_WIDTH * sizeof(uint16_t);
                break;
            }
        }
        for(uint32_t index = 0U; index <= 8U; ++index) {
            const uint32_t x =
                ((RF_SPECTRUM_TEXTURE_WIDTH - 1U) * index) / 8U;
            pixels[row][x] = divider;
            bytes_written += sizeof(uint16_t);
        }
    }
    return bytes_written;
}

static void spectrum_prepare_geometry(const uint8_t * data,
                                      int16_t point_x[RF_UI_SPECTRUM_BINS],
                                      int16_t point_y[RF_UI_SPECTRUM_BINS],
                                      uint16_t * peak_index_out)
{
    int32_t peak_value = -1;
    uint32_t peak_index = 0u;

    for(uint32_t index = 0; index < RF_UI_SPECTRUM_BINS; ++index) {
        int32_t value = data[index];
        if(value < 0) value = 0;
        if(value > 255) value = 255;

        point_x[index] = (int16_t) ((index * (RF_SPECTRUM_TEXTURE_WIDTH - 1u)) /
                                    (RF_UI_SPECTRUM_BINS - 1u));
        point_y[index] = (int16_t) ((RF_SPECTRUM_TEXTURE_HEIGHT - 1u) -
                                    ((uint32_t) value *
                                     (RF_SPECTRUM_TEXTURE_HEIGHT - 1u)) / 255u);
        if(value > peak_value) {
            peak_value = value;
            peak_index = index;
        }
    }
    *peak_index_out = (uint16_t)peak_index;
}

static uint16_t spectrum_draw_trace_segments(
    rf_ui_spectrum_pixels_t pixels,
    const int16_t point_x[RF_UI_SPECTRUM_BINS],
    const int16_t point_y[RF_UI_SPECTRUM_BINS],
    uint16_t first_segment,
    uint16_t segment_count,
    uint32_t * bytes_written_out)
{
    const uint16_t area_fill = rgb565(RF_COLOR_PRIMARY_SOFT);
    const uint16_t trace = rgb565(RF_COLOR_PRIMARY);
    uint32_t end_segment = (uint32_t)first_segment + segment_count;
    uint32_t bytes_written = 0U;
    if(first_segment < 1U) first_segment = 1U;
    if(end_segment > RF_UI_SPECTRUM_BINS) {
        end_segment = RF_UI_SPECTRUM_BINS;
    }

    for(uint32_t index = first_segment; index < end_segment; ++index) {
        const int32_t x0 = point_x[index - 1u];
        const int32_t x1 = point_x[index];
        const int32_t y0 = point_y[index - 1u];
        const int32_t y1 = point_y[index];
        const int32_t span = x1 - x0;

        for(int32_t x = x0; x <= x1; ++x) {
            const int32_t y = span > 0 ? y0 + ((y1 - y0) * (x - x0)) / span : y1;
            bytes_written += spectrum_fill_column(pixels, x, y, area_fill);
        }
        bytes_written += spectrum_draw_line(pixels, x0, y0, x1, y1, trace);
    }
    if(bytes_written_out != NULL) *bytes_written_out = bytes_written;
    return (uint16_t)end_segment;
}

static uint32_t spectrum_draw_peak(
    rf_ui_spectrum_pixels_t pixels,
    const int16_t point_x[RF_UI_SPECTRUM_BINS],
    const int16_t point_y[RF_UI_SPECTRUM_BINS],
    uint16_t peak_index)
{
    const uint16_t peak = rgb565(RF_COLOR_ORANGE);
    const int32_t peak_x = point_x[peak_index];
    const int32_t peak_y = point_y[peak_index];
    uint32_t bytes_written = 0U;
    for(int32_t y = -2; y <= 2; ++y) {
        for(int32_t x = -2; x <= 2; ++x) {
            if(x * x + y * y <= 4) {
                bytes_written += spectrum_put_pixel(
                    pixels, peak_x + x, peak_y + y, peak);
            }
        }
    }
    return bytes_written;
}

static void rasterize_spectrum_to(rf_ui_spectrum_pixels_t pixels,
                                  const uint8_t * data)
{
    int16_t point_x[RF_UI_SPECTRUM_BINS];
    int16_t point_y[RF_UI_SPECTRUM_BINS];
    uint16_t peak_index;

    (void)spectrum_fill_base_rows(pixels, 0U,
                                  RF_SPECTRUM_TEXTURE_HEIGHT);
    spectrum_prepare_geometry(data, point_x, point_y, &peak_index);
    (void)spectrum_draw_trace_segments(pixels, point_x, point_y, 1U,
                                       RF_UI_SPECTRUM_BINS - 1U, NULL);
    (void)spectrum_draw_peak(pixels, point_x, point_y, peak_index);
}

static void prepare_spectrum_image(void)
{
    g_spectrum_active_source = 0U;
    rasterize_spectrum_to(g_spectrum_pixels[g_spectrum_active_source],
                          g_spectrum_data[g_ui.committed_channel]);
    for(uint32_t source = 0U; source < RF_CHANNEL_SOURCE_COUNT; ++source) {
        lv_image_dsc_t * const descriptor = &g_spectrum_image_dsc[source];
        descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor->header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        descriptor->header.w = RF_SPECTRUM_TEXTURE_WIDTH;
        descriptor->header.h = RF_SPECTRUM_TEXTURE_HEIGHT;
        descriptor->header.stride = RF_SPECTRUM_TEXTURE_STRIDE_BYTES;
        descriptor->header.reserved_2 = 0u;
        descriptor->data_size = RF_SPECTRUM_TEXTURE_STRIDE_BYTES *
                                RF_SPECTRUM_TEXTURE_HEIGHT;
        descriptor->data = (const uint8_t *)g_spectrum_pixels[source];
        descriptor->reserved = NULL;
        descriptor->reserved_2 = NULL;
    }
}

static uint32_t interpolate_waterfall_color(uint8_t value)
{
    static const uint8_t stops[6] = {0u, 45u, 92u, 145u, 205u, 255u};
    static const uint8_t colors[6][3] = {
        {2u, 4u, 5u},
        {6u, 58u, 91u},
        {8u, 127u, 146u},
        {53u, 185u, 110u},
        {244u, 202u, 74u},
        {241u, 91u, 70u},
    };

    if(value >= stops[5]) {
        return ((uint32_t) colors[5][0] << 16) |
               ((uint32_t) colors[5][1] << 8) | colors[5][2];
    }

    uint32_t upper = 1u;
    while(upper < 6u && value > stops[upper]) ++upper;
    if(upper >= 6u) upper = 5u;
    const uint32_t lower = upper - 1u;
    const uint32_t range = (uint32_t) stops[upper] - stops[lower];
    const uint32_t offset = (uint32_t) value - stops[lower];
    uint32_t component[3];

    for(uint32_t index = 0; index < 3u; ++index) {
        const int32_t start = colors[lower][index];
        const int32_t delta = (int32_t) colors[upper][index] - start;
        component[index] = (uint32_t) (start +
                           (delta * (int32_t) offset) / (int32_t) range);
    }
    return (component[0] << 16) | (component[1] << 8) | component[2];
}

static uint16_t waterfall_pixel(uint8_t intensity)
{
    if(g_waterfall_lookup_ready) return g_waterfall_color_lut[intensity];
    return rgb565(interpolate_waterfall_color(intensity));
}

static uint8_t waterfall_clut_heat_index(uint8_t intensity)
{
    return (uint8_t)(RF_WATERFALL_CLUT_HEAT_FIRST +
        (((uint32_t)intensity *
          (RF_WATERFALL_CLUT_HEAT_LAST - RF_WATERFALL_CLUT_HEAT_FIRST) +
          127U) / 255U));
}

static void waterfall_clut_map_insert(uint16_t rgb565_pixel,
                                      uint8_t clut_index)
{
    uint32_t position = 0U;
    while(position < g_waterfall_clut_rgb565_count &&
          g_waterfall_clut_rgb565_keys[position] < rgb565_pixel) {
        position++;
    }
    if(position < g_waterfall_clut_rgb565_count &&
       g_waterfall_clut_rgb565_keys[position] == rgb565_pixel) {
        g_waterfall_clut_rgb565_values[position] = clut_index;
        return;
    }
    for(uint32_t index = g_waterfall_clut_rgb565_count;
        index > position; --index) {
        g_waterfall_clut_rgb565_keys[index] =
            g_waterfall_clut_rgb565_keys[index - 1U];
        g_waterfall_clut_rgb565_values[index] =
            g_waterfall_clut_rgb565_values[index - 1U];
    }
    g_waterfall_clut_rgb565_keys[position] = rgb565_pixel;
    g_waterfall_clut_rgb565_values[position] = clut_index;
    g_waterfall_clut_rgb565_count++;
}

static uint8_t waterfall_clut_from_rgb565(uint16_t rgb565_pixel)
{
    uint32_t first = 0U;
    uint32_t count = g_waterfall_clut_rgb565_count;
    while(count != 0U) {
        const uint32_t step = count / 2U;
        const uint32_t middle = first + step;
        if(g_waterfall_clut_rgb565_keys[middle] < rgb565_pixel) {
            first = middle + 1U;
            count -= step + 1U;
        }
        else {
            count = step;
        }
    }
    if(first < g_waterfall_clut_rgb565_count &&
       g_waterfall_clut_rgb565_keys[first] == rgb565_pixel) {
        return g_waterfall_clut_rgb565_values[first];
    }
    return RF_WATERFALL_CLUT_HEAT_FIRST;
}

static void waterfall_clut4_pixel_set(uint8_t * row,
                                      uint32_t pixel_x,
                                      uint8_t clut_index)
{
    const uint32_t byte_x = pixel_x >> 1;
    const uint8_t index = (uint8_t)(clut_index & 0x0FU);
    const uint8_t old_value = row[byte_x];
    if((pixel_x & 1U) == 0U) {
        row[byte_x] = (uint8_t)((old_value & 0xF0U) | index);
    }
    else {
        row[byte_x] = (uint8_t)((old_value & 0x0FU) | (index << 4));
    }
}

static void waterfall_clut4_run_fill(uint8_t * row,
                                     uint32_t pixel_x,
                                     uint32_t pixel_count,
                                     uint8_t clut_index)
{
    if(pixel_count == 0U) return;
    const uint8_t index = (uint8_t)(clut_index & 0x0FU);
    if((pixel_x & 1U) != 0U) {
        waterfall_clut4_pixel_set(row, pixel_x, index);
        pixel_x++;
        pixel_count--;
    }
    const uint32_t byte_count = pixel_count >> 1;
    if(byte_count != 0U) {
        memset(&row[pixel_x >> 1], (uint8_t)(index | (index << 4)),
               byte_count);
        pixel_x += byte_count * 2U;
        pixel_count -= byte_count * 2U;
    }
    if(pixel_count != 0U) {
        waterfall_clut4_pixel_set(row, pixel_x, index);
    }
}

static uint32_t waterfall_clut4_phase_pixel(uint32_t logical_pixel,
                                            uint32_t phase)
{
    const uint32_t phase_offset =
        phase * RF_WATERFALL_CLUT_PHASE_OFFSET_PIXELS;
    return (logical_pixel + RF_WATERFALL_CLUT_RING_WIDTH - phase_offset) %
           RF_WATERFALL_CLUT_RING_WIDTH;
}

static void waterfall_clut4_phase_run_fill(uint8_t * row,
                                           uint32_t phase,
                                           uint32_t logical_pixel,
                                           uint32_t pixel_count,
                                           uint8_t clut_index,
                                           bool mirror)
{
    uint32_t physical_pixel =
        waterfall_clut4_phase_pixel(logical_pixel, phase);
    while(pixel_count != 0U) {
        uint32_t span = RF_WATERFALL_CLUT_RING_WIDTH - physical_pixel;
        if(span > pixel_count) span = pixel_count;
        waterfall_clut4_run_fill(row, physical_pixel, span, clut_index);
        if(mirror) {
            waterfall_clut4_run_fill(
                row, physical_pixel + RF_WATERFALL_CLUT_RING_WIDTH,
                span, clut_index);
        }
        pixel_count -= span;
        physical_pixel = 0U;
    }
}

static void prepare_waterfall_lookup_tables(void)
{
    if(g_waterfall_lookup_ready) return;

    for(uint32_t intensity = 0U; intensity < 256U; ++intensity) {
        const uint32_t rgb =
            interpolate_waterfall_color((uint8_t) intensity);
        const uint8_t clut_index =
            waterfall_clut_heat_index((uint8_t)intensity);
        g_waterfall_color_lut[intensity] = rgb565(rgb);
        g_waterfall_clut_lut[intensity] = clut_index;
    }
    memset(g_waterfall_clut_palette, 0,
           sizeof(g_waterfall_clut_palette));
    for(uint32_t index = RF_WATERFALL_CLUT_HEAT_FIRST;
        index <= RF_WATERFALL_CLUT_HEAT_LAST; ++index) {
        const uint32_t intensity =
            ((index - RF_WATERFALL_CLUT_HEAT_FIRST) * 255U +
             ((RF_WATERFALL_CLUT_HEAT_LAST -
               RF_WATERFALL_CLUT_HEAT_FIRST) / 2U)) /
            (RF_WATERFALL_CLUT_HEAT_LAST - RF_WATERFALL_CLUT_HEAT_FIRST);
        g_waterfall_clut_palette[index] =
            0xFF000000U | interpolate_waterfall_color((uint8_t)intensity);
    }
    for(uint32_t index = 0U; index < RF_UI_DETECTION_COUNT; ++index) {
        g_waterfall_clut_palette[RF_WATERFALL_CLUT_BOX_FIRST + index] =
            0xFF000000U | g_target_accent_colors[index];
    }
    g_waterfall_clut_palette[RF_WATERFALL_CLUT_GAP_A] =
        0xFF000000U | RF_COLOR_GAP_A;
    g_waterfall_clut_palette[RF_WATERFALL_CLUT_GAP_B] =
        0xFF000000U | RF_COLOR_GAP_B;
    g_waterfall_clut_rgb565_count = 0U;
    for(uint32_t intensity = 0U; intensity < 256U; ++intensity) {
        waterfall_clut_map_insert(g_waterfall_color_lut[intensity],
                                  g_waterfall_clut_lut[intensity]);
    }
    for(uint32_t index = 0U; index < RF_UI_DETECTION_COUNT; ++index) {
        waterfall_clut_map_insert(
            rgb565(g_target_accent_colors[index]),
            (uint8_t)(RF_WATERFALL_CLUT_BOX_FIRST + index));
    }
    waterfall_clut_map_insert(rgb565(RF_COLOR_GAP_A),
                              RF_WATERFALL_CLUT_GAP_A);
    waterfall_clut_map_insert(rgb565(RF_COLOR_GAP_B),
                              RF_WATERFALL_CLUT_GAP_B);
    /* Preserve the source contract: input bins are low-to-high and screen
     * rows are high-to-low. The table also keeps the generic endpoint map. */
    for(uint32_t display_row = 0U;
        display_row < RF_UI_WATERFALL_FREQ_BINS;
        ++display_row) {
        const uint32_t ascending_row =
            RF_UI_WATERFALL_FREQ_BINS - 1U - display_row;
        g_waterfall_source_bin_fast[display_row] = (uint8_t)
            ((ascending_row * (RF_WATERFALL_FAST_FREQ_BINS - 1U)) /
             (RF_UI_WATERFALL_FREQ_BINS - 1U));
    }
    /* The viewport is expanded once into native screen pixels. D/AVE2D then
     * performs an unscaled blit, so neither time nor frequency is filtered. */
    for(uint32_t column = 0U; column <= RF_UI_WATERFALL_COLS; ++column) {
        g_waterfall_render_x[column] = (uint16_t)
            ((column * RF_WATERFALL_DISPLAY_WIDTH) /
             RF_UI_WATERFALL_COLS);
    }
    for(uint32_t row = 0U; row <= RF_UI_WATERFALL_FREQ_BINS; ++row) {
        g_waterfall_render_y[row] = (uint16_t)
            ((row * RF_WATERFALL_DISPLAY_HEIGHT) /
             RF_UI_WATERFALL_FREQ_BINS);
    }
    for(uint32_t render_y = 0U;
        render_y < RF_WATERFALL_DISPLAY_HEIGHT; ++render_y) {
        uint32_t source_row = 0U;
        while((source_row + 1U) < RF_UI_WATERFALL_FREQ_BINS &&
              render_y >= g_waterfall_render_y[source_row + 1U]) {
            source_row++;
        }
        g_waterfall_render_source_row[render_y] = (uint8_t)source_row;
    }
    g_waterfall_lookup_ready = true;
}

static void waterfall_overlay_history_pixel_write(uint8_t source,
                                                   uint32_t render_y,
                                                   uint32_t history_column,
                                                   uint8_t clut_index)
{
    const uint32_t pixel_x =
        history_column * RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    for(uint32_t phase = 0U;
        phase < RF_WATERFALL_CLUT_PHASE_COUNT; ++phase) {
        uint8_t * const row =
            g_waterfall_render_rings[source].clut4.phase[phase]
                                                     .rows[render_y];
        waterfall_clut4_phase_run_fill(
            row, phase, pixel_x, RF_WATERFALL_CLUT_PIXELS_PER_COLUMN,
            clut_index, true);
    }
}

static void waterfall_overlay_build_row(uint8_t source,
                                        uint32_t channel,
                                        uint32_t render_y,
                                        uint32_t source_row)
{
    const uint16_t * const history =
        g_waterfall_rings[channel].rows[source_row];
    for(uint32_t phase = 0U;
        phase < RF_WATERFALL_CLUT_PHASE_COUNT; ++phase) {
        uint8_t * const row =
            g_waterfall_render_rings[source].clut4.phase[phase]
                                                     .rows[render_y];
        for(uint32_t history_column = 0U;
            history_column < RF_UI_WATERFALL_HISTORY_COLS;
            ++history_column) {
            waterfall_clut4_phase_run_fill(
                row, phase,
                history_column * RF_WATERFALL_CLUT_PIXELS_PER_COLUMN,
                RF_WATERFALL_CLUT_PIXELS_PER_COLUMN,
                waterfall_clut_from_rgb565(history[history_column]), false);
        }
        memcpy(&row[RF_WATERFALL_CLUT_RING_BYTES], row,
               RF_WATERFALL_CLUT_RING_BYTES);
    }
}

static uint32_t waterfall_overlay_catchup_row(uint8_t source,
                                              uint32_t channel,
                                              uint32_t render_y,
                                              uint32_t source_row,
                                              uint32_t source_head,
                                              uint32_t column_count)
{
    const uint16_t * const history =
        g_waterfall_rings[channel].rows[source_row];
    for(uint32_t offset = 0U; offset < column_count; ++offset) {
        const uint32_t history_column =
            (source_head + offset) % RF_UI_WATERFALL_HISTORY_COLS;
        waterfall_overlay_history_pixel_write(
            source, render_y, history_column,
            waterfall_clut_from_rgb565(history[history_column]));
    }
    return column_count * RF_WATERFALL_CLUT_COLUMN_WRITE_BYTES;
}

typedef struct {
    uint64_t start_column;
    uint64_t end_column;
    uint16_t top_row;
    uint16_t bottom_row;
} rf_ui_waterfall_box_bounds_t;

static bool waterfall_box_history_bounds(
    uint64_t anchor_end_column,
    const rf_ui_rf_box_t * box,
    rf_ui_waterfall_box_bounds_t * bounds)
{
    if(box == NULL || bounds == NULL) return false;

    uint32_t frequency_end = (uint32_t)box->frequency_start_q8 +
                             box->frequency_span_q8;
    uint32_t time_end = (uint32_t)box->time_start_q8 + box->time_span_q8;
    if((box->flags & RF_UI_RF_BOX_FLAG_VALID) == 0U ||
       box->frequency_span_q8 == 0U || box->time_span_q8 == 0U ||
       box->detection_index >= RF_UI_DETECTION_COUNT) return false;
    if(frequency_end > RF_UI_RF_COORD_SCALE) {
        frequency_end = RF_UI_RF_COORD_SCALE;
    }
    if(time_end > RF_UI_RF_COORD_SCALE) time_end = RF_UI_RF_COORD_SCALE;

    const uint32_t start_column_offset =
        ((uint32_t)box->time_start_q8 * RF_WATERFALL_RF_ROWS_PER_WINDOW) /
        RF_UI_RF_COORD_SCALE;
    uint32_t end_column_offset =
        (time_end * RF_WATERFALL_RF_ROWS_PER_WINDOW +
         RF_UI_RF_COORD_SCALE - 1U) / RF_UI_RF_COORD_SCALE;
    if(end_column_offset <= start_column_offset) {
        end_column_offset = start_column_offset + 1U;
    }
    if(end_column_offset > RF_WATERFALL_RF_ROWS_PER_WINDOW) {
        end_column_offset = RF_WATERFALL_RF_ROWS_PER_WINDOW;
    }

    const uint64_t window_start =
        anchor_end_column >= RF_WATERFALL_RF_ROWS_PER_WINDOW ?
        anchor_end_column - RF_WATERFALL_RF_ROWS_PER_WINDOW : 0U;
    uint64_t absolute_end = window_start + end_column_offset;
    if(absolute_end > anchor_end_column) absolute_end = anchor_end_column;
    const uint64_t absolute_start = window_start + start_column_offset;
    if(absolute_end <= absolute_start) return false;

    const uint32_t top =
        ((RF_UI_RF_COORD_SCALE - frequency_end) *
         RF_UI_WATERFALL_FREQ_BINS) / RF_UI_RF_COORD_SCALE;
    uint32_t bottom = scale_ceil_u32(
        RF_UI_RF_COORD_SCALE - box->frequency_start_q8,
        RF_UI_WATERFALL_FREQ_BINS);
    if(bottom <= top) bottom = top + 1U;
    if(bottom > RF_UI_WATERFALL_FREQ_BINS) {
        bottom = RF_UI_WATERFALL_FREQ_BINS;
    }
    if(top >= bottom) return false;

    bounds->start_column = absolute_start;
    bounds->end_column = absolute_end;
    bounds->top_row = (uint16_t)top;
    bounds->bottom_row = (uint16_t)bottom;
    return true;
}

static bool waterfall_box_border_cell(
    const rf_ui_waterfall_box_bounds_t * bounds,
    uint32_t source_row,
    uint64_t absolute_column)
{
    const uint32_t row_count = bounds->bottom_row - bounds->top_row;
    const uint32_t horizontal_border = row_count >= 4U ? 2U : 1U;
    return source_row < bounds->top_row + horizontal_border ||
           source_row + horizontal_border >= bounds->bottom_row ||
           absolute_column == bounds->start_column ||
           absolute_column + 1U == bounds->end_column;
}

static uint32_t waterfall_history_box_raster(
    uint32_t channel,
    uint64_t anchor_end_column,
    const rf_ui_rf_box_t * box)
{
    rf_ui_waterfall_box_bounds_t bounds;
    if(channel >= RF_UI_CHANNEL_COUNT ||
       !waterfall_box_history_bounds(anchor_end_column, box, &bounds)) {
        return 0U;
    }

    const uint16_t pixel =
        rgb565(g_target_accent_colors[box->detection_index]);
    uint32_t bytes_written = 0U;
    for(uint32_t source_row = bounds.top_row;
        source_row < bounds.bottom_row; ++source_row) {
        for(uint64_t absolute_column = bounds.start_column;
            absolute_column < bounds.end_column; ++absolute_column) {
            if(!waterfall_box_border_cell(
                   &bounds, source_row, absolute_column)) continue;
            const uint32_t history_column = (uint32_t)(
                absolute_column % RF_UI_WATERFALL_HISTORY_COLS);
            g_waterfall_rings[channel].rows[source_row][history_column] =
                pixel;
            g_waterfall_rings[channel].rows[source_row]
                [history_column + RF_UI_WATERFALL_HISTORY_COLS] = pixel;
            bytes_written += 2U * sizeof(pixel);
        }
    }
    return bytes_written;
}

static void waterfall_overlay_pixel_set(uint8_t source,
                                        uint32_t render_y,
                                        uint64_t absolute_pixel,
                                        uint8_t clut_index)
{
    const uint32_t pixel_x =
        (uint32_t)(absolute_pixel % RF_WATERFALL_CLUT_RING_WIDTH);
    for(uint32_t phase = 0U;
        phase < RF_WATERFALL_CLUT_PHASE_COUNT; ++phase) {
        const uint32_t physical_pixel =
            waterfall_clut4_phase_pixel(pixel_x, phase);
        uint8_t * const row =
            g_waterfall_render_rings[source].clut4.phase[phase]
                                                     .rows[render_y];
        waterfall_clut4_pixel_set(row, physical_pixel, clut_index);
        waterfall_clut4_pixel_set(
            row, physical_pixel + RF_WATERFALL_CLUT_RING_WIDTH,
            clut_index);
    }
}

static void waterfall_overlay_box_raster(uint8_t source,
                                         uint64_t anchor_end_column,
                                         const rf_ui_rf_box_t * box)
{
    uint32_t frequency_end = (uint32_t)box->frequency_start_q8 +
                             box->frequency_span_q8;
    uint32_t time_end = (uint32_t)box->time_start_q8 + box->time_span_q8;
    if((box->flags & RF_UI_RF_BOX_FLAG_VALID) == 0U ||
       box->frequency_span_q8 == 0U || box->time_span_q8 == 0U ||
       box->detection_index >= RF_UI_DETECTION_COUNT) return;
    if(frequency_end > RF_UI_RF_COORD_SCALE) {
        frequency_end = RF_UI_RF_COORD_SCALE;
    }
    if(time_end > RF_UI_RF_COORD_SCALE) time_end = RF_UI_RF_COORD_SCALE;

    const uint32_t start_column_offset =
        ((uint32_t)box->time_start_q8 * RF_WATERFALL_RF_ROWS_PER_WINDOW) /
        RF_UI_RF_COORD_SCALE;
    uint32_t end_column_offset =
        (time_end * RF_WATERFALL_RF_ROWS_PER_WINDOW +
         RF_UI_RF_COORD_SCALE - 1U) / RF_UI_RF_COORD_SCALE;
    if(end_column_offset <= start_column_offset) {
        end_column_offset = start_column_offset + 1U;
    }
    if(end_column_offset > RF_WATERFALL_RF_ROWS_PER_WINDOW) {
        end_column_offset = RF_WATERFALL_RF_ROWS_PER_WINDOW;
    }
    const uint64_t window_start =
        anchor_end_column >= RF_WATERFALL_RF_ROWS_PER_WINDOW ?
        anchor_end_column - RF_WATERFALL_RF_ROWS_PER_WINDOW : 0U;
    const uint64_t start_pixel =
        (window_start + start_column_offset) *
        RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    const uint64_t end_pixel =
        (window_start + end_column_offset) *
        RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    if(end_pixel <= start_pixel) return;

    const uint32_t top =
        ((RF_UI_RF_COORD_SCALE - frequency_end) *
         RF_WATERFALL_DISPLAY_HEIGHT) / RF_UI_RF_COORD_SCALE;
    uint32_t bottom = scale_ceil_u32(
        RF_UI_RF_COORD_SCALE - box->frequency_start_q8,
        RF_WATERFALL_DISPLAY_HEIGHT);
    if(bottom <= top) bottom = top + 2U;
    if(bottom > RF_WATERFALL_DISPLAY_HEIGHT) {
        bottom = RF_WATERFALL_DISPLAY_HEIGHT;
    }
    const uint8_t clut_index = (uint8_t)(
        RF_WATERFALL_CLUT_BOX_FIRST + box->detection_index);

    for(uint64_t pixel = start_pixel; pixel < end_pixel; ++pixel) {
        for(uint32_t border = 0U; border < 2U; ++border) {
            if((top + border) < bottom) {
                waterfall_overlay_pixel_set(
                    source, top + border, pixel, clut_index);
            }
            if(bottom > border + top) {
                const uint32_t y = bottom - 1U - border;
                waterfall_overlay_pixel_set(source, y, pixel, clut_index);
            }
        }
    }
    for(uint32_t y = top; y < bottom; ++y) {
        for(uint32_t border = 0U; border < 2U; ++border) {
            const uint64_t left = start_pixel + border;
            const uint64_t right = end_pixel - 1U - border;
            waterfall_overlay_pixel_set(source, y, left, clut_index);
            waterfall_overlay_pixel_set(source, y, right, clut_index);
        }
    }
}

static void waterfall_overlay_box_raster_matching_sources(
    uint32_t channel,
    uint64_t anchor_end_column,
    const rf_ui_rf_box_t * box)
{
    if(channel >= RF_UI_CHANNEL_COUNT ||
       !g_waterfall_overlay.requested || g_waterfall_overlay.failed) return;

    int32_t selected_source = -1;
    if(g_waterfall_active_source < RF_CHANNEL_SOURCE_COUNT &&
       g_waterfall_source_state[g_waterfall_active_source].valid &&
       g_waterfall_source_state[g_waterfall_active_source].channel == channel) {
        selected_source = g_waterfall_active_source;
    }
    if(selected_source < 0 &&
       g_channel_build.state >= RF_UI_CHANNEL_SWITCH_SPECTRUM_BASE &&
       g_channel_build.state <= RF_UI_CHANNEL_SWITCH_WATERFALL_RENDER &&
       g_channel_build.channel == channel &&
       g_channel_build.source < RF_CHANNEL_SOURCE_COUNT) {
        selected_source = g_channel_build.source;
    }
    if(selected_source < 0 &&
       g_live_build.state != RF_UI_LIVE_BUILD_IDLE &&
       g_live_build.channel == channel &&
       g_live_build.source < RF_CHANNEL_SOURCE_COUNT) {
        selected_source = g_live_build.source;
    }
    if(selected_source < 0 && g_waterfall_overlay_sync.active &&
       g_waterfall_overlay_sync.channel == channel &&
       g_waterfall_overlay_sync.source < RF_CHANNEL_SOURCE_COUNT) {
        selected_source = g_waterfall_overlay_sync.source;
    }
    for(uint32_t source = 0U; source < RF_CHANNEL_SOURCE_COUNT; ++source) {
        if(selected_source < 0 &&
           g_waterfall_source_state[source].valid &&
           g_waterfall_source_state[source].channel == channel) {
            selected_source = (int32_t)source;
        }
        else if((int32_t)source != selected_source &&
                g_waterfall_source_state[source].valid &&
                g_waterfall_source_state[source].channel == channel) {
            /* One source update plus the mirrored history stays below the
             * 32 KiB UI write budget. A stale cached peer rebuilds later. */
            g_waterfall_source_state[source].valid = false;
        }
    }
    if(g_live_build.state != RF_UI_LIVE_BUILD_IDLE &&
       g_live_build.channel == channel &&
       (int32_t)g_live_build.source != selected_source) {
        live_build_cancel(true);
    }
    if(selected_source < 0) return;

    waterfall_overlay_box_raster(
        (uint8_t)selected_source, anchor_end_column, box);
    if((uint32_t)selected_source == g_waterfall_active_source ||
       (uint32_t)selected_source == g_waterfall_overlay.display_source) {
        g_waterfall_overlay.visual_dirty = true;
    }
}

static void waterfall_overlay_boxes_refresh(uint8_t source)
{
    if(source >= RF_CHANNEL_SOURCE_COUNT ||
       !g_waterfall_source_state[source].valid) return;
    /* RF boxes live in the RGB565 history itself. Base and catch-up builders
     * reproduce every retained box, so an older stamp must never be erased. */
    g_waterfall_overlay.boxes_dirty[source] = false;
    g_waterfall_overlay.visual_dirty = true;
    g_rf_ui_channel_switch_diag.overlay_box_refreshes++;
}

static void waterfall_image_head_set(uint8_t source, uint16_t render_column)
{
    if(source >= RF_CHANNEL_SOURCE_COUNT ||
       render_column >= RF_UI_WATERFALL_COLS) return;
    g_waterfall_image_dsc[source].data = (const uint8_t *)
        &g_waterfall_render_rings[source].rgb565.rows[0]
            [g_waterfall_render_x[render_column]];
}

static bool waterfall_image_source_rebind(uint8_t source)
{
    if(source >= RF_CHANNEL_SOURCE_COUNT || g_ui.waterfall_image == NULL) {
        return false;
    }
    const lv_result_t result = lv_image_rebind_src(
        g_ui.waterfall_image, &g_waterfall_image_dsc[source]);
    if(result != LV_RESULT_OK) {
        g_rf_ui_channel_switch_diag.source_rebind_failures++;
        return false;
    }
    g_rf_ui_channel_switch_diag.waterfall_source_rebinds++;
    g_rf_ui_channel_switch_diag.last_waterfall_descriptor =
        (uint32_t)(uintptr_t)&g_waterfall_image_dsc[source];
    g_rf_ui_channel_switch_diag.last_waterfall_data =
        (uint32_t)(uintptr_t)g_waterfall_image_dsc[source].data;
    g_rf_ui_channel_switch_diag.last_waterfall_source = source;
    g_rf_ui_channel_switch_diag.last_waterfall_render_column =
        g_waterfall_source_state[source].render_write_column;
    return true;
}

static bool spectrum_image_source_rebind(uint8_t source)
{
    if(source >= RF_CHANNEL_SOURCE_COUNT || g_ui.spectrum_image == NULL) {
        return false;
    }
    if(lv_image_rebind_src(g_ui.spectrum_image,
                           &g_spectrum_image_dsc[source]) != LV_RESULT_OK) {
        g_rf_ui_channel_switch_diag.source_rebind_failures++;
        return false;
    }
    g_rf_ui_channel_switch_diag.spectrum_source_rebinds++;
    return true;
}

static void waterfall_image_head_update(void)
{
    /* The head is the expanded x coordinate g_waterfall_render_x[
     * g_waterfall_render_write_column]; keep this operation centralized so
     * background builders can never alter the displayed source in place. */
    waterfall_image_head_set(g_waterfall_active_source,
                             g_waterfall_render_write_column);
}

static void waterfall_source_state_commit(uint8_t source,
                                          uint32_t channel,
                                          uint64_t total_columns,
                                          uint16_t history_head,
                                          uint16_t render_write_column)
{
    if(source >= RF_CHANNEL_SOURCE_COUNT ||
       channel >= RF_UI_CHANNEL_COUNT) return;
    g_waterfall_source_state[source].valid = true;
    g_waterfall_source_state[source].channel = (uint8_t)channel;
    g_waterfall_source_state[source].total_columns = total_columns;
    g_waterfall_source_state[source].history_head = history_head;
    g_waterfall_source_state[source].render_write_column =
        render_write_column;
}

static void waterfall_source_state_invalidate(uint8_t source)
{
    if(source < RF_CHANNEL_SOURCE_COUNT) {
        g_waterfall_source_state[source].valid = false;
    }
}

static void waterfall_source_state_invalidate_channel(uint32_t channel)
{
    for(uint32_t source = 0U; source < RF_CHANNEL_SOURCE_COUNT; ++source) {
        if(g_waterfall_source_state[source].valid &&
           g_waterfall_source_state[source].channel == channel) {
            g_waterfall_source_state[source].valid = false;
        }
    }
}

static void invalidate_image_area_rows(lv_obj_t * image,
                                       uint32_t row_start,
                                       uint32_t row_count)
{
    if(image == NULL || row_count == 0U) return;
    lv_area_t coords;
    lv_obj_get_coords(image, &coords);
    const uint32_t height = (uint32_t)lv_area_get_height(&coords);
    if(row_start >= height) return;
    if(row_count > (height - row_start)) row_count = height - row_start;
    lv_area_t area = coords;
    area.y1 += (int32_t)row_start;
    area.y2 = area.y1 + (int32_t)row_count - 1;
    lv_obj_invalidate_area(image, &area);
    if(image == g_ui.waterfall_image) {
        g_rf_ui_channel_switch_diag.waterfall_invalidations++;
        g_rf_ui_channel_switch_diag.waterfall_invalidated_rows += row_count;
    }
    else if(image == g_ui.spectrum_image) {
        g_rf_ui_channel_switch_diag.spectrum_invalidations++;
        g_rf_ui_channel_switch_diag.spectrum_invalidated_rows += row_count;
    }
}

static void waterfall_render_clear(uint16_t pixel)
{
    for(uint32_t frequency_row = 0; frequency_row < RF_WATERFALL_DISPLAY_HEIGHT;
        ++frequency_row) {
        fill_row(g_waterfall_render_rings[g_waterfall_active_source]
                     .rgb565.rows[frequency_row], pixel,
                 RF_WATERFALL_RENDER_STORAGE_WIDTH);
    }
    g_waterfall_source_state[g_waterfall_active_source].valid = false;
    g_waterfall_render_write_column = 0U;
    waterfall_image_head_update();
}

static void waterfall_render_live_row(rf_ui_waterfall_rgb565_ring_t * target,
                                      uint32_t channel,
                                      uint32_t logical_start,
                                      uint32_t render_y,
                                      uint32_t source_row)
{
    const uint16_t * logical_row = g_waterfall_rings[channel].rows[source_row];
    uint16_t * render_row = target->rows[render_y];
    for(uint32_t column = 0; column < RF_UI_WATERFALL_COLS; ++column) {
        const uint32_t render_start = g_waterfall_render_x[column];
        const uint32_t render_width =
            (uint32_t)g_waterfall_render_x[column + 1U] - render_start;
        fill_row(&render_row[render_start],
                 logical_row[logical_start + column], render_width);
    }
    memcpy(&render_row[RF_WATERFALL_DISPLAY_WIDTH], render_row,
           RF_WATERFALL_DISPLAY_WIDTH * sizeof(uint16_t));
}

static void waterfall_render_bootstrap_live(uint32_t channel)
{
    const uint32_t logical_start =
        (g_waterfall_write_head[channel] + RF_UI_WATERFALL_HISTORY_COLS -
         RF_UI_WATERFALL_COLS) % RF_UI_WATERFALL_HISTORY_COLS;
    rf_ui_waterfall_rgb565_ring_t * const target =
        &g_waterfall_render_rings[g_waterfall_active_source].rgb565;

    for(uint32_t source_row = 0; source_row < RF_UI_WATERFALL_FREQ_BINS;
        ++source_row) {
        for(uint32_t render_y = g_waterfall_render_y[source_row];
            render_y < g_waterfall_render_y[source_row + 1U]; ++render_y) {
            waterfall_render_live_row(target, channel, logical_start,
                                      render_y, source_row);
        }
    }
    g_waterfall_render_write_column = 0U;
    waterfall_source_state_commit(
        g_waterfall_active_source, channel,
        g_waterfall_total_columns[channel],
        g_waterfall_write_head[channel], 0U);
    waterfall_image_head_update();
}

static void waterfall_overlay_source_bootstrap(uint8_t source,
                                               uint32_t channel)
{
    for(uint32_t render_y = 0U;
        render_y < RF_WATERFALL_DISPLAY_HEIGHT; ++render_y) {
        waterfall_overlay_build_row(
            source, channel, render_y,
            g_waterfall_render_source_row[render_y]);
    }
    waterfall_source_state_commit(
        source, channel, g_waterfall_total_columns[channel],
        g_waterfall_write_head[channel], 0U);
    g_waterfall_overlay.boxes_dirty[source] = true;
    waterfall_overlay_boxes_refresh(source);
}

static void waterfall_pause_snapshot_capture(uint32_t channel)
{
    const uint32_t logical_head = g_waterfall_write_head[channel];
    for(uint32_t row = 0U; row < RF_UI_WATERFALL_FREQ_BINS; ++row) {
        memcpy(g_waterfall_pause_snapshot[row],
               &g_waterfall_rings[channel].rows[row][logical_head],
               RF_UI_WATERFALL_HISTORY_COLS * sizeof(uint16_t));
    }
    g_rf_box_pause_snapshot = g_rf_box_batches[channel];
    g_waterfall_pause_total_columns = g_waterfall_total_columns[channel];
}

static void waterfall_render_rebuild_paused(void)
{
    const uint32_t maximum_pan =
        RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS;
    const uint32_t pan = (g_ui.waterfall_pan_columns > maximum_pan) ?
                         maximum_pan : g_ui.waterfall_pan_columns;
    const uint32_t snapshot_start = maximum_pan - pan;

    for(uint32_t source_row = 0; source_row < RF_UI_WATERFALL_FREQ_BINS;
        ++source_row) {
        const uint16_t * snapshot_row = g_waterfall_pause_snapshot[source_row];
        for(uint32_t render_y = g_waterfall_render_y[source_row];
            render_y < g_waterfall_render_y[source_row + 1U]; ++render_y) {
            uint16_t * render_row =
                g_waterfall_render_rings[g_waterfall_active_source]
                    .rgb565.rows[render_y];
            for(uint32_t column = 0; column < RF_UI_WATERFALL_COLS; ++column) {
                const uint32_t render_start = g_waterfall_render_x[column];
                const uint32_t render_width =
                    (uint32_t)g_waterfall_render_x[column + 1U] - render_start;
                fill_row(&render_row[render_start],
                         snapshot_row[snapshot_start + column], render_width);
            }
            memcpy(&render_row[RF_WATERFALL_DISPLAY_WIDTH], render_row,
                   RF_WATERFALL_DISPLAY_WIDTH * sizeof(uint16_t));
        }
    }
    g_waterfall_render_write_column = 0U;
    waterfall_image_head_update();
}

static void waterfall_clear_channel(uint32_t channel)
{
    const uint16_t background = waterfall_pixel(0U);
    waterfall_source_state_invalidate_channel(channel);
    rf_box_window_anchors_clear_channel(channel);
    for(uint32_t frequency_row = 0; frequency_row < RF_UI_WATERFALL_FREQ_BINS;
        ++frequency_row) {
        fill_row(g_waterfall_rings[channel].rows[frequency_row], background,
                 RF_UI_WATERFALL_STORAGE_COLS);
    }
    g_waterfall_write_head[channel] = 0U;
    g_waterfall_total_columns[channel] = 0U;
    memset(&g_rf_box_batches[channel], 0,
           sizeof(g_rf_box_batches[channel]));
    memset(&g_spectrum_rf_box_batches[channel], 0,
           sizeof(g_spectrum_rf_box_batches[channel]));
    if((channel == g_ui.committed_channel) && g_ui.running &&
       (!g_waterfall_overlay.requested || g_waterfall_overlay.failed)) {
        waterfall_render_clear(background);
    }
    else if(channel == g_ui.committed_channel) {
        g_waterfall_overlay.visual_dirty = true;
    }
    g_ui.waterfall_dirty[channel] =
        (channel == g_ui.committed_channel) && !g_ui.running;
}

static void prepare_waterfall_images(void)
{
    for(uint32_t channel = 0; channel < RF_DEMO_CHANNEL_COUNT; ++channel) {
        /* Production UI starts blank and is populated only from IPC tiles. */
        waterfall_clear_channel(channel);
    }

    for(uint32_t source = 0U; source < RF_CHANNEL_SOURCE_COUNT; ++source) {
        lv_image_dsc_t * const descriptor = &g_waterfall_image_dsc[source];
        descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor->header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        descriptor->header.w = RF_WATERFALL_DISPLAY_WIDTH;
        descriptor->header.h = RF_WATERFALL_DISPLAY_HEIGHT;
        descriptor->header.stride = RF_WATERFALL_RENDER_STRIDE_BYTES;
        descriptor->header.reserved_2 = 0u;
        /* LVGL rejects a variable image whose descriptor has no data, even
         * when the object is transparent while Layer 2 owns the pixels. */
        descriptor->data = (const uint8_t *)
            &g_waterfall_render_rings[source].rgb565.rows[0][0];
        descriptor->data_size = RF_WATERFALL_DISPLAY_HEIGHT *
                                RF_WATERFALL_RENDER_STRIDE_BYTES;
        descriptor->reserved = NULL;
        descriptor->reserved_2 = NULL;
    }
    g_waterfall_active_source = 0U;
    if(g_waterfall_overlay.requested) {
        waterfall_overlay_source_bootstrap(
            g_waterfall_active_source, g_ui.committed_channel);
        g_waterfall_overlay.display_source = g_waterfall_active_source;
        g_waterfall_overlay.presented_end_pixels =
            g_waterfall_total_columns[g_ui.committed_channel] *
            RF_WATERFALL_CLUT_PIXELS_PER_COLUMN & ~UINT64_C(1);
        g_waterfall_overlay.visual_dirty = true;
    }
    else {
        waterfall_render_bootstrap_live(g_ui.committed_channel);
    }
}

static void push_waterfall_column(uint32_t channel, const uint8_t * intensities,
                                  size_t frequency_bin_count)
{
    /* The retired hot path used g_waterfall_render_x[g_waterfall_render_write_column]
     * and computed render_width before calling fill_row(&render_row[render_start], pixel, render_width),
     * while checking g_ui.running.  Rendering now
     * consumes this history from the bounded background builder, so ingestion
     * never touches a display source or performs a large SDRAM write. */
    const uint32_t write_column = g_waterfall_write_head[channel];
    const bool use_fast_bin_map =
        frequency_bin_count == RF_WATERFALL_FAST_FREQ_BINS;
    prepare_waterfall_lookup_tables();
    for(uint32_t display_row = 0; display_row < RF_UI_WATERFALL_FREQ_BINS;
        ++display_row) {
        uint32_t source_bin;
        if(use_fast_bin_map) {
            source_bin = g_waterfall_source_bin_fast[display_row];
        }
        else {
            const uint32_t ascending_row =
                RF_UI_WATERFALL_FREQ_BINS - 1U - display_row;
            source_bin = (frequency_bin_count <= 1U) ? 0U :
                (ascending_row * ((uint32_t)frequency_bin_count - 1U)) /
                (RF_UI_WATERFALL_FREQ_BINS - 1U);
        }
        const uint16_t pixel = waterfall_pixel(intensities[source_bin]);
        g_waterfall_rings[channel].rows[display_row][write_column] = pixel;
        g_waterfall_rings[channel].rows[display_row]
                                 [write_column + RF_UI_WATERFALL_HISTORY_COLS] = pixel;
    }
    g_waterfall_write_head[channel] = (uint16_t)
        ((write_column + 1U) % RF_UI_WATERFALL_HISTORY_COLS);
    g_waterfall_total_columns[channel]++;
    g_ui.waterfall_dirty[channel] = true;
}

static void push_waterfall_gap_column(uint32_t channel)
{
    const uint32_t write_column = g_waterfall_write_head[channel];
    const uint16_t pixel = rgb565((write_column & 1U) != 0U ?
                                  RF_COLOR_GAP_B : RF_COLOR_GAP_A);
    for(uint32_t display_row = 0; display_row < RF_UI_WATERFALL_FREQ_BINS;
        ++display_row) {
        g_waterfall_rings[channel].rows[display_row][write_column] = pixel;
        g_waterfall_rings[channel].rows[display_row]
                                 [write_column + RF_UI_WATERFALL_HISTORY_COLS] = pixel;
    }
    g_waterfall_write_head[channel] = (uint16_t)
        ((write_column + 1U) % RF_UI_WATERFALL_HISTORY_COLS);
    g_waterfall_total_columns[channel]++;
    g_ui.waterfall_dirty[channel] = true;
}

static void format_millihz(char * buffer, size_t buffer_size, const char * name,
                           uint32_t millihz)
{
    snprintf(buffer, buffer_size, "%s %u.%03u", name,
             (unsigned) (millihz / 1000u), (unsigned) (millihz % 1000u));
}

static void format_render_max(char * buffer, size_t buffer_size, uint32_t render_max_us)
{
    snprintf(buffer, buffer_size, "Rmax %u.%03ums",
             (unsigned) (render_max_us / 1000u),
             (unsigned) (render_max_us % 1000u));
}

static void refresh_scan_rate(void)
{
    if(g_ui.scan_rate_label == NULL) return;
    lv_label_set_text_fmt(g_ui.scan_rate_label, "%s %u.%u Hz",
                           g_ui.focus_mode ? "Focus" : "Scan",
                           (unsigned) (g_scan_rate_x10 / 10u),
                           (unsigned) (g_scan_rate_x10 % 10u));
}

static void refresh_waterfall_timing(void)
{
    if(g_ui.waterfall_history_label == NULL) return;

    /* Time is RF acquisition time, not wall-clock tile arrival time.  One
     * pooled row contains 590336 / 16 samples at 60 MS/s (614.93 us), so the
     * 160-column viewport spans 98.3893 ms regardless of transport timing. */
    const uint32_t samples_per_column = RF_WATERFALL_RF_WINDOW_SAMPLES /
                                        RF_WATERFALL_RF_ROWS_PER_WINDOW;
    const uint32_t column_thousandths_ms = (uint32_t)
        ((((uint64_t)samples_per_column * 1000000ULL) +
          (RF_WATERFALL_RF_SAMPLE_RATE_HZ / 2U)) /
         RF_WATERFALL_RF_SAMPLE_RATE_HZ);
    const uint32_t view_hundredths_ms = (uint32_t)
        ((((uint64_t)samples_per_column * RF_UI_WATERFALL_COLS * 100000ULL) +
          (RF_WATERFALL_RF_SAMPLE_RATE_HZ / 2U)) /
         RF_WATERFALL_RF_SAMPLE_RATE_HZ);
    const uint32_t buffer_hundredths_ms = (uint32_t)
        ((((uint64_t)samples_per_column * RF_UI_WATERFALL_HISTORY_COLS *
           100000ULL) + (RF_WATERFALL_RF_SAMPLE_RATE_HZ / 2U)) /
         RF_WATERFALL_RF_SAMPLE_RATE_HZ);
    const uint32_t pan_hundredths_ms = (uint32_t)
        ((((uint64_t)samples_per_column * g_ui.waterfall_pan_columns *
           100000ULL) + (RF_WATERFALL_RF_SAMPLE_RATE_HZ / 2U)) /
         RF_WATERFALL_RF_SAMPLE_RATE_HZ);
    const uint32_t tick_hundredths_ms[5] = {
        pan_hundredths_ms + view_hundredths_ms,
        pan_hundredths_ms + (view_hundredths_ms * 3U) / 4U,
        pan_hundredths_ms + view_hundredths_ms / 2U,
        pan_hundredths_ms + view_hundredths_ms / 4U,
        pan_hundredths_ms,
    };
    uint32_t tick_ms[5];
    for(uint32_t index = 0U; index < 5U; ++index) {
        tick_ms[index] = (tick_hundredths_ms[index] + 50U) / 100U;
    }

    if(g_ui.running) {
        lv_label_set_text_fmt(g_ui.waterfall_history_label,
                              "%u RF 行 | %u.%03u ms/列 | %u.%02u ms",
                              (unsigned)RF_UI_WATERFALL_COLS,
                              (unsigned)(column_thousandths_ms / 1000U),
                              (unsigned)(column_thousandths_ms % 1000U),
                              (unsigned)(view_hundredths_ms / 100U),
                              (unsigned)(view_hundredths_ms % 100U));
        for(uint32_t index = 0U; index < 4U; ++index) {
            lv_label_set_text_fmt(g_ui.waterfall_time_labels[index],
                                  "-%u ms", (unsigned)tick_ms[index]);
        }
        lv_label_set_text(g_ui.waterfall_time_labels[4], "当前");
    }
    else {
        lv_label_set_text_fmt(g_ui.waterfall_history_label,
                              "已保持 | 视窗 %u.%02u ms | 缓存 %u.%02u ms",
                              (unsigned)(view_hundredths_ms / 100U),
                              (unsigned)(view_hundredths_ms % 100U),
                              (unsigned)(buffer_hundredths_ms / 100U),
                              (unsigned)(buffer_hundredths_ms % 100U));
        for(uint32_t index = 0U; index < 5U; ++index) {
            if(index == 4U && tick_ms[index] == 0U) {
                lv_label_set_text(g_ui.waterfall_time_labels[index], "保持点");
            }
            else {
                lv_label_set_text_fmt(g_ui.waterfall_time_labels[index],
                                      "-%u ms", (unsigned)tick_ms[index]);
            }
        }
    }

    if(g_ui.live_label != NULL) {
        lv_label_set_text(g_ui.live_label,
                          g_ui.running ? "实时" :
                          (g_ui.waterfall_pan_columns == 0U ?
                           "暂停" : "回放"));
    }
    if(g_ui.transport_time_label != NULL) {
        if(g_ui.running) {
            lv_label_set_text(g_ui.transport_time_label, "LIVE");
        }
        else if(g_ui.waterfall_pan_columns == 0U) {
            lv_label_set_text(g_ui.transport_time_label, "HOLD");
        }
        else {
            lv_label_set_text_fmt(g_ui.transport_time_label, "-%u ms",
                                  (unsigned)tick_ms[4]);
        }
    }

    if(g_ui.history_slider != NULL) {
        const int32_t maximum_pan = (int32_t)
            (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS);
        const int32_t slider_value = maximum_pan -
                                     (int32_t)g_ui.waterfall_pan_columns;
        g_ui.history_slider_updating = true;
        lv_slider_set_value(g_ui.history_slider, slider_value, LV_ANIM_OFF);
        g_ui.history_slider_updating = false;
    }
    const uint16_t maximum_pan =
        RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS;
    if(g_ui.history_buttons[RF_HISTORY_OLDER] != NULL) {
        if(g_ui.waterfall_pan_columns >= maximum_pan) {
            lv_obj_add_state(g_ui.history_buttons[RF_HISTORY_OLDER],
                             LV_STATE_DISABLED);
        }
        else {
            lv_obj_remove_state(g_ui.history_buttons[RF_HISTORY_OLDER],
                                LV_STATE_DISABLED);
        }
    }
    if(g_ui.history_buttons[RF_HISTORY_NEWER] != NULL) {
        if(g_ui.waterfall_pan_columns == 0U) {
            lv_obj_add_state(g_ui.history_buttons[RF_HISTORY_NEWER],
                             LV_STATE_DISABLED);
        }
        else {
            lv_obj_remove_state(g_ui.history_buttons[RF_HISTORY_NEWER],
                                LV_STATE_DISABLED);
        }
    }
}

static void refresh_live_state(void)
{
    if(g_ui.live_button == NULL) return;
    const uint32_t state_color = g_ui.running ? RF_COLOR_GREEN : RF_COLOR_AMBER;
    if(g_ui.live_dot != NULL) {
        lv_obj_set_style_bg_color(g_ui.live_dot, color(state_color), 0);
    }
    lv_label_set_text(g_ui.live_icon,
                      g_ui.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(g_ui.live_icon, color(state_color), 0);
    lv_obj_set_style_text_color(g_ui.live_label, color(state_color), 0);
    lv_obj_set_style_bg_color(g_ui.live_button,
                              color(g_ui.running ? RF_COLOR_GREEN_SOFT :
                                                   RF_COLOR_AMBER_SOFT), 0);
    if(g_ui.transport != NULL) {
        lv_obj_set_style_border_color(g_ui.transport, color(state_color), 0);
    }
    if(g_ui.transport_time_label != NULL) {
        lv_obj_set_style_text_color(g_ui.transport_time_label,
                                    color(state_color), 0);
    }
    refresh_waterfall_timing();
}

static void waterfall_paused_view_present(void)
{
    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        g_waterfall_overlay.visual_dirty = true;
        g_waterfall_overlay.boxes_dirty[g_waterfall_active_source] = true;
    }
    else {
        waterfall_render_rebuild_paused();
        if(g_ui.waterfall_image != NULL) {
            lv_image_set_src(g_ui.waterfall_image,
                             &g_waterfall_image_dsc[g_waterfall_active_source]);
        }
    }
    g_ui.waterfall_rendered_pan_columns = g_ui.waterfall_pan_columns;
    g_ui.waterfall_pan_present_tick = lv_tick_get();
    refresh_waterfall_timing();
    refresh_rf_box_overlays();
}

static void waterfall_pan_by(int32_t delta_columns)
{
    if(g_ui.running) rf_ui_set_running(false);

    const int32_t maximum_pan = (int32_t)
        (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS);
    int32_t next_pan = (int32_t)g_ui.waterfall_pan_columns + delta_columns;
    if(next_pan < 0) next_pan = 0;
    if(next_pan > maximum_pan) next_pan = maximum_pan;
    if((uint16_t)next_pan == g_ui.waterfall_pan_columns) {
        refresh_waterfall_timing();
        return;
    }

    g_ui.waterfall_pan_columns = (uint16_t)next_pan;
    waterfall_paused_view_present();
}

static void input_diag_record(rf_ui_input_control_t control,
                              uint32_t value,
                              lv_event_code_t event_code,
                              bool handled)
{
    g_rf_ui_input_diag.events++;
    if(handled) g_rf_ui_input_diag.handled_events++;
    else g_rf_ui_input_diag.ignored_events++;
    g_rf_ui_input_diag.last_control = (uint32_t)control;
    g_rf_ui_input_diag.last_value = value;
    g_rf_ui_input_diag.last_event_code = (uint32_t)event_code;
    g_rf_ui_input_diag.last_event_tick_ms = lv_tick_get();
    if((uint32_t)control < RF_UI_INPUT_CONTROL_COUNT) {
        g_rf_ui_input_diag.control_events[control]++;
    }
}

static void history_slider_event(lv_event_t * event)
{
    if(lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
       g_ui.history_slider_updating) return;

    const int32_t maximum_pan = (int32_t)
        (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS);
    int32_t slider_value = lv_slider_get_value(g_ui.history_slider);
    if(slider_value < 0) slider_value = 0;
    if(slider_value > maximum_pan) slider_value = maximum_pan;
    if(g_ui.running) rf_ui_set_running(false);

    const uint16_t next_pan = (uint16_t)(maximum_pan - slider_value);
    if(next_pan == g_ui.waterfall_pan_columns) {
        input_diag_record(RF_UI_INPUT_CONTROL_HISTORY_SLIDER,
                          (uint32_t)slider_value,
                          LV_EVENT_VALUE_CHANGED, true);
        refresh_waterfall_timing();
        return;
    }
    g_ui.waterfall_pan_columns = next_pan;
    input_diag_record(RF_UI_INPUT_CONTROL_HISTORY_SLIDER,
                      (uint32_t)slider_value,
                      LV_EVENT_VALUE_CHANGED, true);
    waterfall_paused_view_present();
}

static void history_button_event(lv_event_t * event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uint32_t action = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    if(action == RF_HISTORY_LIVE) {
        rf_ui_toggle_running();
    }
    else if(action == RF_HISTORY_OLDER) {
        waterfall_pan_by((int32_t)RF_WATERFALL_HISTORY_STEP_COLS);
    }
    else if(action == RF_HISTORY_NEWER) {
        waterfall_pan_by(-(int32_t)RF_WATERFALL_HISTORY_STEP_COLS);
    }
    else {
        input_diag_record(RF_UI_INPUT_CONTROL_HISTORY_BUTTON, action,
                          LV_EVENT_CLICKED, false);
        return;
    }
    input_diag_record(RF_UI_INPUT_CONTROL_HISTORY_BUTTON, action,
                      LV_EVENT_CLICKED, true);
}

static void live_button_event(lv_event_t * event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED) {
        input_diag_record(RF_UI_INPUT_CONTROL_LIVE, 0U,
                          LV_EVENT_CLICKED, true);
        rf_ui_toggle_running();
    }
}

static void waterfall_pan_event(lv_event_t * event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_PRESSED) {
        input_diag_record(RF_UI_INPUT_CONTROL_WATERFALL, 1U, code, true);
        g_ui.waterfall_drag_accumulator = 0;
        g_ui.waterfall_pan_present_tick =
            lv_tick_get() - RF_WATERFALL_PAN_PRESENT_PERIOD_MS;
        return;
    }
    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        input_diag_record(RF_UI_INPUT_CONTROL_WATERFALL, 0U, code, true);
        if(!g_ui.running &&
           g_ui.waterfall_rendered_pan_columns != g_ui.waterfall_pan_columns) {
            waterfall_paused_view_present();
        }
        return;
    }
    if((code != LV_EVENT_PRESSING) || g_ui.running) return;

    lv_indev_t * const input = lv_indev_active();
    if(input == NULL) return;
    lv_point_t vector = {0};
    lv_indev_get_vect(input, &vector);
    g_ui.waterfall_drag_accumulator +=
        (int32_t)vector.x * (int32_t)RF_UI_WATERFALL_COLS;
    const int32_t delta_columns =
        g_ui.waterfall_drag_accumulator / RF_WATERFALL_DISPLAY_WIDTH;
    if(delta_columns == 0) return;

    const int32_t maximum_pan = (int32_t)
        (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS);
    int32_t next_pan = (int32_t)g_ui.waterfall_pan_columns + delta_columns;
    if(next_pan < 0) next_pan = 0;
    if(next_pan > maximum_pan) next_pan = maximum_pan;
    g_ui.waterfall_drag_accumulator -=
        delta_columns * RF_WATERFALL_DISPLAY_WIDTH;
    if((uint16_t)next_pan == g_ui.waterfall_pan_columns) {
        g_ui.waterfall_drag_accumulator = 0;
        return;
    }

    g_ui.waterfall_pan_columns = (uint16_t)next_pan;
    if((uint32_t)(lv_tick_get() - g_ui.waterfall_pan_present_tick) >=
       RF_WATERFALL_PAN_PRESENT_PERIOD_MS) {
        waterfall_paused_view_present();
    }
}

static const char * const g_target_display_names[RF_DEMO_CLASS_COUNT] = {
    "大疆 MINI 3 PRO", "小霸王", "乐迪 AT9S", "云卓 T12"
};

static const char * const g_target_class_names[RF_DEMO_CLASS_COUNT] = {
    "四旋翼无人机", "涵道无人机", "遥控链路", "控制链路"
};

typedef struct {
    const rf_ui_rf_box_t * box;
    uint8_t channel;
    uint32_t observation_generation;
} rf_ui_box_ref_t;

static bool rf_box_generation_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static bool find_last_detection_box(uint32_t detection_index,
                                    rf_ui_box_ref_t * ref)
{
    if(detection_index >= RF_UI_DETECTION_COUNT || ref == NULL) return false;

    bool found = false;
    for(uint32_t channel = 0U; channel < RF_UI_CHANNEL_COUNT; ++channel) {
        const rf_ui_rf_box_batch_t * const batch =
            (!g_ui.running && channel == g_ui.committed_channel) ?
            &g_rf_box_pause_snapshot : &g_rf_box_batches[channel];
        for(uint32_t index = 0U; index < batch->count; ++index) {
            const rf_ui_rf_box_t * const box = &batch->boxes[index];
            const uint32_t generation =
                batch->observation_generation[index];
            if((box->flags & RF_UI_RF_BOX_FLAG_VALID) == 0U ||
               box->detection_index != detection_index ||
               box->frequency_span_q8 == 0U) {
                continue;
            }
            if(!found || generation == ref->observation_generation ||
               rf_box_generation_newer(
                   generation, ref->observation_generation)) {
                ref->box = box;
                ref->channel = (uint8_t)channel;
                ref->observation_generation = generation;
                found = true;
            }
        }
    }
    return found;
}

static void format_box_frequency_range(char * buffer,
                                       size_t buffer_size,
                                       const rf_ui_box_ref_t * ref)
{
    if(buffer == NULL || buffer_size == 0U || ref == NULL ||
       ref->box == NULL || ref->channel >= RF_UI_CHANNEL_COUNT) return;

    const uint32_t end_q8 =
        (uint32_t)ref->box->frequency_start_q8 +
        ref->box->frequency_span_q8;
    const uint32_t base_tenths =
        rf_demo_channels[ref->channel].center_mhz * 10U - 280U;
    const uint32_t start_tenths = base_tenths +
        (((uint32_t)ref->box->frequency_start_q8 * 560U + 128U) /
         RF_UI_RF_COORD_SCALE);
    const uint32_t end_tenths = base_tenths +
        ((end_q8 * 560U + 128U) / RF_UI_RF_COORD_SCALE);
    snprintf(buffer, buffer_size, "%u.%u-%u.%u MHz",
             (unsigned)(start_tenths / 10U),
             (unsigned)(start_tenths % 10U),
             (unsigned)(end_tenths / 10U),
             (unsigned)(end_tenths % 10U));
}

static const char * box_signal_name(uint32_t detection_index,
                                    const rf_ui_rf_box_t * box)
{
    if(box != NULL &&
       (box->flags & RF_UI_RF_BOX_FLAG_VIDEO_20MHZ) != 0U) {
        return detection_index == 3U ? "跳频遥控图传" : "图传信号";
    }
    static const char * const names[RF_UI_DETECTION_COUNT] = {
        "遥控信号", "跳频遥控", "扩频跳频遥控", "跳频遥控图传"
    };
    return detection_index < RF_UI_DETECTION_COUNT ?
           names[detection_index] : "射频信号";
}

static bool detection_online(uint32_t index)
{
    return index < RF_DEMO_CLASS_COUNT &&
           g_detections[index].state == RF_UI_DETECTION_ACTIVE;
}

static const char * detection_state_text(rf_ui_detection_state_t state)
{
    if(state == RF_UI_DETECTION_ACTIVE) return "工作";
    return "空闲";
}

static uint32_t detection_state_color(rf_ui_detection_state_t state)
{
    if(state == RF_UI_DETECTION_ACTIVE) return RF_COLOR_GREEN;
    return RF_COLOR_MUTED;
}

static uint32_t channel_target_count(uint32_t channel)
{
    uint32_t count = 0U;
    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        if(detection_online(index) && g_detections[index].channel_index == channel) {
            count++;
        }
    }
    return count;
}

static void refresh_header_status(void)
{
    if(g_ui.header_status_label == NULL) return;
    uint32_t target_count = 0U;
    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        if(detection_online(index)) target_count++;
    }
    const rf_demo_channel_t * channel =
        &rf_demo_channels[g_ui.committed_channel];
    if(g_ui.pending_channel != g_ui.committed_channel &&
       g_ui.pending_channel < RF_UI_CHANNEL_COUNT) {
        lv_label_set_text_fmt(g_ui.header_status_label,
                              "%s | %u MHz | %u 目标 | 等待 %s",
                              channel->id,
                              (unsigned)channel->center_mhz,
                              (unsigned)target_count,
                              rf_demo_channels[g_ui.pending_channel].id);
    }
    else {
        lv_label_set_text_fmt(g_ui.header_status_label,
                              "%s | %u MHz | %u 目标",
                              channel->id,
                              (unsigned)channel->center_mhz,
                              (unsigned)target_count);
    }
}

static void refresh_source_badge(void)
{
    if(g_ui.source_badge == NULL) return;
    const uint32_t badge_color = g_ui.external_spectrum_mode ?
                                 RF_COLOR_PRIMARY : RF_COLOR_ORANGE;
    lv_obj_set_style_bg_color(g_ui.source_badge,
                              color(g_ui.external_spectrum_mode ?
                                    RF_COLOR_PRIMARY_SOFT :
                                    RF_COLOR_ORANGE_SOFT), 0);
    /* The transport owns the live/paused outline.  Keep the IQ cell as an
     * internal divider so a source refresh cannot paint a second outline. */
    lv_obj_set_style_border_color(g_ui.source_badge,
                                  color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_text_color(g_ui.source_badge_label, color(badge_color), 0);
    lv_label_set_text(g_ui.source_badge_label,
                      g_ui.external_spectrum_mode ? "IQ" : "DEMO");
    refresh_header_status();
}

static void refresh_selector_style(uint32_t index)
{
    if(index >= RF_DEMO_CHANNEL_COUNT ||
       g_ui.selector_buttons[index] == NULL) return;

    const bool selected = index == g_ui.committed_channel;
    const bool pending = !selected && index == g_ui.pending_channel;
    const uint32_t accent = selected ? RF_COLOR_PRIMARY :
                            (pending ? RF_COLOR_AMBER : RF_COLOR_BORDER);
    const uint32_t target_count = channel_target_count(index);
    const uint32_t occupancy = g_channel_metrics[index].occupancy_percent;
    lv_obj_set_style_bg_color(g_ui.selector_buttons[index],
                              color(selected ? RF_COLOR_PRIMARY_SOFT :
                                    (pending ? RF_COLOR_AMBER_SOFT :
                                               RF_COLOR_PANEL)), 0);
    lv_obj_set_style_border_color(g_ui.selector_buttons[index],
                                  color(accent), 0);
    lv_obj_set_style_text_color(g_ui.selector_titles[index],
                                color(selected ? RF_COLOR_PRIMARY :
                                      (pending ? RF_COLOR_AMBER :
                                                 RF_COLOR_TEXT)), 0);
    lv_obj_set_style_text_color(g_ui.selector_frequencies[index],
                                color(selected ? RF_COLOR_PRIMARY :
                                      (pending ? RF_COLOR_AMBER :
                                                 RF_COLOR_MUTED)), 0);
    if(g_ui.selector_counts[index] != NULL) {
        if(target_count == 0U) {
            lv_label_set_text(g_ui.selector_counts[index], "空闲");
        }
        else {
            lv_label_set_text_fmt(g_ui.selector_counts[index], "%u 目标",
                                  (unsigned)target_count);
        }
        lv_obj_set_style_text_color(g_ui.selector_counts[index],
                                    color(pending ? RF_COLOR_AMBER :
                                          (target_count == 0U ?
                                           RF_COLOR_MUTED : RF_COLOR_PRIMARY)), 0);
    }
    if(g_ui.selector_bars[index] != NULL) {
        const int32_t width = occupancy == 0U ? 4 :
            (int32_t)(((RF_CHANNEL_CARD_WIDTH - 16U) * occupancy + 50U) /
                      100U);
        lv_obj_set_width(g_ui.selector_bars[index], width);
        lv_obj_set_style_bg_color(g_ui.selector_bars[index], color(accent), 0);
        set_visible(g_ui.selector_bars[index], true);
    }
}

static void refresh_selector_styles(void)
{
    for(uint32_t index = 0; index < RF_DEMO_CHANNEL_COUNT; ++index) {
        refresh_selector_style(index);
    }
}

static void refresh_target_cards(void)
{
    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        if(g_ui.target_buttons[index] == NULL) continue;
        const rf_ui_detection_t * detection = &g_detections[index];
        const bool online = detection_online(index);
        const bool selected =
            (int32_t)index == g_ui.selected_detection_index;
        const uint32_t accent = g_target_accent_colors[index];
        lv_obj_set_style_bg_color(g_ui.target_buttons[index],
                                  color(RF_COLOR_PANEL), 0);
        lv_obj_set_style_border_color(g_ui.target_buttons[index],
                                      color(online ? accent : RF_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(g_ui.target_buttons[index],
                                      selected ? 2 : 1, 0);
        lv_obj_set_style_bg_color(g_ui.target_number_labels[index],
                                  color(online ? accent : RF_COLOR_PANEL_ALT), 0);
        lv_obj_set_style_text_color(g_ui.target_number_labels[index],
                                    color(online ? RF_COLOR_ON_PRIMARY :
                                                   RF_COLOR_MUTED), 0);
        if(g_ui.target_images[index] != NULL) {
            lv_obj_set_style_opa(g_ui.target_images[index],
                                 online ? (lv_opa_t)235U : (lv_opa_t)208U, 0);
            lv_obj_set_style_image_recolor(
                g_ui.target_images[index], color(RF_COLOR_MUTED), 0);
            lv_obj_set_style_image_recolor_opa(
                g_ui.target_images[index],
                online ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        }
        if(g_ui.target_image_frames[index] != NULL) {
            lv_obj_set_style_border_color(
                g_ui.target_image_frames[index],
                color(online ? accent : RF_COLOR_BORDER), 0);
        }
        lv_label_set_text(g_ui.target_name_labels[index],
                          g_target_display_names[index]);
        if(online) {
            rf_ui_box_ref_t ref = {0};
            if(find_last_detection_box(index, &ref)) {
                char range[32];
                format_box_frequency_range(range, sizeof(range), &ref);
                lv_label_set_text(g_ui.target_channel_labels[index], range);
            }
            else {
                const rf_demo_channel_t * channel =
                    &rf_demo_channels[detection->channel_index];
                lv_label_set_text_fmt(g_ui.target_channel_labels[index],
                                      "%s | %u MHz", channel->id,
                                      (unsigned)channel->center_mhz);
            }
        }
        else {
            lv_label_set_text(g_ui.target_channel_labels[index], "");
        }
        lv_label_set_text(g_ui.target_state_labels[index],
                          detection_state_text(detection->state));
        lv_obj_set_style_text_color(g_ui.target_state_labels[index],
                                    color(online ? accent : RF_COLOR_MUTED), 0);
        if(g_ui.target_confidence_labels[index] != NULL) {
            if(online) {
                lv_label_set_text_fmt(g_ui.target_confidence_labels[index],
                                      "%u%%",
                                      (unsigned)detection->confidence_percent);
            }
            else {
                lv_label_set_text(g_ui.target_confidence_labels[index],
                                  "空闲");
            }
            lv_obj_set_style_text_color(g_ui.target_confidence_labels[index],
                                        color(online ? accent : RF_COLOR_MUTED), 0);
        }
        lv_obj_set_style_bg_color(g_ui.target_bars[index], color(accent), 0);
        lv_obj_set_style_bg_opa(g_ui.target_bars[index],
                                online ? (lv_opa_t)((uint32_t)
                                detection->confidence_percent * 255U / 100U) :
                                (lv_opa_t)51U, 0);
        lv_obj_set_width(g_ui.target_bars[index], RF_TARGET_CARD_WIDTH);
    }
}

static void refresh_compare_overlay(void)
{
    if(g_ui.compare_overlay == NULL) return;
    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        const rf_ui_detection_t * detection = &g_detections[index];
        const bool online = detection_online(index);
        const uint32_t state_color = detection_state_color(detection->state);
        lv_label_set_text(g_ui.compare_names[index], g_target_display_names[index]);
        if(online) {
            const rf_demo_channel_t * channel =
                &rf_demo_channels[detection->channel_index];
            const rf_ui_channel_metrics_t * metrics =
                &g_channel_metrics[detection->channel_index];
            lv_label_set_text_fmt(g_ui.compare_channels[index],
                                  "%s | %u MHz | 峰值 %d dBFS",
                                  channel->id, (unsigned)channel->center_mhz,
                                  metrics->peak_dbfs);
            lv_label_set_text_fmt(g_ui.compare_confidences[index],
                                  "置信度 %u%%", (unsigned)detection->confidence_percent);
        }
        else {
            lv_label_set_text(g_ui.compare_channels[index], "当前空闲");
            lv_label_set_text(g_ui.compare_confidences[index], "置信度 空闲");
        }
        lv_label_set_text(g_ui.compare_states[index],
                          detection_state_text(detection->state));
        lv_obj_set_style_text_color(g_ui.compare_states[index], color(state_color), 0);
        lv_obj_set_style_border_color(g_ui.compare_cards[index],
                                      color((int32_t)index ==
                                            g_ui.selected_detection_index ?
                                            RF_COLOR_PRIMARY : RF_COLOR_BORDER), 0);
    }
}

static void refresh_acquisition_mode(void)
{
    for(uint32_t index = 0; index < RF_ACQUISITION_MODE_COUNT; ++index) {
        if(g_ui.acquisition_buttons[index] == NULL) continue;
        const bool selected = (index == RF_ACQUISITION_FOCUS) == g_ui.focus_mode;
        lv_obj_set_style_bg_color(g_ui.acquisition_buttons[index],
                                  color(selected ? RF_COLOR_PRIMARY :
                                                   RF_COLOR_PANEL_ALT), 0);
        lv_obj_set_style_border_color(g_ui.acquisition_buttons[index],
                                      color(selected ? RF_COLOR_PRIMARY :
                                                       RF_COLOR_BORDER), 0);
        lv_obj_set_style_text_color(g_ui.acquisition_labels[index],
                                    color(selected ? RF_COLOR_ON_PRIMARY :
                                                     RF_COLOR_TEXT), 0);
    }
    refresh_scan_rate();
}

static void set_frequency_labels(lv_obj_t * labels[5], uint32_t center_mhz)
{
    if(labels[0] == NULL) return;
    lv_label_set_text_fmt(labels[0], "%u",
                          (unsigned)(center_mhz - RF_CHANNEL_HALF_BANDWIDTH_MHZ));
    lv_label_set_text_fmt(labels[1], "%u", (unsigned)(center_mhz - 14U));
    lv_label_set_text_fmt(labels[2], "%u", (unsigned)center_mhz);
    lv_label_set_text_fmt(labels[3], "%u", (unsigned)(center_mhz + 14U));
    lv_label_set_text_fmt(labels[4], "%u",
                          (unsigned)(center_mhz + RF_CHANNEL_HALF_BANDWIDTH_MHZ));
}

static void set_waterfall_frequency_labels(lv_obj_t * labels[5],
                                           uint32_t center_mhz)
{
    if(labels[0] == NULL) return;
    lv_label_set_text_fmt(labels[0], "%u",
                          (unsigned)(center_mhz + RF_CHANNEL_HALF_BANDWIDTH_MHZ));
    lv_label_set_text_fmt(labels[1], "%u", (unsigned)(center_mhz + 14U));
    lv_label_set_text_fmt(labels[2], "%u", (unsigned)center_mhz);
    lv_label_set_text_fmt(labels[3], "%u", (unsigned)(center_mhz - 14U));
    lv_label_set_text_fmt(labels[4], "%u",
                          (unsigned)(center_mhz - RF_CHANNEL_HALF_BANDWIDTH_MHZ));
}

static void refresh_selected_metric(uint32_t metric_index)
{
    if(metric_index >= RF_CHANNEL_METRIC_COUNT) return;

    const rf_ui_channel_metrics_t * metrics =
        &g_channel_metrics[g_ui.committed_channel];
    lv_obj_t * const header_label = g_ui.selected_metric_labels[metric_index];
    lv_obj_t * const side_label = g_ui.side_metric_labels[metric_index];
    const bool detail_selected =
        g_ui.selected_detection_index >= 0 &&
        g_ui.selected_detection_index < (int8_t)RF_DEMO_CLASS_COUNT;
    if(side_label != NULL && !detail_selected) {
        lv_label_set_text(side_label, "--");
        lv_obj_set_style_text_color(side_label, color(RF_COLOR_MUTED), 0);
    }
    switch(metric_index) {
        case RF_CHANNEL_METRIC_PEAK:
            if(header_label != NULL) {
                lv_label_set_text_fmt(header_label, "峰值 %d dBFS",
                                      metrics->peak_dbfs);
            }
            if(side_label != NULL && detail_selected) {
                lv_label_set_text_fmt(side_label, "%d dBFS", metrics->peak_dbfs);
            }
            break;
        case RF_CHANNEL_METRIC_NOISE:
            if(header_label != NULL) {
                lv_label_set_text_fmt(header_label, "底噪 %d dBFS",
                                      metrics->noise_floor_dbfs);
            }
            if(side_label != NULL && detail_selected) {
                lv_label_set_text_fmt(side_label, "%d dBFS",
                                      metrics->noise_floor_dbfs);
            }
            break;
        case RF_CHANNEL_METRIC_OCCUPANCY:
        {
            const uint32_t metric_color = metrics->occupancy_percent < 35u ?
                                          RF_COLOR_GREEN :
                                          (metrics->occupancy_percent < 70u ?
                                           RF_COLOR_AMBER : RF_COLOR_RED);
            if(header_label != NULL) {
                lv_label_set_text_fmt(header_label, "占用 %u%%",
                                      (unsigned)metrics->occupancy_percent);
                lv_obj_set_style_text_color(header_label, color(metric_color), 0);
            }
            if(side_label != NULL && detail_selected) {
                lv_label_set_text_fmt(side_label, "%u%%",
                                      (unsigned)metrics->occupancy_percent);
            }
            break;
        }
        default:
            if(header_label != NULL) {
                if(metrics->age_ms < 1000u) {
                    lv_label_set_text_fmt(header_label, "龄期 %u ms",
                                          (unsigned)metrics->age_ms);
                } else {
                    lv_label_set_text_fmt(header_label, "龄期 %u.%us",
                                          (unsigned)(metrics->age_ms / 1000u),
                                          (unsigned)((metrics->age_ms % 1000u) / 100u));
                }
            }
            if(side_label != NULL && detail_selected) {
                if(metrics->age_ms < 1000u) {
                    lv_label_set_text_fmt(side_label, "%u ms",
                                          (unsigned)metrics->age_ms);
                }
                else {
                    lv_label_set_text_fmt(side_label, "%u.%u s",
                                          (unsigned)(metrics->age_ms / 1000u),
                                          (unsigned)((metrics->age_ms % 1000u) / 100u));
                }
            }
            break;
    }
}

static void refresh_target_detail_surface(void)
{
    lv_obj_set_style_bg_color(g_ui.alert_banner, color(RF_COLOR_PANEL), 0);
    lv_obj_set_style_border_color(g_ui.alert_banner,
                                  color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_bg_color(g_ui.alert_prefix, color(RF_COLOR_PRIMARY), 0);
    if(g_ui.detail_title_panel != NULL) {
        lv_obj_set_style_bg_color(g_ui.detail_title_panel,
                                  color(RF_COLOR_HEADER), 0);
        lv_obj_set_style_border_color(g_ui.detail_title_panel,
                                      color(RF_COLOR_DIVIDER), 0);
    }
    if(g_ui.detail_ranges_panel != NULL) {
        lv_obj_set_style_bg_color(g_ui.detail_ranges_panel,
                                  color(RF_COLOR_PANEL), 0);
        lv_obj_set_style_border_color(g_ui.detail_ranges_panel,
                                      color(RF_COLOR_DIVIDER), 0);
    }
    for(uint32_t index = 0U; index < 3U; ++index) {
        if(g_ui.detail_metric_panels[index] != NULL) {
            lv_obj_set_style_bg_color(g_ui.detail_metric_panels[index],
                                      color(RF_COLOR_PANEL), 0);
            lv_obj_set_style_border_color(g_ui.detail_metric_panels[index],
                                          color(RF_COLOR_DIVIDER), 0);
        }
    }
    if(g_ui.detail_confidence_panel != NULL) {
        lv_obj_set_style_bg_color(g_ui.detail_confidence_panel,
                                  color(RF_COLOR_PANEL), 0);
        lv_obj_set_style_border_color(g_ui.detail_confidence_panel,
                                      color(RF_COLOR_DIVIDER), 0);
    }
    if(g_ui.detail_preview_panel != NULL) {
        lv_obj_set_style_bg_color(g_ui.detail_preview_panel,
                                  color(RF_COLOR_PANEL_ALT), 0);
    }
    if(g_ui.alert_badge != NULL) {
        lv_obj_set_style_bg_color(g_ui.alert_badge,
                                  color(RF_COLOR_BORDER), 0);
    }
    if(g_ui.detail_image_frame != NULL) {
        lv_obj_set_style_border_color(g_ui.detail_image_frame,
                                      color(RF_COLOR_BORDER), 0);
    }
}

static void refresh_target_detail(void)
{
    if(g_ui.alert_banner == NULL) return;
    int32_t selected = g_ui.selected_detection_index;
    if(selected < 0 || selected >= (int32_t)RF_DEMO_CLASS_COUNT) {
        selected = -1;
    }
    g_ui.selected_detection_index = (int8_t)selected;
    const bool selection_present = selected >= 0;
    const bool working = selection_present &&
        detection_online((uint32_t)selected);
    const uint32_t accent = working ? RF_COLOR_PRIMARY : RF_COLOR_MUTED;
    refresh_target_detail_surface();

    if(!selection_present) {
        lv_label_set_text(g_ui.alert_drone_label, "空闲");
        lv_label_set_text(g_ui.side_channel_label, "--");
        set_visible(g_ui.detail_range_name_labels[0], false);
        set_visible(g_ui.detail_range_value_labels[0], false);
        set_visible(g_ui.detail_range_name_labels[1], false);
        set_visible(g_ui.detail_range_value_labels[1], false);
        lv_label_set_text(g_ui.alert_idle_label, "空闲");
        lv_obj_set_width(g_ui.alert_confidence_fill, 0);
        lv_label_set_text(g_ui.detail_empty_label, "空闲");
        set_visible(g_ui.detail_image, false);
        set_visible(g_ui.detail_empty_label, true);
    }
    else {
        const uint32_t target = (uint32_t)selected;
        const rf_ui_detection_t * const detection = &g_detections[target];
        rf_ui_box_ref_t last_ref = {0};
        const bool range_present =
            working && find_last_detection_box(target, &last_ref);
        lv_label_set_text(g_ui.alert_drone_label,
                          working ? g_target_display_names[target] : "空闲");
        lv_label_set_text_fmt(g_ui.side_channel_label, "%02u",
                              (unsigned)target + 1U);
        for(uint32_t index = 0U; index < 2U; ++index) {
            const bool present = index == 0U && range_present;
            set_visible(g_ui.detail_range_name_labels[index], present);
            set_visible(g_ui.detail_range_value_labels[index], present);
            if(!present) continue;
            char range[32];
            format_box_frequency_range(range, sizeof(range), &last_ref);
            lv_label_set_text(g_ui.detail_range_name_labels[index],
                              box_signal_name(target, last_ref.box));
            lv_label_set_text(g_ui.detail_range_value_labels[index], range);
            lv_obj_set_style_text_color(
                g_ui.detail_range_name_labels[index], color(accent), 0);
            lv_obj_set_style_text_color(
                g_ui.detail_range_value_labels[index], color(accent), 0);
        }
        if(working && !range_present) {
            set_visible(g_ui.detail_range_name_labels[0], true);
            set_visible(g_ui.detail_range_value_labels[0], true);
            lv_label_set_text(g_ui.detail_range_name_labels[0],
                              rf_demo_channels[detection->channel_index].id);
            lv_label_set_text_fmt(g_ui.detail_range_value_labels[0],
                                  "%u MHz",
                                  (unsigned)rf_demo_channels[
                                      detection->channel_index].center_mhz);
        }
        lv_obj_set_style_text_color(g_ui.detail_range_name_labels[0],
                                    color(accent), 0);
        lv_obj_set_style_text_color(g_ui.detail_range_value_labels[0],
                                    color(accent), 0);
        if(working) {
            lv_label_set_text_fmt(g_ui.alert_idle_label, "%u%%",
                                  (unsigned)detection->confidence_percent);
            lv_obj_set_width(g_ui.alert_confidence_fill,
                             (int32_t)((140U *
                                 detection->confidence_percent + 50U) /
                                100U));
        }
        else {
            lv_label_set_text(g_ui.alert_idle_label, "空闲");
            lv_obj_set_width(g_ui.alert_confidence_fill, 0);
        }
        lv_image_set_src(g_ui.detail_image, g_target_detail_assets[target]);
        lv_obj_set_style_image_recolor(g_ui.detail_image,
                                       color(RF_COLOR_MUTED), 0);
        lv_obj_set_style_image_recolor_opa(
            g_ui.detail_image,
            working ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        set_visible(g_ui.detail_image, true);
        set_visible(g_ui.detail_empty_label, false);
    }

    lv_obj_set_style_text_color(g_ui.side_channel_label, color(accent), 0);
    lv_obj_set_style_text_color(g_ui.alert_idle_label, color(accent), 0);
    lv_obj_set_style_bg_color(g_ui.alert_confidence_fill, color(accent), 0);
    for(uint32_t metric = RF_CHANNEL_METRIC_PEAK;
        metric <= RF_CHANNEL_METRIC_OCCUPANCY; ++metric) {
        if(g_ui.side_metric_labels[metric] != NULL) {
            lv_obj_set_style_text_color(g_ui.side_metric_labels[metric],
                                        color(accent), 0);
        }
        refresh_selected_metric(metric);
    }
}

static void refresh_selected_view(void)
{
    if(g_ui.screen == NULL) return;
    const rf_demo_channel_t * channel =
        &rf_demo_channels[g_ui.committed_channel];

    if(g_ui.selected_channel_label != NULL) {
        lv_label_set_text_fmt(g_ui.selected_channel_label, "%s | %u MHz",
                              channel->id, (unsigned)channel->center_mhz);
    }
    if(g_ui.waterfall_channel_label != NULL) {
        lv_label_set_text_fmt(g_ui.waterfall_channel_label, "%s | %u MHz",
                              channel->id, (unsigned)channel->center_mhz);
    }
    for(uint32_t index = 0; index < RF_CHANNEL_METRIC_COUNT; ++index) {
        refresh_selected_metric(index);
    }
    set_frequency_labels(g_ui.spectrum_frequency_labels, channel->center_mhz);
    set_waterfall_frequency_labels(g_ui.waterfall_frequency_labels, channel->center_mhz);
    refresh_selector_styles();
    refresh_header_status();
    refresh_target_detail();
    refresh_target_cards();
    refresh_rf_box_overlays();
}

static void refresh_alert(bool force)
{
    (void)force;
    refresh_target_cards();
    refresh_selector_styles();
    refresh_target_detail();
    refresh_compare_overlay();
    refresh_header_status();
}

static void select_target_index(uint32_t index)
{
    if(index >= RF_DEMO_CLASS_COUNT) return;
    const bool online = detection_online(index);
    const uint32_t target_channel = g_detections[index].channel_index;
    if(g_ui.selected_detection_index == (int8_t)index) {
        g_ui.selected_detection_index = -1;
        g_ui.pending_detection_index = -1;
        refresh_alert(true);
        return;
    }
    const bool channel_changed =
        online && target_channel != g_ui.committed_channel;
    if(channel_changed) {
        if(!rf_ui_set_selected_channel(target_channel)) return;
        g_ui.pending_detection_index = (int8_t)index;
        (void)channel_switch_defer_metadata_refresh();
        if(g_ui.focus_mode) {
            (void)display_app_request_focus(target_channel);
        }
        return;
    }
    if(!online && g_ui.pending_channel != g_ui.committed_channel) {
        if(!rf_ui_set_selected_channel(g_ui.committed_channel)) return;
    }
    g_ui.selected_detection_index = (int8_t)index;
    g_ui.pending_detection_index = -1;
    refresh_alert(true);
}

static void target_click_event(lv_event_t * event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uint32_t index =
        (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    const bool handled = g_ui.running && index < RF_DEMO_CLASS_COUNT;
    input_diag_record(RF_UI_INPUT_CONTROL_TARGET, index,
                      LV_EVENT_CLICKED, handled);
    if(!handled) return;
    select_target_index(index);
}

static void compare_target_click_event(lv_event_t * event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const uint32_t index =
        (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    input_diag_record(RF_UI_INPUT_CONTROL_COMPARE_TARGET, index,
                      LV_EVENT_CLICKED, g_ui.running);
    if(!g_ui.running) return;
    select_target_index(index);
    set_visible(g_ui.compare_overlay, false);
}

static void compare_button_event(lv_event_t * event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    input_diag_record(RF_UI_INPUT_CONTROL_COMPARE_OPEN, 0U,
                      LV_EVENT_CLICKED, true);
    refresh_compare_overlay();
    set_visible(g_ui.compare_overlay, true);
    lv_obj_move_foreground(g_ui.compare_overlay);
}

static void compare_close_event(lv_event_t * event)
{
    if(lv_event_get_code(event) == LV_EVENT_CLICKED) {
        input_diag_record(RF_UI_INPUT_CONTROL_COMPARE_CLOSE, 0U,
                          LV_EVENT_CLICKED, true);
        set_visible(g_ui.compare_overlay, false);
    }
}

static void selector_click_event(lv_event_t * event)
{
    const uint32_t channel = (uint32_t) (uintptr_t) lv_event_get_user_data(event);
    if(!g_ui.running) {
        input_diag_record(RF_UI_INPUT_CONTROL_CHANNEL, channel,
                          LV_EVENT_CLICKED, false);
        return;
    }
    if(channel == g_ui.committed_channel &&
       g_ui.pending_channel == g_ui.committed_channel) {
        input_diag_record(RF_UI_INPUT_CONTROL_CHANNEL, channel,
                          LV_EVENT_CLICKED, false);
        return;
    }
    const bool accepted = rf_ui_set_selected_channel(channel);
    input_diag_record(RF_UI_INPUT_CONTROL_CHANNEL, channel,
                      LV_EVENT_CLICKED, accepted);
    if(accepted && g_ui.focus_mode) {
        (void) display_app_request_focus(channel);
    }
}

static void acquisition_mode_click_event(lv_event_t * event)
{
    const uint32_t mode = (uint32_t) (uintptr_t) lv_event_get_user_data(event);
    bool accepted = false;

    if(mode == RF_ACQUISITION_SCAN) {
        if(g_ui.focus_mode) accepted = display_app_request_scan();
    }
    else if(mode == RF_ACQUISITION_FOCUS) {
        if(!g_ui.focus_mode) {
            accepted = display_app_request_focus(g_ui.committed_channel);
        }
    }

    input_diag_record(RF_UI_INPUT_CONTROL_ACQUISITION, mode,
                      LV_EVENT_CLICKED, accepted);
    if(accepted) rf_ui_set_focus_mode(mode == RF_ACQUISITION_FOCUS);
}

static void create_acquisition_modes(lv_obj_t * header)
{
    static const char * labels[RF_ACQUISITION_MODE_COUNT] = {
        "全频扫描", "重点锁定"
    };

    for(uint32_t index = 0; index < RF_ACQUISITION_MODE_COUNT; ++index) {
        lv_obj_t * button = create_box(header,
                                       RF_MODE_X +
                                       (int32_t)(index * (RF_MODE_WIDTH + RF_MODE_GAP)),
                                       RF_MODE_Y, RF_MODE_WIDTH, RF_MODE_HEIGHT,
                                       RF_COLOR_PANEL, LV_OPA_COVER);
        g_ui.acquisition_buttons[index] = button;
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, color(RF_COLOR_BORDER), 0);
        lv_obj_set_style_radius(button, 3, 0);
        lv_obj_set_style_bg_color(button, color(RF_COLOR_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, acquisition_mode_click_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)index);
        g_ui.acquisition_labels[index] = create_label(
            button, 0, 13, RF_MODE_WIDTH, 18, labels[index], &rf_font_zh_14,
            RF_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);
    }
}

static void create_header(void)
{
    lv_obj_t * header = create_box(g_ui.screen, 0, 0, RF_SCREEN_WIDTH,
                                   RF_HEADER_HEIGHT, RF_COLOR_HEADER, LV_OPA_COVER);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t * brand = create_box(header, 12, 7, 40, 40,
                                  RF_COLOR_PRIMARY, LV_OPA_COVER);
    lv_obj_set_style_radius(brand, 4, 0);
    create_label(brand, 0, 10, 40, 20, "RF", &rf_font_14,
                 RF_COLOR_ON_PRIMARY, LV_TEXT_ALIGN_CENTER);
    create_label(header, 60, 6, 190, 20, "低空射频监测", &rf_font_zh_14,
                 RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    create_label(header, 60, 29, 190, 16, "RA8P1",
                 &rf_font_12, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);

    create_acquisition_modes(header);

    lv_obj_t * transport = create_box(
        header, RF_TRANSPORT_X, RF_TRANSPORT_Y,
        RF_TRANSPORT_WIDTH, RF_TRANSPORT_HEIGHT,
        RF_COLOR_PANEL, LV_OPA_COVER);
    g_ui.transport = transport;
    lv_obj_set_style_border_width(transport, 1, 0);
    lv_obj_set_style_border_color(transport, color(RF_COLOR_GREEN), 0);
    lv_obj_set_style_radius(transport, 5, 0);

    g_ui.live_button = create_box(transport, RF_LIVE_BUTTON_X, 0,
                                  RF_LIVE_BUTTON_WIDTH,
                                  RF_TRANSPORT_HEIGHT,
                                  RF_COLOR_GREEN_SOFT, LV_OPA_COVER);
    lv_obj_set_style_border_width(g_ui.live_button, 1, 0);
    lv_obj_set_style_border_color(g_ui.live_button, color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_side(g_ui.live_button, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_bg_color(g_ui.live_button, color(RF_COLOR_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_add_flag(g_ui.live_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_ui.live_button, live_button_event,
                        LV_EVENT_CLICKED, NULL);
    g_ui.live_icon = create_label(g_ui.live_button, 0, 13,
                                  RF_LIVE_BUTTON_WIDTH, 18,
                                  LV_SYMBOL_PAUSE, &lv_font_montserrat_14,
                                  RF_COLOR_GREEN, LV_TEXT_ALIGN_CENTER);

    static const uint32_t actions[2] = {
        RF_HISTORY_OLDER, RF_HISTORY_NEWER
    };
    static const int32_t button_x[2] = {
        RF_HISTORY_OLDER_X, RF_HISTORY_NEWER_X
    };
    static const char * symbols[2] = {LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT};
    for(uint32_t index = 0U; index < 2U; ++index) {
        const uint32_t action = actions[index];
        lv_obj_t * button = create_box(
            transport, button_x[index], 0,
            RF_HISTORY_BUTTON_WIDTH, RF_TRANSPORT_HEIGHT,
            RF_COLOR_PANEL_ALT, LV_OPA_COVER);
        g_ui.history_buttons[action] = button;
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, color(RF_COLOR_DIVIDER), 0);
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_bg_color(button, color(RF_COLOR_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, history_button_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)action);
        g_ui.history_labels[action] = create_label(
            button, 0, 13, RF_HISTORY_BUTTON_WIDTH, 18, symbols[index],
            &lv_font_montserrat_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    }

    lv_obj_t * timeline = create_box(
        transport, RF_HISTORY_TIMELINE_X, 0,
        RF_HISTORY_TIMELINE_WIDTH, RF_TRANSPORT_HEIGHT,
        RF_COLOR_PANEL, LV_OPA_COVER);
    g_ui.live_label = create_label(timeline, 8, 3, 42, 16, "实时",
                                   &rf_font_zh_14, RF_COLOR_GREEN,
                                   LV_TEXT_ALIGN_LEFT);
    g_ui.transport_meta_label = create_label(
        timeline, 52, 3, 274, 16, "CH1 | 2420 MHz | 0 目标",
        &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    g_ui.header_status_label = g_ui.transport_meta_label;
    g_ui.transport_time_label = create_label(
        timeline, 326, 3, 58, 16, "LIVE", &rf_font_10,
        RF_COLOR_GREEN, LV_TEXT_ALIGN_RIGHT);

    g_ui.history_slider = lv_slider_create(timeline);
    reset_object(g_ui.history_slider);
    lv_obj_add_flag(g_ui.history_slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(g_ui.history_slider, 10, 22);
    lv_obj_set_size(g_ui.history_slider, 372, 18);
    lv_slider_set_range(g_ui.history_slider, 0,
                        RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS);
    lv_slider_set_value(g_ui.history_slider,
                        RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS,
                        LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_ui.history_slider, color(RF_COLOR_BORDER),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_ui.history_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(g_ui.history_slider, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(g_ui.history_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_ui.history_slider, color(RF_COLOR_PRIMARY),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_ui.history_slider, LV_OPA_COVER,
                            LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_ui.history_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_ui.history_slider, color(RF_COLOR_PRIMARY),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_ui.history_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(g_ui.history_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_height(g_ui.history_slider, 12, LV_PART_KNOB);
    lv_obj_set_style_radius(g_ui.history_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(g_ui.history_slider, history_slider_event,
                        LV_EVENT_VALUE_CHANGED, NULL);

    g_ui.source_badge = create_box(
        transport, RF_SOURCE_BADGE_X, 0,
        RF_SOURCE_BADGE_WIDTH, RF_TRANSPORT_HEIGHT,
        RF_COLOR_PRIMARY_SOFT, LV_OPA_COVER);
    lv_obj_set_style_border_width(g_ui.source_badge, 1, 0);
    lv_obj_set_style_border_color(g_ui.source_badge, color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_side(g_ui.source_badge, LV_BORDER_SIDE_LEFT, 0);
    create_label(g_ui.source_badge, 5, 14, 16, 16, LV_SYMBOL_WIFI,
                 &lv_font_montserrat_14, RF_COLOR_PRIMARY,
                 LV_TEXT_ALIGN_CENTER);
    g_ui.source_badge_label = create_label(
        g_ui.source_badge, 24, 14, 24, 16, "IQ", &rf_font_10,
        RF_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
}

static void create_metrics_footer(void)
{
    lv_obj_t * footer = create_box(g_ui.screen, 0, RF_METRICS_Y, RF_SCREEN_WIDTH,
                                   RF_METRICS_HEIGHT, RF_COLOR_HEADER, LV_OPA_COVER);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_color(footer, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);

    g_ui.scan_rate_label = create_label(footer, 10, 4, 136, 16, "扫描 --.- Hz",
                                        &rf_font_zh_14, RF_COLOR_PRIMARY,
                                        LV_TEXT_ALIGN_LEFT);
    char panel[24];
    char presented[24];
    char render_max[24];
    char underflows[24];
    format_millihz(panel, sizeof(panel), "Panel", g_render_metrics.panel_millihz);
    format_millihz(presented, sizeof(presented), "FPS", g_render_metrics.presented_millihz);
    format_render_max(render_max, sizeof(render_max), g_render_metrics.render_max_us);
    snprintf(underflows, sizeof(underflows), "UF %u", (unsigned) g_render_metrics.underflow_count);

    g_ui.performance_labels[RF_METRIC_PANEL] = create_label(
        footer, 154, 4, 124, 16, panel, &rf_font_12,
        RF_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    g_ui.performance_labels[RF_METRIC_PRESENTED] = create_label(
        footer, 286, 4, 124, 16, presented, &rf_font_12,
        RF_COLOR_GREEN, LV_TEXT_ALIGN_LEFT);
    g_ui.performance_labels[RF_METRIC_RENDER_MAX] = create_label(
        footer, 418, 4, 140, 16, render_max, &rf_font_12,
        RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    g_ui.performance_labels[RF_METRIC_UNDERFLOW] = create_label(
        footer, 566, 4, 100, 16, underflows, &rf_font_12,
        g_render_metrics.underflow_count == 0u ? RF_COLOR_MUTED : RF_COLOR_RED,
        LV_TEXT_ALIGN_LEFT);
    create_label(footer, 674, 4, 156, 16, "SP 256 | WF 192x160",
                 &rf_font_12, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    g_ui.detection_status_label = create_label(
        footer, 838, 4, 176, 16, "背景标定中",
        &rf_font_zh_14, RF_COLOR_AMBER, LV_TEXT_ALIGN_RIGHT);
}

static void create_target_strip(void)
{
    lv_obj_t * strip = create_box(g_ui.screen, 0, RF_TARGET_STRIP_Y,
                                  RF_SCREEN_WIDTH, RF_TARGET_STRIP_HEIGHT,
                                  RF_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_style_border_color(strip, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, 0);

    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        const int32_t x = (int32_t)index * RF_TARGET_CARD_WIDTH;
        lv_obj_t * button = create_box(strip, x, 0,
                                       RF_TARGET_CARD_WIDTH,
                                       RF_TARGET_STRIP_HEIGHT,
                                       RF_COLOR_PANEL, LV_OPA_COVER);
        g_ui.target_buttons[index] = button;
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, color(RF_COLOR_BORDER), 0);
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(button, 0, 0);
        lv_obj_set_style_bg_color(button, color(RF_COLOR_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, target_click_event, LV_EVENT_CLICKED,
                            (void *) (uintptr_t) index);

        const uint32_t accent = g_target_accent_colors[index];
        lv_obj_t * thumb = create_box(
            button, 8, 5, RF_TARGET_THUMB_WIDTH, RF_TARGET_THUMB_HEIGHT,
            0x343A3Eu, LV_OPA_COVER);
        g_ui.target_image_frames[index] = thumb;
        lv_obj_set_style_border_width(thumb, 1, 0);
        lv_obj_set_style_border_color(thumb, color(RF_COLOR_BORDER), 0);
        lv_obj_set_style_radius(thumb, 3, 0);
        lv_obj_t * image = lv_image_create(thumb);
        reset_object(image);
        g_ui.target_images[index] = image;
        lv_image_set_src(image, g_target_strip_assets[index]);
        lv_obj_set_pos(image, 0, 0);
        lv_image_set_antialias(image, false);

        char number[4];
        snprintf(number, sizeof(number), "%02u", (unsigned)index + 1U);
        g_ui.target_number_labels[index] = create_label(
            thumb, 2, 2, 22, 14, number, &rf_font_10,
            RF_COLOR_ON_PRIMARY, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_bg_color(g_ui.target_number_labels[index],
                                  color(accent), 0);
        lv_obj_set_style_bg_opa(g_ui.target_number_labels[index], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g_ui.target_number_labels[index], 1, 0);
        g_ui.target_name_labels[index] = create_label(
            button, 68, 8, 180, 18, g_target_display_names[index],
            &rf_font_zh_14, RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        g_ui.target_channel_labels[index] = create_label(
            button, 68, 29, 132, 16, "", &rf_font_10,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        g_ui.target_state_labels[index] = create_label(
            button, 200, 29, 48, 18, "空闲", &rf_font_zh_14,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        g_ui.target_bars[index] = create_box(
            button, 0, RF_TARGET_STRIP_HEIGHT - 3,
            RF_TARGET_CARD_WIDTH, 3, accent, LV_OPA_COVER);
        lv_obj_set_style_bg_opa(g_ui.target_bars[index], (lv_opa_t)51U, 0);
    }
}

static void create_selectors(void)
{
    lv_obj_t * deck = create_box(g_ui.screen, RF_CHANNEL_DECK_X, RF_BOTTOM_Y,
                                 RF_CHANNEL_DECK_WIDTH, RF_BOTTOM_HEIGHT,
                                 RF_COLOR_PANEL, LV_OPA_COVER);
    lv_obj_set_style_border_width(deck, 1, 0);
    lv_obj_set_style_border_color(deck, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(deck, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT, 0);

    for(uint32_t index = 0; index < RF_DEMO_CHANNEL_COUNT; ++index) {
        const int32_t x = (int32_t)index * RF_CHANNEL_CARD_WIDTH;
        lv_obj_t * button = create_box(deck, x, 0,
                                       RF_CHANNEL_CARD_WIDTH, RF_BOTTOM_HEIGHT,
                                       RF_COLOR_PANEL, LV_OPA_COVER);
        g_ui.selector_buttons[index] = button;
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, color(RF_COLOR_BORDER), 0);
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_radius(button, 0, 0);
        lv_obj_set_style_bg_color(button, color(RF_COLOR_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, selector_click_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);

        g_ui.selector_titles[index] = create_label(
            button, 8, 5, 36, 16, rf_demo_channels[index].id,
            &rf_font_12, RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        g_ui.selector_counts[index] = create_label(
            button, RF_CHANNEL_CARD_WIDTH - 76, 5, 68, 16,
            "空闲", &rf_font_zh_14,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        g_ui.selector_frequencies[index] = create_label(
            button, 8, 25, RF_CHANNEL_CARD_WIDTH - 16, 16, "", &rf_font_12,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text_fmt(g_ui.selector_frequencies[index], "%u MHz",
                              (unsigned)rf_demo_channels[index].center_mhz);
        g_ui.selector_bars[index] = create_box(button, 8, 43, 4, 3,
                                               RF_COLOR_BORDER, LV_OPA_COVER);
    }
}

static void create_history_bar(void)
{
    /* V27_3 moves transport and history controls into the header. */
}

static void create_sidebar(void)
{
    lv_obj_t * sidebar = create_box(g_ui.screen, RF_SIDEBAR_X, RF_SIDEBAR_Y,
                                    RF_SIDEBAR_WIDTH, RF_SIDEBAR_HEIGHT,
                                    RF_COLOR_PANEL, LV_OPA_COVER);
    g_ui.alert_banner = sidebar;
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_color(sidebar, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_radius(sidebar, 0, 0);

    lv_obj_t * title = create_box(sidebar, 0, 0, RF_SIDEBAR_WIDTH, 48,
                                  RF_COLOR_HEADER, LV_OPA_COVER);
    g_ui.detail_title_panel = title;
    lv_obj_set_style_border_width(title, 1, 0);
    lv_obj_set_style_border_color(title, color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_side(title, LV_BORDER_SIDE_BOTTOM, 0);
    g_ui.alert_prefix = create_box(title, 0, 0, 4, 48,
                                   RF_COLOR_MUTED, LV_OPA_COVER);
    create_label(title, 8, 4, 92, 16, "当前目标", &rf_font_zh_14,
                 RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    g_ui.alert_drone_label = create_label(title, 8, 24, 144, 20,
                                          "空闲", &rf_font_zh_14,
                                          RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);

    g_ui.side_channel_label = create_label(title, 122, 4, 30, 16, "--",
                                            &rf_font_12, RF_COLOR_MUTED,
                                            LV_TEXT_ALIGN_RIGHT);

    lv_obj_t * ranges = create_box(sidebar, 0, 48, RF_SIDEBAR_WIDTH, 62,
                                   RF_COLOR_PANEL, LV_OPA_COVER);
    g_ui.detail_ranges_panel = ranges;
    lv_obj_set_style_border_width(ranges, 1, 0);
    lv_obj_set_style_border_color(ranges, color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_side(ranges, LV_BORDER_SIDE_BOTTOM, 0);
    for(uint32_t index = 0U; index < 2U; ++index) {
        const int32_t y = 3 + (int32_t)index * 29;
        g_ui.detail_range_name_labels[index] = create_label(
            ranges, 8, y, 144, 14, "--", &rf_font_zh_14,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        g_ui.detail_range_value_labels[index] = create_label(
            ranges, 8, y + 14, 144, 13, "", &rf_font_10,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    }

    static const char * metric_names[3] = {
        "信号峰值", "噪声底", "信道占用"
    };
    for(uint32_t row = 0; row < 3U; ++row) {
        lv_obj_t * metric = create_box(sidebar, 0, 110 + (int32_t)row * 44,
                                       RF_SIDEBAR_WIDTH, 44,
                                       RF_COLOR_PANEL, LV_OPA_COVER);
        g_ui.detail_metric_panels[row] = metric;
        lv_obj_set_style_border_width(metric, 1, 0);
        lv_obj_set_style_border_color(metric, color(RF_COLOR_DIVIDER), 0);
        lv_obj_set_style_border_side(metric, LV_BORDER_SIDE_BOTTOM, 0);
        create_label(metric, 8, 13, 70, 18, metric_names[row],
                     &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        lv_obj_t * value = create_label(metric, 76, 13, 76, 18, "--",
                                         &rf_font_12, RF_COLOR_TEXT,
                                         LV_TEXT_ALIGN_RIGHT);
        g_ui.side_metric_labels[row] = value;
    }

    lv_obj_t * confidence = create_box(sidebar, 0, 242, RF_SIDEBAR_WIDTH, 58,
                                       RF_COLOR_PANEL, LV_OPA_COVER);
    g_ui.detail_confidence_panel = confidence;
    lv_obj_set_style_border_width(confidence, 1, 0);
    lv_obj_set_style_border_color(confidence, color(RF_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_side(confidence, LV_BORDER_SIDE_BOTTOM, 0);
    g_ui.alert_details = create_label(confidence, 8, 8, 88, 18,
                                      "识别置信度", &rf_font_zh_14,
                                      RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    g_ui.alert_idle_label = create_label(confidence, 100, 8, 52, 18, "--",
                                         &rf_font_zh_14, RF_COLOR_MUTED,
                                         LV_TEXT_ALIGN_RIGHT);
    g_ui.alert_badge = create_box(confidence, 10, 40, 140, 5,
                                  RF_COLOR_BORDER, LV_OPA_COVER);
    g_ui.alert_confidence_fill = create_box(g_ui.alert_badge, 0, 0, 0, 5,
                                            RF_COLOR_PRIMARY, LV_OPA_COVER);

    lv_obj_t * preview = create_box(sidebar, 0, 300,
                                    RF_SIDEBAR_WIDTH, 164,
                                    RF_COLOR_PANEL_ALT, LV_OPA_COVER);
    g_ui.detail_preview_panel = preview;
    lv_obj_t * image_frame = create_box(preview, 16, 26, 128, 112,
                                        0x343A3Eu, LV_OPA_COVER);
    g_ui.detail_image_frame = image_frame;
    lv_obj_set_style_border_width(image_frame, 1, 0);
    lv_obj_set_style_border_color(image_frame, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_radius(image_frame, 2, 0);
    g_ui.detail_image = lv_image_create(image_frame);
    reset_object(g_ui.detail_image);
    lv_image_set_src(g_ui.detail_image, g_target_detail_assets[0]);
    lv_obj_set_pos(g_ui.detail_image, 0, 0);
    lv_image_set_antialias(g_ui.detail_image, false);
    set_visible(g_ui.detail_image, false);
    g_ui.detail_empty_label = create_label(
        preview, 0, 73, RF_SIDEBAR_WIDTH, 18, "空闲", &rf_font_zh_14,
        RF_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
}

static void create_spectrum_panel(void)
{
    lv_obj_t * panel = create_panel(RF_SPECTRUM_Y, RF_SPECTRUM_HEIGHT);
    create_label(panel, RF_PLOT_X + 8, 1, 100, 16, "功率频谱",
                 &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);

    g_ui.spectrum_image = lv_image_create(panel);
    reset_object(g_ui.spectrum_image);
    lv_obj_set_pos(g_ui.spectrum_image, RF_PLOT_X, 20);
    lv_image_set_src(g_ui.spectrum_image,
                     &g_spectrum_image_dsc[g_spectrum_active_source]);
    lv_image_set_pivot(g_ui.spectrum_image, 0, 0);
    lv_image_set_scale_x(g_ui.spectrum_image,
                         (RF_SPECTRUM_DISPLAY_WIDTH * LV_SCALE_NONE) /
                         RF_SPECTRUM_TEXTURE_WIDTH);
    lv_image_set_scale_y(g_ui.spectrum_image,
                         (RF_SPECTRUM_DISPLAY_HEIGHT * LV_SCALE_NONE) /
                         RF_SPECTRUM_TEXTURE_HEIGHT);
    lv_image_set_antialias(g_ui.spectrum_image, false);
    for(uint32_t index = 0U; index < RF_UI_MAX_RF_BOXES; ++index) {
        g_ui.spectrum_rf_boxes[index] = create_rf_box_overlay(panel);
    }

    static const char * const db_labels[5] = {
        "-30", "-45", "-60", "-75", "-90"
    };
    static const int32_t y_positions[5] = {13, 30, 46, 63, 79};
    static const int32_t x_positions[5] = {
        RF_PLOT_X, 246, 428, 610, 792
    };
    for(uint32_t index = 0U; index < 5U; ++index) {
        create_label(panel, 22, y_positions[index], 36, 14,
                     db_labels[index], &rf_font_12,
                     RF_COLOR_AXIS, LV_TEXT_ALIGN_RIGHT);
        const lv_text_align_t alignment = index == 0U ? LV_TEXT_ALIGN_LEFT :
            (index == 4U ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER);
        g_ui.spectrum_frequency_labels[index] = create_label(
            panel, x_positions[index], 88, 72, 14, "", &rf_font_12,
            index == 4U ? RF_COLOR_PRIMARY : RF_COLOR_AXIS, alignment);
    }
}

static void create_waterfall_panel(void)
{
    lv_obj_t * panel = create_panel(RF_WATERFALL_Y, RF_WATERFALL_HEIGHT);
    create_label(panel, 8, 7, 96, 18, "信号瀑布", &rf_font_zh_14,
                 RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    g_ui.waterfall_channel_label = create_label(panel, 108, 7, 150, 18, "",
                                                 &rf_font_12, RF_COLOR_PRIMARY,
                                                 LV_TEXT_ALIGN_LEFT);
    g_ui.waterfall_history_label = create_label(
        panel, 520, 7, 336, 18, "160 RF ROW | 0.615 ms/COL | 98.39 ms",
        &rf_font_zh_14,
        RF_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);

    g_ui.waterfall_image = lv_image_create(panel);
    reset_object(g_ui.waterfall_image);
    lv_obj_set_pos(g_ui.waterfall_image, RF_PLOT_X, 36);
    lv_image_set_src(g_ui.waterfall_image,
                     &g_waterfall_image_dsc[g_waterfall_active_source]);
    lv_image_set_pivot(g_ui.waterfall_image, 0, 0);
    lv_image_set_scale_x(g_ui.waterfall_image, LV_SCALE_NONE);
    lv_image_set_scale_y(g_ui.waterfall_image, LV_SCALE_NONE);
    lv_image_set_antialias(g_ui.waterfall_image, false);
    /* Layer 2 owns the pixels while this transparent LVGL image remains the
     * touch target for pause and history gestures. */
    if(g_waterfall_overlay.requested) {
        lv_obj_set_style_opa(g_ui.waterfall_image, LV_OPA_TRANSP, 0);
    }
    lv_obj_add_flag(g_ui.waterfall_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_ui.waterfall_image, waterfall_pan_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_ui.waterfall_image, waterfall_pan_event,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_ui.waterfall_image, waterfall_pan_event,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_ui.waterfall_image, waterfall_pan_event,
                        LV_EVENT_PRESS_LOST, NULL);
    for(uint32_t index = 0U; index < RF_UI_MAX_RF_BOXES; ++index) {
        g_ui.waterfall_rf_boxes[index] = create_rf_box_overlay(panel);
    }

    static const int32_t y_positions[5] = {29, 92, 155, 218, 281};
    static const int32_t x_positions[5] = {
        RF_PLOT_X, 246, 428, 610, 792
    };
    static const char * const time_labels[5] = {
        "-98 ms", "-74 ms", "-49 ms", "-25 ms", "当前"
    };
    for(uint32_t index = 0U; index < 5U; ++index) {
        g_ui.waterfall_frequency_labels[index] = create_label(
            panel, 22, y_positions[index], 36, 14, "", &rf_font_12,
            RF_COLOR_AXIS, LV_TEXT_ALIGN_RIGHT);
        const lv_text_align_t alignment = index == 0U ? LV_TEXT_ALIGN_LEFT :
            (index == 4U ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER);
        g_ui.waterfall_time_labels[index] = create_label(
            panel, x_positions[index], 290, 72, 14, time_labels[index],
            index == 4U ? &rf_font_zh_14 : &rf_font_12,
            index == 4U ? RF_COLOR_PRIMARY : RF_COLOR_AXIS, alignment);
    }
}

static void create_compare_overlay(void)
{
    g_ui.compare_overlay = create_box(g_ui.screen, 0, 0,
                                      RF_SCREEN_WIDTH, RF_SCREEN_HEIGHT,
                                      RF_COLOR_SCREEN, LV_OPA_COVER);
    /* Keep the full-screen overlay modal even though decorative boxes no
     * longer participate in hit testing. */
    lv_obj_add_flag(g_ui.compare_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * header = create_box(g_ui.compare_overlay, 0, 0,
                                   RF_SCREEN_WIDTH, 64,
                                   RF_COLOR_HEADER, LV_OPA_COVER);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    create_label(header, 16, 10, 180, 22, "多目标对比", &rf_font_zh_14,
                 RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    create_label(header, 16, 36, 280, 18, "同一检测帧 | 最多 4 个目标",
                 &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_t * close = create_box(header, 964, 10, RF_TOUCH_TARGET,
                                  RF_TOUCH_TARGET, RF_COLOR_PANEL_ALT,
                                  LV_OPA_COVER);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, color(RF_COLOR_BORDER), 0);
    lv_obj_set_style_radius(close, 3, 0);
    lv_obj_set_style_bg_color(close, color(RF_COLOR_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, compare_close_event, LV_EVENT_CLICKED, NULL);
    create_label(close, 0, 13, RF_TOUCH_TARGET, 18, LV_SYMBOL_CLOSE,
                 &lv_font_montserrat_14, RF_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);

    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        const int32_t column = (int32_t)(index & 1U);
        const int32_t row = (int32_t)(index >> 1U);
        const int32_t x = 16 + column * 504;
        const int32_t y = 80 + row * 248;
        lv_obj_t * card = create_box(g_ui.compare_overlay, x, y, 488, 232,
                                     RF_COLOR_PANEL, LV_OPA_COVER);
        g_ui.compare_cards[index] = card;
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, color(RF_COLOR_BORDER), 0);
        lv_obj_set_style_radius(card, 4, 0);
        lv_obj_set_style_bg_color(card, color(RF_COLOR_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, compare_target_click_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)index);

        char number[4];
        snprintf(number, sizeof(number), "%02u", (unsigned)index + 1U);
        lv_obj_t * badge = create_label(card, 14, 14, 48, 48, number,
                                        &rf_font_18, RF_COLOR_PRIMARY,
                                        LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_bg_color(badge, color(RF_COLOR_PRIMARY_SOFT), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 4, 0);
        g_ui.compare_names[index] = create_label(
            card, 78, 14, 286, 22, g_target_display_names[index],
            &rf_font_zh_14, RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        create_label(card, 78, 42, 286, 18, g_target_class_names[index],
                     &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        g_ui.compare_states[index] = create_label(
            card, 376, 14, 96, 20, "空闲", &rf_font_zh_14,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        g_ui.compare_channels[index] = create_label(
            card, 16, 86, 456, 20, "当前空闲", &rf_font_zh_14,
            RF_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        g_ui.compare_confidences[index] = create_label(
            card, 16, 116, 456, 20, "置信度 空闲", &rf_font_zh_14,
            RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        create_label(card, 16, 158, 456, 18,
                     "点击目标后联动通道与重点锁定",
                     &rf_font_zh_14, RF_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        create_label(card, 16, 196, 456, 18,
                     "身份与频段解耦 | 同通道允许多个目标",
                     &rf_font_zh_14, RF_COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    }
    set_visible(g_ui.compare_overlay, false);
}

static uint32_t channel_switch_next_revision(uint32_t revision)
{
    revision++;
    return revision == 0U ? 1U : revision;
}

static bool complete_spectrum_snapshot_ready(uint32_t channel)
{
    if(channel >= RF_UI_CHANNEL_COUNT) return false;
    const rf_ui_complete_window_t * const window =
        &g_complete_windows[channel];
    return window->valid && window->spectrum_snapshot_valid &&
           window->spectrum_revision != 0U;
}

static void complete_spectrum_snapshot_try_capture(uint32_t channel)
{
    if(channel >= RF_UI_CHANNEL_COUNT) return;
    const rf_ui_spectrum_identity_t * const spectrum =
        &g_spectrum_identity[channel];
    rf_ui_complete_window_t * const window =
        &g_complete_windows[channel];
    if(!spectrum->valid || !window->valid ||
       spectrum->session_id != window->session_id ||
       spectrum->window_sequence != window->window_sequence) {
        return;
    }

    memcpy(g_complete_spectrum_data[channel],
           g_spectrum_data[channel],
           sizeof(g_complete_spectrum_data[channel]));
    window->spectrum_revision = spectrum->revision;
    window->spectrum_snapshot_valid = true;
}

static bool waterfall_history_snapshot(uint32_t channel,
                                       uint64_t * total_columns,
                                       uint16_t * write_head)
{
    if(channel >= RF_UI_CHANNEL_COUNT || total_columns == NULL ||
       write_head == NULL) return false;

    const uint64_t total_before = g_waterfall_total_columns[channel];
    const uint16_t head = g_waterfall_write_head[channel];
    const uint64_t total_after = g_waterfall_total_columns[channel];
    if(total_before != total_after ||
       head != (uint16_t)(total_after % RF_UI_WATERFALL_HISTORY_COLS)) {
        return false;
    }

    *total_columns = total_after;
    *write_head = head;
    return true;
}

static bool waterfall_history_head_matches(uint16_t source_head,
                                           uint64_t column_delta,
                                           uint16_t target_head)
{
    return target_head == (uint16_t)(
        (source_head + column_delta) % RF_UI_WATERFALL_HISTORY_COLS);
}

static bool channel_switch_revision_reached(uint32_t revision,
                                            uint32_t required)
{
    return (int32_t)(revision - required) >= 0;
}

static void channel_switch_set_state(rf_ui_channel_switch_state_t state)
{
    g_channel_build.state = state;
    g_rf_ui_channel_switch_diag.state = (uint32_t)state;
}

static void channel_switch_record_chunk(uint32_t bytes_written,
                                        uint32_t rows_written)
{
    g_rf_ui_channel_switch_diag.build_chunks++;
    g_rf_ui_channel_switch_diag.build_rows += rows_written;
    g_rf_ui_channel_switch_diag.last_chunk_bytes = bytes_written;
    if(bytes_written > g_rf_ui_channel_switch_diag.max_chunk_bytes) {
        g_rf_ui_channel_switch_diag.max_chunk_bytes = bytes_written;
    }
}

static bool channel_switch_window_ready(uint32_t channel)
{
    const rf_ui_complete_window_t * window;
    if(channel >= RF_UI_CHANNEL_COUNT) return false;

    window = &g_complete_windows[channel];
    return complete_spectrum_snapshot_ready(channel) &&
           channel_switch_revision_reached(
               window->spectrum_revision,
               g_channel_build.required_spectrum_revision) &&
           channel_switch_revision_reached(
               window->revision,
               g_channel_build.required_window_revision);
}

static bool channel_switch_build_start(bool restart)
{
    const uint32_t channel = g_ui.pending_channel;
    const uint32_t request_generation = g_channel_build.request_generation;
    const uint32_t required_spectrum_revision =
        g_channel_build.required_spectrum_revision;
    const uint32_t required_window_revision =
        g_channel_build.required_window_revision;
    const rf_ui_complete_window_t window =
        channel < RF_UI_CHANNEL_COUNT ? g_complete_windows[channel] :
        (rf_ui_complete_window_t){0};
    uint64_t latest_total_columns;
    uint16_t latest_write_head;

    if(!channel_switch_window_ready(channel) ||
       !waterfall_history_snapshot(channel,
                                   &latest_total_columns,
                                   &latest_write_head)) {
        channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WAIT_WINDOW);
        return false;
    }

    const uint8_t source = (uint8_t)(g_waterfall_active_source ^ 1U);
    const rf_ui_waterfall_source_state_t cached_state =
        g_waterfall_source_state[source];
    bool reuse_waterfall_cache = false;
    if(cached_state.valid && cached_state.channel == channel) {
        if(latest_total_columns >= cached_state.total_columns &&
           (latest_total_columns - cached_state.total_columns) <=
               RF_UI_WATERFALL_HISTORY_COLS &&
           waterfall_history_head_matches(
               cached_state.history_head,
               latest_total_columns - cached_state.total_columns,
               latest_write_head)) {
            reuse_waterfall_cache = true;
        }
        else {
            g_rf_ui_channel_switch_diag.switch_cache_stale_misses++;
        }
    }

    memset(&g_channel_build, 0, sizeof(g_channel_build));
    g_channel_build.channel = (uint8_t)channel;
    g_channel_build.source = source;
    g_channel_build.waterfall_cache_reused = reuse_waterfall_cache;
    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        g_waterfall_overlay.boxes_dirty[g_channel_build.source] = true;
    }
    waterfall_source_state_invalidate(g_channel_build.source);
    g_channel_build.request_generation = request_generation;
    g_channel_build.required_spectrum_revision = required_spectrum_revision;
    g_channel_build.required_window_revision = required_window_revision;
    g_channel_build.spectrum_revision = window.spectrum_revision;
    g_channel_build.session_id = window.session_id;
    g_channel_build.window_sequence = window.window_sequence;
    g_channel_build.base_write_head = reuse_waterfall_cache ?
        cached_state.history_head : latest_write_head;
    g_channel_build.logical_start = (uint16_t)(
        (g_channel_build.base_write_head + RF_UI_WATERFALL_HISTORY_COLS -
         RF_UI_WATERFALL_COLS) % RF_UI_WATERFALL_HISTORY_COLS);
    g_channel_build.base_total_columns = reuse_waterfall_cache ?
        cached_state.total_columns : latest_total_columns;
    g_channel_build.caught_up_total_columns =
        g_channel_build.base_total_columns;
    g_channel_build.catchup_source_head =
        g_channel_build.base_write_head;
    g_channel_build.render_write_column = reuse_waterfall_cache ?
        cached_state.render_write_column : 0U;
    memcpy(g_channel_build.spectrum_snapshot,
           g_complete_spectrum_data[channel],
           sizeof(g_channel_build.spectrum_snapshot));
    spectrum_prepare_geometry(g_channel_build.spectrum_snapshot,
                              g_channel_build.spectrum_x,
                              g_channel_build.spectrum_y,
                              &g_channel_build.spectrum_peak_index);
    channel_switch_set_state(RF_UI_CHANNEL_SWITCH_SPECTRUM_BASE);

    g_rf_ui_channel_switch_diag.build_starts++;
    if(reuse_waterfall_cache) {
        g_rf_ui_channel_switch_diag.switch_cache_hits++;
    }
    else {
        g_rf_ui_channel_switch_diag.switch_cache_misses++;
    }
    if(restart) g_rf_ui_channel_switch_diag.build_restarts++;
    g_rf_ui_channel_switch_diag.build_channel = channel;
    g_rf_ui_channel_switch_diag.last_session_id = window.session_id;
    g_rf_ui_channel_switch_diag.last_window_sequence =
        window.window_sequence;
    return true;
}

static void channel_switch_restart(void)
{
    if(!channel_switch_build_start(true)) {
        g_channel_build.channel = g_ui.pending_channel;
        g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
    }
}

static uint32_t waterfall_render_catchup_row_at(
    rf_ui_waterfall_rgb565_ring_t * target,
    uint32_t channel,
    uint32_t render_y,
    uint32_t source_row,
    uint32_t source_head,
    uint32_t render_head,
    uint32_t column_count)
{
    uint16_t * const render_row = target->rows[render_y];
    const uint16_t * const history_row =
        g_waterfall_rings[channel].rows[source_row];
    uint32_t bytes_written = 0U;

    for(uint32_t offset = 0U; offset < column_count; ++offset) {
        const uint32_t source_column =
            (source_head + offset) %
            RF_UI_WATERFALL_HISTORY_COLS;
        const uint32_t render_column =
            (render_head + offset) %
            RF_UI_WATERFALL_COLS;
        const uint32_t render_start =
            g_waterfall_render_x[render_column];
        const uint32_t render_width =
            (uint32_t)g_waterfall_render_x[render_column + 1U] -
            render_start;
        const uint16_t pixel = history_row[source_column];
        fill_row(&render_row[render_start], pixel, render_width);
        fill_row(&render_row[render_start + RF_WATERFALL_DISPLAY_WIDTH],
                 pixel, render_width);
        bytes_written += render_width * sizeof(uint16_t) * 2U;
    }
    return bytes_written;
}

static uint32_t waterfall_catchup_rows_per_step(uint32_t render_head,
                                                uint32_t column_count,
                                                uint32_t remaining_rows)
{
    uint32_t row_bytes = 0U;
    for(uint32_t offset = 0U; offset < column_count; ++offset) {
        const uint32_t render_column =
            (render_head + offset) % RF_UI_WATERFALL_COLS;
        const uint32_t render_width =
            (uint32_t)g_waterfall_render_x[render_column + 1U] -
            g_waterfall_render_x[render_column];
        row_bytes += render_width * sizeof(uint16_t) * 2U;
    }
    if(row_bytes == 0U) return 0U;

    uint32_t rows = RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES / row_bytes;
    if(rows == 0U) rows = 1U;
    if(rows > remaining_rows) rows = remaining_rows;
    return rows;
}

static uint32_t waterfall_overlay_catchup_rows_per_step(
    uint32_t column_count,
    uint32_t remaining_rows)
{
    const uint32_t row_bytes = column_count *
        RF_WATERFALL_CLUT_COLUMN_WRITE_BYTES;
    if(row_bytes == 0U) return 0U;
    uint32_t rows = RF_CHANNEL_SWITCH_CATCHUP_MAX_BYTES / row_bytes;
    if(rows == 0U) rows = 1U;
    if(rows > RF_WATERFALL_OVERLAY_CATCHUP_MAX_ROWS_PER_TICK) {
        rows = RF_WATERFALL_OVERLAY_CATCHUP_MAX_ROWS_PER_TICK;
    }
    if(rows > remaining_rows) rows = remaining_rows;
    return rows;
}

static void waterfall_overlay_sync_cancel(void)
{
    memset(&g_waterfall_overlay_sync, 0,
           sizeof(g_waterfall_overlay_sync));
}

static bool waterfall_overlay_sync_start(uint32_t channel)
{
    if(g_waterfall_overlay_sync.active ||
       channel >= RF_UI_CHANNEL_COUNT || !g_ui.running ||
       !g_waterfall_overlay.requested || g_waterfall_overlay.failed) {
        return false;
    }

    const uint8_t source = g_waterfall_active_source;
    rf_ui_waterfall_source_state_t * const source_state =
        &g_waterfall_source_state[source];
    const uint64_t target_total = g_waterfall_total_columns[channel];
    if(!source_state->valid || source_state->channel != channel ||
       target_total <= source_state->total_columns ||
       (target_total - source_state->total_columns) >
           RF_UI_WATERFALL_HISTORY_COLS) {
        return false;
    }

    memset(&g_waterfall_overlay_sync, 0,
           sizeof(g_waterfall_overlay_sync));
    g_waterfall_overlay_sync.active = true;
    g_waterfall_overlay_sync.channel = (uint8_t)channel;
    g_waterfall_overlay_sync.source = source;
    g_waterfall_overlay_sync.source_head = source_state->history_head;
    g_waterfall_overlay_sync.target_head =
        g_waterfall_write_head[channel];
    g_waterfall_overlay_sync.source_total = source_state->total_columns;
    g_waterfall_overlay_sync.target_total = target_total;
    g_rf_ui_channel_switch_diag.overlay_sync_starts++;
    return true;
}

static bool waterfall_overlay_sync_step(void)
{
    if(!g_waterfall_overlay_sync.active) return false;

    const uint8_t source = g_waterfall_overlay_sync.source;
    const uint8_t channel = g_waterfall_overlay_sync.channel;
    rf_ui_waterfall_source_state_t * const source_state =
        &g_waterfall_source_state[source];
    if(!g_ui.running || source != g_waterfall_active_source ||
       channel != g_ui.committed_channel ||
       !g_waterfall_overlay.requested || g_waterfall_overlay.failed ||
       !source_state->valid || source_state->channel != channel ||
       source_state->total_columns !=
           g_waterfall_overlay_sync.source_total ||
       source_state->history_head != g_waterfall_overlay_sync.source_head) {
        waterfall_overlay_sync_cancel();
        return false;
    }

    const uint32_t column_count = (uint32_t)(
        g_waterfall_overlay_sync.target_total -
        g_waterfall_overlay_sync.source_total);
    const uint32_t remaining_rows = RF_WATERFALL_DISPLAY_HEIGHT -
        g_waterfall_overlay_sync.render_y;
    const uint32_t rows_this_step =
        waterfall_overlay_catchup_rows_per_step(column_count,
                                                remaining_rows);
    uint32_t rows_written = 0U;
    uint32_t bytes_written = 0U;
    while(g_waterfall_overlay_sync.render_y <
              RF_WATERFALL_DISPLAY_HEIGHT &&
          rows_written < rows_this_step) {
        const uint32_t render_y = g_waterfall_overlay_sync.render_y;
        const uint32_t source_row =
            g_waterfall_render_source_row[render_y];
        bytes_written += waterfall_overlay_catchup_row(
            source, channel, render_y, source_row,
            g_waterfall_overlay_sync.source_head, column_count);
        g_waterfall_overlay_sync.render_y++;
        rows_written++;
    }

    g_rf_ui_channel_switch_diag.overlay_sync_chunks++;
    g_rf_ui_channel_switch_diag.overlay_sync_rows += rows_written;
    g_rf_ui_channel_switch_diag.overlay_sync_last_chunk_bytes =
        bytes_written;
    if(bytes_written >
       g_rf_ui_channel_switch_diag.overlay_sync_max_chunk_bytes) {
        g_rf_ui_channel_switch_diag.overlay_sync_max_chunk_bytes =
            bytes_written;
    }

    if(g_waterfall_overlay_sync.render_y ==
       RF_WATERFALL_DISPLAY_HEIGHT) {
        source_state->history_head = g_waterfall_overlay_sync.target_head;
        source_state->total_columns = g_waterfall_overlay_sync.target_total;
        source_state->render_write_column = (uint16_t)(
            (source_state->render_write_column + column_count) %
            RF_UI_WATERFALL_COLS);
        g_waterfall_overlay.boxes_dirty[source] = true;
        g_waterfall_overlay.visual_dirty = true;
        g_rf_ui_channel_switch_diag.overlay_sync_completions++;
        waterfall_overlay_sync_cancel();
    }
    return true;
}

static uint32_t waterfall_render_catchup_row(
    rf_ui_waterfall_rgb565_ring_t * target,
    uint32_t channel,
    uint32_t render_y,
    uint32_t source_row,
    uint32_t column_count)
{
    return waterfall_render_catchup_row_at(
        target, channel, render_y, source_row,
        g_channel_build.catchup_source_head,
        g_channel_build.render_write_column,
        column_count);
}

static int8_t channel_switch_target_detection_index(uint32_t channel)
{
    if(g_ui.pending_detection_index >= 0 &&
       g_ui.pending_detection_index < (int8_t)RF_UI_DETECTION_COUNT &&
       g_detections[(uint32_t)g_ui.pending_detection_index].channel_index ==
           channel) {
        return g_ui.pending_detection_index;
    }
    return -1;
}

static bool channel_switch_defer_metadata_refresh(void)
{
    if(!g_render_txn.active ||
       g_render_txn.kind != RF_UI_RENDER_CHANNEL_SWITCH) {
        return false;
    }

    /* Updates before stage 0 are included in the staged view. Once staging
     * starts, freeze that pass and coalesce later updates after the commit. */
    if((g_render_txn.metadata_stage != 0U ||
        g_render_txn.commit_queued) &&
       !g_render_txn.metadata_refresh_pending) {
        g_render_txn.metadata_refresh_pending = true;
        g_rf_ui_channel_switch_diag.switch_metadata_refresh_deferrals++;
    }
    return true;
}

static bool channel_switch_stage_metadata_step(void)
{
    const uint32_t channel = g_channel_build.channel;
    if(channel >= RF_UI_CHANNEL_COUNT ||
       channel != g_ui.pending_channel ||
       g_channel_build.request_generation !=
           g_rf_ui_channel_switch_diag.request_generation) {
        return false;
    }

    if(g_render_txn.metadata_stage == 0U) {
        g_render_txn.staged_selected_detection_index =
            channel_switch_target_detection_index(channel);
        if(g_render_txn.staged_selected_detection_index >= 0 &&
           g_ui.pending_detection_index ==
               g_render_txn.staged_selected_detection_index) {
            g_ui.pending_detection_index = -1;
        }
    }
    const int8_t target_detection =
        g_render_txn.staged_selected_detection_index;

    const uint8_t saved_committed_channel = g_ui.committed_channel;
    const uint8_t saved_pending_channel = g_ui.pending_channel;
    const int8_t saved_selected_detection =
        g_ui.selected_detection_index;
    const int8_t saved_pending_detection =
        g_ui.pending_detection_index;

    /* Populate the hidden framebuffer with target metadata while the public
     * committed state and the scanned-out framebuffer still describe the old
     * channel.  Each group is refreshed in a separate owner-thread tick. */
    g_ui.committed_channel = (uint8_t)channel;
    g_ui.pending_channel = (uint8_t)channel;
    g_ui.selected_detection_index = target_detection;
    g_ui.pending_detection_index = -1;

    const rf_demo_channel_t * const target = &rf_demo_channels[channel];
    switch(g_render_txn.metadata_stage) {
        case 0U:
            if(g_ui.selected_channel_label != NULL) {
                lv_label_set_text_fmt(g_ui.selected_channel_label,
                                      "%s | %u MHz", target->id,
                                      (unsigned)target->center_mhz);
            }
            lv_label_set_text_fmt(g_ui.waterfall_channel_label,
                                  "%s | %u MHz", target->id,
                                  (unsigned)target->center_mhz);
            for(uint32_t index = 0U;
                index < RF_CHANNEL_METRIC_COUNT; ++index) {
                refresh_selected_metric(index);
            }
            set_frequency_labels(g_ui.spectrum_frequency_labels,
                                 target->center_mhz);
            set_waterfall_frequency_labels(g_ui.waterfall_frequency_labels,
                                           target->center_mhz);
            break;
        case 1U:
            refresh_selector_styles();
            refresh_header_status();
            break;
        case 2U:
            refresh_target_detail();
            g_render_txn.staged_selected_detection_index =
                g_ui.selected_detection_index;
            refresh_target_cards();
            break;
        case 3U:
            refresh_rf_box_overlays();
            refresh_compare_overlay();
            break;
        default:
            break;
    }

    g_ui.committed_channel = saved_committed_channel;
    g_ui.pending_channel = saved_pending_channel;
    g_ui.selected_detection_index = saved_selected_detection;
    g_ui.pending_detection_index = saved_pending_detection;
    g_render_txn.metadata_stage++;
    g_rf_ui_channel_switch_diag.switch_metadata_stage_steps++;
    return true;
}

static bool channel_switch_commit(void)
{
    const uint32_t channel = g_channel_build.channel;
    const uint8_t source = g_channel_build.source;
    const uint64_t target_total_columns =
        g_channel_build.caught_up_total_columns;
    const uint32_t target_spectrum_revision =
        g_channel_build.spectrum_revision;
    const int8_t target_detection =
        g_render_txn.staged_selected_detection_index;
    if(channel >= RF_UI_CHANNEL_COUNT ||
       channel != g_ui.pending_channel ||
       g_channel_build.request_generation !=
           g_rf_ui_channel_switch_diag.request_generation ||
       g_render_txn.metadata_stage !=
           RF_CHANNEL_SWITCH_METADATA_STAGE_COUNT) {
        return false;
    }

    g_spectrum_active_source = source;
    g_waterfall_active_source = source;
    g_waterfall_render_write_column =
        g_channel_build.render_write_column;
    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        g_waterfall_overlay.visual_dirty = true;
        g_waterfall_overlay.boxes_dirty[source] = true;
        if(g_waterfall_overlay.display_source != source) {
            g_rf_ui_channel_switch_diag.overlay_source_switches++;
        }
    }
    else {
        waterfall_image_head_update();
    }

    g_ui.committed_channel = (uint8_t)channel;
    g_ui.pending_channel = (uint8_t)channel;
    g_ui.selected_detection_index = target_detection;
    g_ui.spectrum_dirty[channel] =
        g_spectrum_identity[channel].revision != target_spectrum_revision;
    g_waterfall_presented_columns[channel] = target_total_columns;
    g_ui.waterfall_dirty[channel] =
        g_waterfall_total_columns[channel] != target_total_columns;
    g_ui.rf_boxes_dirty[channel] = rf_box_batch_has_anchor_after(
        &g_rf_box_batches[channel], target_total_columns);
    waterfall_source_state_commit(
        source, channel, target_total_columns,
        g_channel_build.catchup_source_head,
        g_channel_build.render_write_column);

    /* Descriptors and every metadata object were already drawn into the
     * staged framebuffer.  Finalization only publishes the matching state;
     * it must not add more invalidations to the commit tick. */

    g_rf_ui_channel_switch_diag.build_completions++;
    g_rf_ui_channel_switch_diag.atomic_commits++;
    const uint32_t commit_line_event = g_display_diag.glcdc_line_events;
    const uint32_t latency_line_events = commit_line_event -
        g_rf_ui_channel_switch_diag.switch_request_line_event;
    g_rf_ui_channel_switch_diag.switch_commit_line_event = commit_line_event;
    g_rf_ui_channel_switch_diag.switch_last_latency_line_events =
        latency_line_events;
    if(latency_line_events >
       g_rf_ui_channel_switch_diag.switch_max_latency_line_events) {
        g_rf_ui_channel_switch_diag.switch_max_latency_line_events =
            latency_line_events;
    }
    g_rf_ui_channel_switch_diag.pending_channel = channel;
    g_rf_ui_channel_switch_diag.committed_channel = channel;
    g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
    g_rf_ui_channel_switch_diag.active_source = source;
    g_channel_build.channel = UINT8_MAX;
    channel_switch_set_state(RF_UI_CHANNEL_SWITCH_IDLE);
    return true;
}

static void render_transaction_reset(void)
{
    memset(&g_render_txn, 0, sizeof(g_render_txn));
    g_render_txn.kind = RF_UI_RENDER_NONE;
}

static void render_transaction_restore_sources(void)
{
    if(!g_waterfall_overlay.requested || g_waterfall_overlay.failed) {
        waterfall_image_head_update();
        (void)waterfall_image_source_rebind(g_waterfall_active_source);
    }
    else {
        g_waterfall_overlay.visual_dirty = true;
    }
    (void)spectrum_image_source_rebind(g_spectrum_active_source);
}

static void render_transaction_abort(void)
{
    lv_display_t * const display = lv_display_get_default();
    if(!g_render_txn.active) return;

    if(display != NULL && lv_display_deferred_is_active(display)) {
        g_display_diag.lvgl_deferred_aborts++;
        lv_display_deferred_abort(display);
    }
    const bool committed_metadata = g_render_txn.commit_queued;
    const bool channel_switch =
        g_render_txn.kind == RF_UI_RENDER_CHANNEL_SWITCH;
    const bool staged_metadata = g_render_txn.metadata_stage != 0U ||
        g_render_txn.metadata_refresh_pending;
    const uint32_t channel = g_render_txn.channel;
    const bool dirty_waterfall = channel < RF_UI_CHANNEL_COUNT &&
        g_ui.waterfall_dirty[channel];
    const bool dirty_spectrum = channel < RF_UI_CHANNEL_COUNT &&
        g_ui.spectrum_dirty[channel];
    const bool dirty_boxes = channel < RF_UI_CHANNEL_COUNT &&
        g_ui.rf_boxes_dirty[channel];

    if(committed_metadata) {
        g_waterfall_active_source =
            g_render_txn.previous_waterfall_source;
        g_spectrum_active_source = g_render_txn.previous_spectrum_source;
        g_waterfall_render_write_column =
            g_render_txn.previous_render_write_column;
        if(channel < RF_UI_CHANNEL_COUNT) {
            g_waterfall_presented_columns[channel] =
                g_render_txn.previous_presented_columns;
            g_ui.waterfall_dirty[channel] =
                g_render_txn.previous_waterfall_dirty || dirty_waterfall;
            g_ui.spectrum_dirty[channel] =
                g_render_txn.previous_spectrum_dirty || dirty_spectrum;
            g_ui.rf_boxes_dirty[channel] =
                g_render_txn.previous_rf_boxes_dirty || dirty_boxes;
        }
        g_ui.committed_channel = g_render_txn.previous_committed_channel;
        if(g_ui.pending_channel == channel) {
            g_ui.pending_channel = g_render_txn.previous_pending_channel;
        }
        g_ui.selected_detection_index =
            g_render_txn.previous_selected_detection_index;
        g_rf_ui_channel_switch_diag.committed_channel =
            g_ui.committed_channel;
        g_rf_ui_channel_switch_diag.pending_channel =
            g_ui.pending_channel;
    }
    if(channel_switch && g_ui.pending_detection_index < 0) {
        g_ui.pending_detection_index =
            g_render_txn.previous_pending_detection_index;
    }
    if(staged_metadata) {
        refresh_selected_view();
        refresh_compare_overlay();
    }
    render_transaction_restore_sources();
    render_transaction_reset();
}

static void render_transaction_poll_complete(void)
{
    lv_display_t * const display = lv_display_get_default();
    if(!g_render_txn.active || !g_render_txn.commit_queued ||
       (display != NULL && lv_display_deferred_is_active(display))) {
        return;
    }
    if(g_display_diag.lvgl_deferred_aborts !=
       g_render_txn.deferred_abort_baseline) {
        render_transaction_abort();
        if(g_ui.pending_channel < RF_UI_CHANNEL_COUNT &&
           g_ui.pending_channel != g_ui.committed_channel) {
            const uint32_t retry_channel = g_ui.pending_channel;
            memset(&g_channel_build, 0, sizeof(g_channel_build));
            g_channel_build.channel = (uint8_t)retry_channel;
            g_channel_build.request_generation =
                g_rf_ui_channel_switch_diag.request_generation;
            g_channel_build.required_spectrum_revision =
                channel_switch_next_revision(
                    g_spectrum_identity[retry_channel].revision);
            g_channel_build.required_window_revision =
                channel_switch_next_revision(
                    g_complete_windows[retry_channel].revision);
            g_rf_ui_channel_switch_diag.build_restarts++;
            channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WAIT_WINDOW);
        }
        return;
    }

    const bool channel_switch =
        g_render_txn.kind == RF_UI_RENDER_CHANNEL_SWITCH;
    bool refresh_metadata = g_render_txn.metadata_refresh_pending;
    if(channel_switch && g_ui.pending_detection_index >= 0) {
        const int8_t pending_detection =
            channel_switch_target_detection_index(g_ui.committed_channel);
        if(pending_detection >= 0 &&
           pending_detection != g_ui.selected_detection_index) {
            g_ui.selected_detection_index = pending_detection;
            refresh_metadata = true;
        }
        g_ui.pending_detection_index = -1;
    }
    render_transaction_reset();

    /* The old framebuffer may still be resynchronizing. LVGL retains these
     * invalidations until that bounded peer-buffer copy is complete. */
    if(channel_switch && refresh_metadata) {
        refresh_selected_view();
        refresh_compare_overlay();
        g_rf_ui_channel_switch_diag.switch_metadata_post_commit_refreshes++;
    }
}

static bool render_transaction_begin(rf_ui_render_kind_t kind,
                                     uint32_t channel,
                                     uint8_t source,
                                     uint16_t render_write_column,
                                     uint64_t target_total_columns)
{
    lv_display_t * const display = lv_display_get_default();
    render_transaction_poll_complete();
    if(g_render_txn.active || display == NULL ||
       !lv_display_deferred_begin(display)) {
        return false;
    }

    memset(&g_render_txn, 0, sizeof(g_render_txn));
    g_render_txn.kind = kind;
    g_render_txn.active = true;
    g_render_txn.channel = (uint8_t)channel;
    g_render_txn.source = source;
    g_render_txn.previous_waterfall_source = g_waterfall_active_source;
    g_render_txn.previous_spectrum_source = g_spectrum_active_source;
    g_render_txn.previous_committed_channel = g_ui.committed_channel;
    g_render_txn.previous_pending_channel = g_ui.pending_channel;
    g_render_txn.render_write_column = render_write_column;
    g_render_txn.previous_render_write_column =
        g_waterfall_render_write_column;
    g_render_txn.previous_selected_detection_index =
        g_ui.selected_detection_index;
    g_render_txn.previous_pending_detection_index =
        g_ui.pending_detection_index;
    g_render_txn.deferred_abort_baseline =
        g_display_diag.lvgl_deferred_aborts;
    if(channel < RF_UI_CHANNEL_COUNT) {
        g_render_txn.previous_waterfall_dirty =
            g_ui.waterfall_dirty[channel];
        g_render_txn.previous_spectrum_dirty =
            g_ui.spectrum_dirty[channel];
        g_render_txn.previous_rf_boxes_dirty =
            g_ui.rf_boxes_dirty[channel];
        g_render_txn.previous_presented_columns =
            g_waterfall_presented_columns[channel];
    }
    g_render_txn.target_total_columns = target_total_columns;

    /* Point LVGL at the private source while the currently displayed source
     * remains untouched.  Only the final row below is allowed to hand the
     * resulting framebuffer to GLCDC. */
    if(!g_waterfall_overlay.requested || g_waterfall_overlay.failed) {
        waterfall_image_head_set(source, render_write_column);
        if(!waterfall_image_source_rebind(source)) {
            render_transaction_abort();
            return false;
        }
    }
    else {
        g_render_txn.next_row = RF_WATERFALL_DISPLAY_HEIGHT;
    }
    g_rf_ui_channel_switch_diag.last_waterfall_render_column =
        render_write_column;
    if(kind == RF_UI_RENDER_CHANNEL_SWITCH &&
       !spectrum_image_source_rebind(source)) {
        render_transaction_abort();
        return false;
    }
    g_display_diag.lvgl_deferred_begins++;
    return true;
}

static void live_render_commit(void)
{
    const uint32_t channel = g_render_txn.channel;
    const uint8_t source = g_render_txn.source;
    const uint16_t render_write_column = g_render_txn.render_write_column;
    const uint64_t target_total = g_render_txn.target_total_columns;

    g_waterfall_active_source = source;
    g_waterfall_render_write_column = render_write_column;
    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        g_waterfall_overlay.visual_dirty = true;
        if(g_waterfall_overlay.display_source != source) {
            g_rf_ui_channel_switch_diag.overlay_source_switches++;
        }
    }
    else {
        waterfall_image_head_update();
    }
    if(channel < RF_UI_CHANNEL_COUNT) {
        g_waterfall_presented_columns[channel] = target_total;
        g_ui.waterfall_dirty[channel] =
            g_waterfall_total_columns[channel] != target_total;
        if(g_ui.rf_boxes_dirty[channel]) {
            refresh_rf_box_overlays();
            g_ui.rf_boxes_dirty[channel] = false;
        }
    }
    waterfall_source_state_commit(
        source, channel, target_total,
        g_live_build.catchup_source_head, render_write_column);
    g_rf_ui_channel_switch_diag.live_build_completions++;
    g_rf_ui_channel_switch_diag.live_atomic_commits++;
    if(g_waterfall_overlay.failed &&
       g_waterfall_overlay.fallback_rebuilding) {
        if(g_ui.waterfall_image != NULL) {
            lv_obj_set_style_opa(g_ui.waterfall_image, LV_OPA_COVER, 0);
        }
        g_waterfall_overlay.fallback_rebuilding = false;
        g_waterfall_overlay.fallback_disable_ready = true;
        g_display_diag.overlay_fallback_ready = 1U;
        refresh_rf_box_overlays();
    }
    memset(&g_live_build, 0, sizeof(g_live_build));
    g_live_build.state = RF_UI_LIVE_BUILD_IDLE;
}

static bool render_transaction_step(void)
{
    lv_display_t * const display = lv_display_get_default();
    if(!g_render_txn.active || display == NULL) return false;

    if(g_render_txn.next_row < RF_WATERFALL_DISPLAY_HEIGHT) {
        const uint32_t remaining = RF_WATERFALL_DISPLAY_HEIGHT -
                                   g_render_txn.next_row;
        const uint32_t rows = remaining <
                              RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK ?
                              remaining :
                              RF_CHANNEL_SWITCH_WATERFALL_RENDER_ROWS_PER_TICK;
        const bool last_waterfall = rows == remaining;
        const bool last_transaction = last_waterfall &&
            g_render_txn.kind != RF_UI_RENDER_CHANNEL_SWITCH;
        const uint32_t bytes = rows * RF_WATERFALL_VISIBLE_ROW_BYTES;

        if(last_transaction) {
            if(!lv_display_deferred_commit(display)) {
                render_transaction_abort();
                return false;
            }
            live_render_commit();
        }

        invalidate_image_area_rows(g_ui.waterfall_image,
                                   g_render_txn.next_row, rows);
        g_render_txn.next_row = (uint16_t)(g_render_txn.next_row + rows);

        if(g_render_txn.kind == RF_UI_RENDER_LIVE_WATERFALL) {
            g_rf_ui_channel_switch_diag.live_render_chunks++;
            g_rf_ui_channel_switch_diag.live_last_chunk_bytes = bytes;
            if(bytes > g_rf_ui_channel_switch_diag.live_max_chunk_bytes) {
                g_rf_ui_channel_switch_diag.live_max_chunk_bytes = bytes;
            }
        }

        if(last_transaction) {
            g_render_txn.commit_queued = true;
            return true;
        }
        return false;
    }

    if(g_render_txn.kind == RF_UI_RENDER_CHANNEL_SWITCH &&
       g_render_txn.metadata_stage <
           RF_CHANNEL_SWITCH_METADATA_STAGE_COUNT) {
        if(!channel_switch_stage_metadata_step()) {
            render_transaction_abort();
        }
        return false;
    }

    if(g_render_txn.kind == RF_UI_RENDER_CHANNEL_SWITCH &&
       g_render_txn.spectrum_render_row < RF_SPECTRUM_DISPLAY_HEIGHT) {
        const uint32_t remaining = RF_SPECTRUM_DISPLAY_HEIGHT -
                                   g_render_txn.spectrum_render_row;
        const uint32_t rows = remaining <
                              RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK ?
                              remaining :
                              RF_CHANNEL_SWITCH_SPECTRUM_RENDER_ROWS_PER_TICK;
        const bool last = rows == remaining;

        if(last) {
            /* Metadata changes precede only the final small invalidation, so
             * both images, labels, boxes, and selector state commit together. */
            if(!lv_display_deferred_commit(display) ||
               !channel_switch_commit()) {
                render_transaction_abort();
                return false;
            }
        }

        invalidate_image_area_rows(g_ui.spectrum_image,
                                   g_render_txn.spectrum_render_row, rows);
        g_render_txn.spectrum_render_row = (uint16_t)(
            g_render_txn.spectrum_render_row + rows);
        if(!last) return false;

        g_render_txn.commit_queued = true;
        return true;
    }

    render_transaction_abort();
    return false;
}

static void live_build_cancel(bool count_cancellation)
{
    if(g_live_build.state != RF_UI_LIVE_BUILD_IDLE && count_cancellation) {
        g_rf_ui_channel_switch_diag.live_build_cancellations++;
    }
    memset(&g_live_build, 0, sizeof(g_live_build));
    g_live_build.state = RF_UI_LIVE_BUILD_IDLE;
    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_IDLE) {
        g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
    }
}

static bool live_build_start(uint32_t channel)
{
    const uint64_t total = channel < RF_UI_CHANNEL_COUNT ?
                           g_waterfall_total_columns[channel] : 0U;
    const uint8_t source = (uint8_t)(g_waterfall_active_source ^ 1U);
    const bool overlay_build =
        g_waterfall_overlay.requested && !g_waterfall_overlay.failed;
    const uint64_t maximum_delta = overlay_build ?
        RF_UI_WATERFALL_HISTORY_COLS : RF_UI_WATERFALL_COLS;
    const rf_ui_waterfall_source_state_t source_state =
        g_waterfall_source_state[source];
    if(channel >= RF_UI_CHANNEL_COUNT || !g_ui.running ||
       g_render_txn.active || g_channel_build.state !=
           RF_UI_CHANNEL_SWITCH_IDLE ||
       (!g_waterfall_overlay.fallback_rebuilding &&
        total <= g_waterfall_presented_columns[channel])) {
        return false;
    }

    waterfall_overlay_sync_cancel();
    memset(&g_live_build, 0, sizeof(g_live_build));
    g_live_build.channel = (uint8_t)channel;
    g_live_build.source = source;
    g_live_build.request_generation =
        g_rf_ui_channel_switch_diag.request_generation;
    if(source_state.valid && source_state.channel == channel &&
       total >= source_state.total_columns &&
       (total - source_state.total_columns) <= maximum_delta &&
       waterfall_history_head_matches(
           source_state.history_head,
           total - source_state.total_columns,
           g_waterfall_write_head[channel])) {
        g_live_build.base_total_columns = source_state.total_columns;
        g_live_build.caught_up_total_columns = source_state.total_columns;
        g_live_build.base_write_head = source_state.history_head;
        g_live_build.catchup_source_head = source_state.history_head;
        g_live_build.render_write_column =
            source_state.render_write_column;
        g_live_build.catchup_target_total_columns = total;
        g_live_build.catchup_target_head = g_waterfall_write_head[channel];
        g_live_build.state = total == source_state.total_columns ?
                             RF_UI_LIVE_BUILD_READY :
                             RF_UI_LIVE_BUILD_CATCHUP;
        if(g_live_build.state == RF_UI_LIVE_BUILD_CATCHUP) {
            g_rf_ui_channel_switch_diag.live_catchup_passes++;
        }
        g_rf_ui_channel_switch_diag.live_incremental_builds++;
    }
    else {
        if(overlay_build) {
            g_waterfall_overlay.boxes_dirty[source] = true;
        }
        g_live_build.state = RF_UI_LIVE_BUILD_BASE;
        g_live_build.base_total_columns = total;
        g_live_build.caught_up_total_columns = total;
        g_live_build.base_write_head = g_waterfall_write_head[channel];
        g_live_build.logical_start = (uint16_t)(
            (g_live_build.base_write_head +
             RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS) %
            RF_UI_WATERFALL_HISTORY_COLS);
        g_live_build.catchup_source_head = g_live_build.base_write_head;
        g_live_build.render_write_column = 0U;
        g_rf_ui_channel_switch_diag.live_base_rebuilds++;
    }
    waterfall_source_state_invalidate(source);
    g_rf_ui_channel_switch_diag.live_build_starts++;
    g_rf_ui_channel_switch_diag.build_channel = channel;
    return true;
}

static bool live_build_prepare_catchup(void)
{
    const uint32_t channel = g_live_build.channel;
    uint64_t current_total;
    uint16_t current_head;
    if(!waterfall_history_snapshot(channel, &current_total, &current_head)) {
        g_rf_ui_channel_switch_diag.live_catchup_head_mismatches++;
        live_build_cancel(true);
        return false;
    }
    const uint64_t delta = current_total >=
                           g_live_build.caught_up_total_columns ?
                           current_total -
                           g_live_build.caught_up_total_columns : UINT64_MAX;

    if(current_total < g_live_build.caught_up_total_columns ||
       delta > RF_UI_WATERFALL_COLS) {
        live_build_cancel(true);
        return false;
    }
    if(!waterfall_history_head_matches(g_live_build.catchup_source_head,
                                       delta, current_head)) {
        g_rf_ui_channel_switch_diag.live_catchup_head_mismatches++;
        live_build_cancel(true);
        return false;
    }
    if(delta == 0U) {
        g_live_build.state = RF_UI_LIVE_BUILD_READY;
        return true;
    }

    g_live_build.catchup_target_total_columns = current_total;
    g_live_build.catchup_target_head = current_head;
    g_live_build.waterfall_render_y = 0U;
    g_live_build.waterfall_source_row = 0U;
    g_live_build.state = RF_UI_LIVE_BUILD_CATCHUP;
    g_rf_ui_channel_switch_diag.live_catchup_passes++;
    return true;
}

static bool live_build_step(void)
{
    rf_ui_waterfall_rgb565_ring_t * target;
    uint32_t rows_written = 0U;
    uint32_t bytes_written = 0U;
    const bool overlay_build =
        g_waterfall_overlay.requested && !g_waterfall_overlay.failed;

    if(g_live_build.state == RF_UI_LIVE_BUILD_IDLE) return false;
    if(g_live_build.channel != g_ui.committed_channel ||
       g_live_build.request_generation !=
           g_rf_ui_channel_switch_diag.request_generation) {
        live_build_cancel(true);
        return false;
    }

    if(g_live_build.state == RF_UI_LIVE_BUILD_READY) {
        if(overlay_build) {
            const uint8_t source = g_live_build.source;
            const uint32_t channel = g_live_build.channel;
            waterfall_source_state_commit(
                source, channel, g_live_build.caught_up_total_columns,
                g_live_build.catchup_source_head,
                g_live_build.render_write_column);
            g_waterfall_overlay.boxes_dirty[source] = true;
            waterfall_overlay_boxes_refresh(source);
            if(g_waterfall_overlay.display_source != source) {
                g_rf_ui_channel_switch_diag.overlay_source_switches++;
            }
            g_waterfall_active_source = source;
            g_waterfall_render_write_column =
                g_live_build.render_write_column;
            g_waterfall_overlay.visual_dirty = true;
            g_rf_ui_channel_switch_diag.live_build_completions++;
            g_rf_ui_channel_switch_diag.live_atomic_commits++;
            memset(&g_live_build, 0, sizeof(g_live_build));
            g_live_build.state = RF_UI_LIVE_BUILD_IDLE;
            return true;
        }
        if(!g_render_txn.active) {
            if(!render_transaction_begin(
                   RF_UI_RENDER_LIVE_WATERFALL,
                   g_live_build.channel,
                   g_live_build.source,
                   g_live_build.render_write_column,
                   g_live_build.caught_up_total_columns)) {
                return false;
            }
        }
        return render_transaction_step();
    }

    target = &g_waterfall_render_rings[g_live_build.source].rgb565;
    if(g_live_build.state == RF_UI_LIVE_BUILD_BASE) {
        if(g_waterfall_total_columns[g_live_build.channel] <
               g_live_build.base_total_columns ||
           (g_waterfall_total_columns[g_live_build.channel] -
            g_live_build.base_total_columns) >
               (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS)) {
            live_build_cancel(true);
            return false;
        }
        const uint32_t rows_per_tick = overlay_build ?
            RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK :
            RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK;
        while(g_live_build.waterfall_render_y <
                  RF_WATERFALL_DISPLAY_HEIGHT &&
              rows_written < rows_per_tick) {
            while((g_live_build.waterfall_source_row + 1U) <
                      RF_UI_WATERFALL_FREQ_BINS &&
                  g_live_build.waterfall_render_y >=
                      g_waterfall_render_y[
                          g_live_build.waterfall_source_row + 1U]) {
                g_live_build.waterfall_source_row++;
            }
            if(overlay_build) {
                waterfall_overlay_build_row(
                    g_live_build.source,
                    g_live_build.channel,
                    g_live_build.waterfall_render_y,
                    g_live_build.waterfall_source_row);
            }
            else {
                waterfall_render_live_row(
                    target, g_live_build.channel, g_live_build.logical_start,
                    g_live_build.waterfall_render_y,
                    g_live_build.waterfall_source_row);
            }
            g_live_build.waterfall_render_y++;
            rows_written++;
        }
        bytes_written = rows_written * (overlay_build ?
            RF_WATERFALL_CLUT_BUILD_ROW_WRITE_BYTES :
            RF_WATERFALL_RENDER_STRIDE_BYTES);
        g_rf_ui_channel_switch_diag.live_build_chunks++;
        g_rf_ui_channel_switch_diag.live_build_rows += rows_written;
        g_rf_ui_channel_switch_diag.live_last_chunk_bytes = bytes_written;
        if(bytes_written > g_rf_ui_channel_switch_diag.live_max_chunk_bytes) {
            g_rf_ui_channel_switch_diag.live_max_chunk_bytes = bytes_written;
        }
        if(overlay_build) {
            g_rf_ui_channel_switch_diag.overlay_build_chunks++;
            g_rf_ui_channel_switch_diag.overlay_build_rows += rows_written;
        }
        if(g_live_build.waterfall_render_y == RF_WATERFALL_DISPLAY_HEIGHT) {
            g_live_build.caught_up_total_columns =
                g_live_build.base_total_columns;
            g_live_build.catchup_source_head = g_live_build.base_write_head;
            g_live_build.render_write_column = 0U;
            (void)live_build_prepare_catchup();
        }
        return false;
    }

    if(g_live_build.state == RF_UI_LIVE_BUILD_CATCHUP) {
        const uint64_t delta = g_live_build.catchup_target_total_columns -
                               g_live_build.caught_up_total_columns;
        uint64_t latest_total;
        uint16_t latest_head;
        if(!waterfall_history_snapshot(g_live_build.channel,
                                       &latest_total, &latest_head)) {
            g_rf_ui_channel_switch_diag.live_catchup_head_mismatches++;
            live_build_cancel(true);
            return false;
        }
        if(latest_total < g_live_build.caught_up_total_columns ||
           latest_total < g_live_build.catchup_target_total_columns ||
           (latest_total - g_live_build.caught_up_total_columns) >
               RF_UI_WATERFALL_HISTORY_COLS) {
            g_rf_ui_channel_switch_diag
                .live_catchup_overwrite_cancellations++;
            live_build_cancel(true);
            return false;
        }
        if(!waterfall_history_head_matches(
               g_live_build.catchup_source_head,
               latest_total - g_live_build.caught_up_total_columns,
               latest_head)) {
            g_rf_ui_channel_switch_diag.live_catchup_head_mismatches++;
            live_build_cancel(true);
            return false;
        }
        if(delta > RF_UI_WATERFALL_COLS) {
            live_build_cancel(true);
            return false;
        }
        if(delta == 0U) {
            g_live_build.state = RF_UI_LIVE_BUILD_READY;
            return false;
        }
        const uint32_t remaining_rows = RF_WATERFALL_DISPLAY_HEIGHT -
            g_live_build.waterfall_render_y;
        const uint32_t rows_this_step = overlay_build ?
            waterfall_overlay_catchup_rows_per_step(
                (uint32_t)delta, remaining_rows) :
            waterfall_catchup_rows_per_step(
                g_live_build.render_write_column, (uint32_t)delta,
                remaining_rows);
        while(g_live_build.waterfall_render_y <
                  RF_WATERFALL_DISPLAY_HEIGHT &&
              rows_written < rows_this_step) {
            while((g_live_build.waterfall_source_row + 1U) <
                      RF_UI_WATERFALL_FREQ_BINS &&
                  g_live_build.waterfall_render_y >=
                      g_waterfall_render_y[
                          g_live_build.waterfall_source_row + 1U]) {
                g_live_build.waterfall_source_row++;
            }
            if(overlay_build) {
                bytes_written += waterfall_overlay_catchup_row(
                    g_live_build.source,
                    g_live_build.channel,
                    g_live_build.waterfall_render_y,
                    g_live_build.waterfall_source_row,
                    g_live_build.catchup_source_head,
                    (uint32_t)delta);
            }
            else {
                bytes_written += waterfall_render_catchup_row_at(
                    target, g_live_build.channel,
                    g_live_build.waterfall_render_y,
                    g_live_build.waterfall_source_row,
                    g_live_build.catchup_source_head,
                    g_live_build.render_write_column,
                    (uint32_t)delta);
            }
            g_live_build.waterfall_render_y++;
            rows_written++;
        }
        g_rf_ui_channel_switch_diag.live_build_chunks++;
        g_rf_ui_channel_switch_diag.live_build_rows += rows_written;
        g_rf_ui_channel_switch_diag.live_last_chunk_bytes = bytes_written;
        if(bytes_written > g_rf_ui_channel_switch_diag.live_max_chunk_bytes) {
            g_rf_ui_channel_switch_diag.live_max_chunk_bytes = bytes_written;
        }
        if(overlay_build) {
            g_rf_ui_channel_switch_diag.overlay_build_chunks++;
            g_rf_ui_channel_switch_diag.overlay_build_rows += rows_written;
        }
        if(g_live_build.waterfall_render_y == RF_WATERFALL_DISPLAY_HEIGHT) {
            g_live_build.render_write_column = (uint16_t)(
                (g_live_build.render_write_column + delta) %
                RF_UI_WATERFALL_COLS);
            g_live_build.catchup_source_head =
                g_live_build.catchup_target_head;
            g_live_build.caught_up_total_columns =
                g_live_build.catchup_target_total_columns;
            const uint64_t backlog = latest_total -
                g_live_build.caught_up_total_columns;
            const uint32_t backlog32 = backlog > UINT32_MAX ?
                                       UINT32_MAX : (uint32_t)backlog;
            g_rf_ui_channel_switch_diag.live_catchup_completions++;
            g_rf_ui_channel_switch_diag.live_catchup_backlog_at_ready =
                backlog32;
            if(backlog32 > g_rf_ui_channel_switch_diag
                           .live_catchup_max_backlog_at_ready) {
                g_rf_ui_channel_switch_diag
                    .live_catchup_max_backlog_at_ready = backlog32;
            }
            /* This source is a coherent frozen snapshot. Newer columns remain
             * dirty and are handled by the next normal live build. */
            g_live_build.state = RF_UI_LIVE_BUILD_READY;
        }
    }
    return false;
}

static bool channel_switch_prepare_render(void)
{
    if(g_channel_build.channel >= RF_UI_CHANNEL_COUNT ||
       g_channel_build.channel != g_ui.pending_channel ||
       g_render_txn.active) {
        return false;
    }
    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        waterfall_source_state_commit(
            g_channel_build.source, g_channel_build.channel,
            g_channel_build.caught_up_total_columns,
            g_channel_build.catchup_source_head,
            g_channel_build.render_write_column);
        g_waterfall_overlay.boxes_dirty[g_channel_build.source] = true;
        waterfall_overlay_boxes_refresh(g_channel_build.source);
    }
    if(!render_transaction_begin(
           RF_UI_RENDER_CHANNEL_SWITCH,
           g_channel_build.channel,
           g_channel_build.source,
           g_channel_build.render_write_column,
           g_channel_build.caught_up_total_columns)) {
        return false;
    }
    channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WATERFALL_RENDER);
    return true;
}

static bool channel_switch_prepare_catchup(void)
{
    const uint32_t channel = g_channel_build.channel;
    uint64_t current_total;
    uint16_t current_head;
    uint64_t delta;

    if(!waterfall_history_snapshot(channel, &current_total, &current_head)) {
        g_rf_ui_channel_switch_diag.switch_catchup_head_mismatches++;
        channel_switch_restart();
        return false;
    }
    if(current_total < g_channel_build.caught_up_total_columns) {
        channel_switch_restart();
        return false;
    }
    delta = current_total - g_channel_build.caught_up_total_columns;
    if(delta > RF_UI_WATERFALL_HISTORY_COLS) {
        channel_switch_restart();
        return false;
    }
    if(!waterfall_history_head_matches(g_channel_build.catchup_source_head,
                                       delta, current_head)) {
        g_rf_ui_channel_switch_diag.switch_catchup_head_mismatches++;
        channel_switch_restart();
        return false;
    }
    if(g_channel_build.waterfall_cache_reused) {
        const uint32_t catchup_columns = (uint32_t)delta;
        g_rf_ui_channel_switch_diag.switch_cache_catchup_columns +=
            catchup_columns;
        if(catchup_columns >
           g_rf_ui_channel_switch_diag.switch_cache_max_catchup_columns) {
            g_rf_ui_channel_switch_diag.switch_cache_max_catchup_columns =
                catchup_columns;
        }
    }
    if(delta == 0U) {
        (void)channel_switch_prepare_render();
        return false;
    }

    g_channel_build.catchup_target_total_columns = current_total;
    g_channel_build.catchup_target_head = current_head;
    g_channel_build.waterfall_render_y = 0U;
    g_channel_build.waterfall_source_row = 0U;
    channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WATERFALL_CATCHUP);
    g_rf_ui_channel_switch_diag.switch_catchup_passes++;
    return false;
}

bool rf_ui_channel_switch_step(void)
{
    rf_ui_waterfall_rgb565_ring_t * target;
    uint32_t rows_written = 0U;
    uint32_t bytes_written = 0U;

    render_transaction_poll_complete();
    channel_switch_soak_step();
    if(!g_ui.running || g_ui.pending_channel == g_ui.committed_channel) {
        return false;
    }
    if(g_channel_build.request_generation !=
       g_rf_ui_channel_switch_diag.request_generation ||
       g_channel_build.channel != g_ui.pending_channel) {
        return false;
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_WAIT_WINDOW) {
        (void)channel_switch_build_start(false);
        return false;
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_WATERFALL_RENDER) {
        return render_transaction_step();
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_SPECTRUM_BASE) {
        const uint32_t remaining =
            RF_SPECTRUM_TEXTURE_HEIGHT - g_channel_build.spectrum_row;
        rows_written = remaining < RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK ?
                       remaining : RF_CHANNEL_SWITCH_SPECTRUM_ROWS_PER_TICK;
        bytes_written = spectrum_fill_base_rows(
            g_spectrum_pixels[g_channel_build.source],
            g_channel_build.spectrum_row, rows_written);
        g_channel_build.spectrum_row =
            (uint16_t)(g_channel_build.spectrum_row + rows_written);
        channel_switch_record_chunk(bytes_written, rows_written);
        if(g_channel_build.spectrum_row == RF_SPECTRUM_TEXTURE_HEIGHT) {
            g_channel_build.spectrum_segment = 1U;
            channel_switch_set_state(RF_UI_CHANNEL_SWITCH_SPECTRUM_TRACE);
        }
        return false;
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_SPECTRUM_TRACE) {
        uint32_t trace_bytes = 0U;
        g_channel_build.spectrum_segment = spectrum_draw_trace_segments(
            g_spectrum_pixels[g_channel_build.source],
            g_channel_build.spectrum_x,
            g_channel_build.spectrum_y,
            g_channel_build.spectrum_segment,
            RF_CHANNEL_SWITCH_SPECTRUM_SEGMENTS_PER_TICK,
            &trace_bytes);
        channel_switch_record_chunk(trace_bytes, 0U);
        if(g_channel_build.spectrum_segment >= RF_UI_SPECTRUM_BINS) {
            channel_switch_set_state(RF_UI_CHANNEL_SWITCH_SPECTRUM_PEAK);
        }
        return false;
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_SPECTRUM_PEAK) {
        bytes_written = spectrum_draw_peak(
            g_spectrum_pixels[g_channel_build.source],
            g_channel_build.spectrum_x,
            g_channel_build.spectrum_y,
            g_channel_build.spectrum_peak_index);
        channel_switch_record_chunk(bytes_written, 0U);
        if(g_channel_build.waterfall_cache_reused) {
            return channel_switch_prepare_catchup();
        }
        g_channel_build.waterfall_render_y = 0U;
        g_channel_build.waterfall_source_row = 0U;
        channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WATERFALL_BASE);
        return false;
    }

    target = &g_waterfall_render_rings[g_channel_build.source].rgb565;
    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_WATERFALL_BASE) {
        const bool overlay_build =
            g_waterfall_overlay.requested && !g_waterfall_overlay.failed;
        const uint32_t rows_per_tick = overlay_build ?
            RF_WATERFALL_OVERLAY_BUILD_ROWS_PER_TICK :
            RF_CHANNEL_SWITCH_WATERFALL_SOURCE_ROWS_PER_TICK;
        uint64_t latest_total;
        uint16_t latest_head;
        if(!waterfall_history_snapshot(g_channel_build.channel,
                                       &latest_total,
                                       &latest_head) ||
           latest_total < g_channel_build.base_total_columns ||
           (latest_total - g_channel_build.base_total_columns) >
               (RF_UI_WATERFALL_HISTORY_COLS - RF_UI_WATERFALL_COLS) ||
           !waterfall_history_head_matches(
               g_channel_build.base_write_head,
               latest_total - g_channel_build.base_total_columns,
               latest_head)) {
            channel_switch_restart();
            return false;
        }
        while(g_channel_build.waterfall_render_y <
                  RF_WATERFALL_DISPLAY_HEIGHT &&
              rows_written < rows_per_tick) {
            while((g_channel_build.waterfall_source_row + 1U) <
                      RF_UI_WATERFALL_FREQ_BINS &&
                  g_channel_build.waterfall_render_y >=
                      g_waterfall_render_y[
                          g_channel_build.waterfall_source_row + 1U]) {
                g_channel_build.waterfall_source_row++;
            }
            if(overlay_build) {
                waterfall_overlay_build_row(
                    g_channel_build.source,
                    g_channel_build.channel,
                    g_channel_build.waterfall_render_y,
                    g_channel_build.waterfall_source_row);
            }
            else {
                waterfall_render_live_row(
                    target, g_channel_build.channel,
                    g_channel_build.logical_start,
                    g_channel_build.waterfall_render_y,
                    g_channel_build.waterfall_source_row);
            }
            g_channel_build.waterfall_render_y++;
            rows_written++;
        }
        bytes_written = rows_written * (overlay_build ?
            RF_WATERFALL_CLUT_BUILD_ROW_WRITE_BYTES :
            RF_WATERFALL_RENDER_STRIDE_BYTES);
        channel_switch_record_chunk(bytes_written, rows_written);
        if(overlay_build) {
            g_rf_ui_channel_switch_diag.overlay_build_chunks++;
            g_rf_ui_channel_switch_diag.overlay_build_rows += rows_written;
        }
        if(g_channel_build.waterfall_render_y ==
           RF_WATERFALL_DISPLAY_HEIGHT) {
            g_channel_build.caught_up_total_columns =
                g_channel_build.base_total_columns;
            g_channel_build.catchup_source_head =
                g_channel_build.base_write_head;
            g_channel_build.render_write_column = 0U;
            return channel_switch_prepare_catchup();
        }
        return false;
    }

    if(g_channel_build.state == RF_UI_CHANNEL_SWITCH_WATERFALL_CATCHUP) {
        const bool overlay_build =
            g_waterfall_overlay.requested && !g_waterfall_overlay.failed;
        const uint32_t catchup_columns = (uint32_t)(
            g_channel_build.catchup_target_total_columns -
            g_channel_build.caught_up_total_columns);
        uint64_t latest_total;
        uint16_t latest_head;
        if(!waterfall_history_snapshot(g_channel_build.channel,
                                       &latest_total, &latest_head)) {
            g_rf_ui_channel_switch_diag.switch_catchup_head_mismatches++;
            channel_switch_restart();
            return false;
        }
        if(latest_total < g_channel_build.caught_up_total_columns ||
           latest_total < g_channel_build.catchup_target_total_columns ||
           (latest_total - g_channel_build.caught_up_total_columns) >
               RF_UI_WATERFALL_HISTORY_COLS) {
            g_rf_ui_channel_switch_diag.switch_catchup_overwrite_restarts++;
            channel_switch_restart();
            return false;
        }
        if(!waterfall_history_head_matches(
               g_channel_build.catchup_source_head,
               latest_total - g_channel_build.caught_up_total_columns,
               latest_head)) {
            g_rf_ui_channel_switch_diag.switch_catchup_head_mismatches++;
            channel_switch_restart();
            return false;
        }
        const uint32_t remaining_rows = RF_WATERFALL_DISPLAY_HEIGHT -
            g_channel_build.waterfall_render_y;
        const uint32_t rows_this_step = overlay_build ?
            waterfall_overlay_catchup_rows_per_step(
                catchup_columns, remaining_rows) :
            waterfall_catchup_rows_per_step(
                g_channel_build.render_write_column, catchup_columns,
                remaining_rows);
        while(g_channel_build.waterfall_render_y <
                  RF_WATERFALL_DISPLAY_HEIGHT &&
              rows_written < rows_this_step) {
            while((g_channel_build.waterfall_source_row + 1U) <
                      RF_UI_WATERFALL_FREQ_BINS &&
                  g_channel_build.waterfall_render_y >=
                      g_waterfall_render_y[
                          g_channel_build.waterfall_source_row + 1U]) {
                g_channel_build.waterfall_source_row++;
            }
            if(overlay_build) {
                bytes_written += waterfall_overlay_catchup_row(
                    g_channel_build.source,
                    g_channel_build.channel,
                    g_channel_build.waterfall_render_y,
                    g_channel_build.waterfall_source_row,
                    g_channel_build.catchup_source_head,
                    catchup_columns);
            }
            else {
                bytes_written += waterfall_render_catchup_row(
                    target, g_channel_build.channel,
                    g_channel_build.waterfall_render_y,
                    g_channel_build.waterfall_source_row,
                    catchup_columns);
            }
            g_channel_build.waterfall_render_y++;
            rows_written++;
        }
        channel_switch_record_chunk(bytes_written, rows_written);
        if(overlay_build) {
            g_rf_ui_channel_switch_diag.overlay_build_chunks++;
            g_rf_ui_channel_switch_diag.overlay_build_rows += rows_written;
        }
        if(g_channel_build.waterfall_render_y ==
           RF_WATERFALL_DISPLAY_HEIGHT) {
            g_channel_build.render_write_column = (uint16_t)(
                (g_channel_build.render_write_column + catchup_columns) %
                RF_UI_WATERFALL_COLS);
            g_channel_build.catchup_source_head =
                g_channel_build.catchup_target_head;
            g_channel_build.caught_up_total_columns =
                g_channel_build.catchup_target_total_columns;
            const uint64_t backlog = latest_total -
                g_channel_build.caught_up_total_columns;
            const uint32_t backlog32 = backlog > UINT32_MAX ?
                                       UINT32_MAX : (uint32_t)backlog;
            g_rf_ui_channel_switch_diag.switch_catchup_completions++;
            g_rf_ui_channel_switch_diag.switch_catchup_backlog_at_render =
                backlog32;
            if(backlog32 > g_rf_ui_channel_switch_diag
                           .switch_catchup_max_backlog_at_render) {
                g_rf_ui_channel_switch_diag
                    .switch_catchup_max_backlog_at_render = backlog32;
            }
            /* Commit this coherent cutoff. Columns that arrived while it was
             * built stay dirty and are caught up after the atomic switch. */
            (void)channel_switch_prepare_render();
            return false;
        }
    }
    return false;
}

void rf_ui_create(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    memset(&g_channel_build, 0, sizeof(g_channel_build));
    memset(&g_live_build, 0, sizeof(g_live_build));
    memset(&g_render_txn, 0, sizeof(g_render_txn));
    memset(&g_waterfall_overlay, 0, sizeof(g_waterfall_overlay));
    memset(&g_waterfall_overlay_sync, 0,
           sizeof(g_waterfall_overlay_sync));
    g_waterfall_overlay.requested = true;
    g_waterfall_overlay.next_generation = 1U;
    g_waterfall_overlay.visual_dirty = true;
    memset(g_waterfall_presented_columns, 0,
           sizeof(g_waterfall_presented_columns));
    memset(g_waterfall_source_state, 0,
           sizeof(g_waterfall_source_state));
    g_live_build.state = RF_UI_LIVE_BUILD_IDLE;
    g_render_txn.kind = RF_UI_RENDER_NONE;
    memset(g_spectrum_identity, 0, sizeof(g_spectrum_identity));
    memset(g_complete_windows, 0, sizeof(g_complete_windows));
    memset(g_complete_spectrum_data, 0, sizeof(g_complete_spectrum_data));
    memset((void *)&g_rf_ui_channel_switch_diag, 0,
           sizeof(g_rf_ui_channel_switch_diag));
    g_rf_ui_channel_switch_diag.magic = RF_UI_CHANNEL_SWITCH_DIAG_MAGIC;
    g_rf_ui_channel_switch_diag.version =
        RF_UI_CHANNEL_SWITCH_DIAG_VERSION;
    g_rf_ui_channel_switch_diag.state = RF_UI_CHANNEL_SWITCH_IDLE;
    g_rf_ui_channel_switch_diag.pending_channel = 0U;
    g_rf_ui_channel_switch_diag.committed_channel = 0U;
    g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
    g_rf_ui_channel_switch_diag.active_source = 0U;
    memset((void *)&g_rf_ui_channel_soak, 0,
           sizeof(g_rf_ui_channel_soak));
    g_rf_ui_channel_soak.magic = RF_UI_CHANNEL_SOAK_MAGIC;
    g_rf_ui_channel_soak.version = RF_UI_CHANNEL_SOAK_VERSION;
    g_rf_ui_channel_soak.next_channel = RF_UI_CHANNEL_NONE;
    g_rf_ui_channel_soak.last_requested_channel = RF_UI_CHANNEL_NONE;
    memset((void *)&g_rf_ui_input_diag, 0,
           sizeof(g_rf_ui_input_diag));
    g_rf_ui_input_diag.magic = RF_UI_INPUT_DIAG_MAGIC;
    g_rf_ui_input_diag.version = RF_UI_INPUT_DIAG_VERSION;
    memset(&g_rf_box_pause_snapshot, 0,
           sizeof(g_rf_box_pause_snapshot));
    memset(g_rf_box_batches, 0, sizeof(g_rf_box_batches));
    memset(g_spectrum_rf_box_batches, 0,
           sizeof(g_spectrum_rf_box_batches));
    memset(&g_spectrum_rf_box_pause_snapshot, 0,
           sizeof(g_spectrum_rf_box_pause_snapshot));
    memset(g_pending_box_batches, 0, sizeof(g_pending_box_batches));
    memset(g_fusion_decision_cache, 0, sizeof(g_fusion_decision_cache));
    memset(g_window_anchors, 0, sizeof(g_window_anchors));
    g_window_anchor_write_index = 0U;
    memset(g_latest_box_window_valid, 0, sizeof(g_latest_box_window_valid));
    memset(g_latest_box_session_id, 0, sizeof(g_latest_box_session_id));
    memset(g_latest_box_window_sequence, 0,
           sizeof(g_latest_box_window_sequence));
    memset(g_last_detail_round_index, 0,
           sizeof(g_last_detail_round_index));
    memset(g_last_detail_round_valid_mask, 0,
           sizeof(g_last_detail_round_valid_mask));
    g_fusion_decision_generation = 0U;
    g_rf_box_observation_generation = 0U;
    g_waterfall_pause_total_columns = 0U;
    /* This integration is live-IQ only.  The reference package seeded these
     * arrays with a pleasing demo, but that made the boot screen misleading
     * until the first SDR window reached CPU1. */
    memset(g_spectrum_data, 0, sizeof(g_spectrum_data));
    for(uint32_t channel = 0; channel < RF_DEMO_CHANNEL_COUNT; ++channel) {
        g_channel_metrics[channel].peak_dbfs = -120;
        g_channel_metrics[channel].noise_floor_dbfs = -120;
        g_channel_metrics[channel].occupancy_percent = 0U;
        g_channel_metrics[channel].age_ms = 0U;
    }
    for(uint32_t index = 0; index < RF_DEMO_CLASS_COUNT; ++index) {
        g_detections[index].state = RF_UI_DETECTION_INACTIVE;
        g_detections[index].confidence_percent = 0U;
        g_detections[index].channel_index = 0U;
    }

    g_scan_rate_x10 = 0U;
    g_ui.page = RF_UI_PAGE_MONITOR;
    g_ui.running = true;
    g_ui.external_spectrum_mode = true;
    g_ui.model_placeholder = true;
    g_ui.focus_mode = false;
    g_ui.pending_channel = 0U;
    g_ui.committed_channel = 0U;
    g_ui.selected_detection_index = -1;
    g_ui.pending_detection_index = -1;
    g_channel_build.channel = UINT8_MAX;

    prepare_spectrum_image();
    prepare_waterfall_lookup_tables();
    prepare_waterfall_images();
    g_waterfall_presented_columns[g_ui.committed_channel] =
        g_waterfall_total_columns[g_ui.committed_channel];

    g_ui.screen = lv_screen_active();
    reset_object(g_ui.screen);
    lv_obj_set_size(g_ui.screen, RF_SCREEN_WIDTH, RF_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(g_ui.screen, color(RF_COLOR_SCREEN), 0);
    lv_obj_set_style_bg_opa(g_ui.screen, LV_OPA_COVER, 0);

    create_header();
    create_target_strip();
    create_sidebar();
    create_waterfall_panel();
    create_spectrum_panel();
    create_selectors();
    create_metrics_footer();
    refresh_waterfall_timing();
    refresh_scan_rate();
    refresh_live_state();
    refresh_source_badge();
    refresh_acquisition_mode();
    refresh_selected_view();
    refresh_alert(true);
    refresh_rf_box_overlays();
}

void rf_ui_set_page(rf_ui_page_t page)
{
    if(page <= RF_UI_PAGE_RECOGNITION) g_ui.page = page;
}

rf_ui_page_t rf_ui_get_page(void)
{
    return g_ui.page;
}

void rf_ui_set_running(int running)
{
    const bool next = running != 0;
    if(g_ui.running == next) return;

    /* A pause/resume or review gesture owns the LVGL thread as well.  Drop
     * any private live/switch transaction before rebuilding the paused view so
     * no stale source can be committed after the mode change. */
    render_transaction_abort();
    live_build_cancel(true);
    waterfall_overlay_sync_cancel();

    if(!next) {
        if(g_ui.pending_channel != g_ui.committed_channel) {
            g_rf_ui_channel_switch_diag.cancellations++;
            g_ui.pending_channel = g_ui.committed_channel;
            g_ui.pending_detection_index = -1;
            g_channel_build.channel = UINT8_MAX;
            g_rf_ui_channel_switch_diag.pending_channel =
                g_ui.committed_channel;
            g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
            channel_switch_set_state(RF_UI_CHANNEL_SWITCH_IDLE);
        }
        /* Commit a pending spectrum before freezing both plots. Ingestion
         * continues into the live rings while this selected-channel snapshot
         * remains stable for touch review. */
        (void)rf_ui_present_spectrum();
        g_ui.running = false;
        g_spectrum_rf_box_pause_snapshot =
            g_spectrum_rf_box_batches[g_ui.committed_channel];
        g_ui.waterfall_pan_columns = 0U;
        g_ui.waterfall_rendered_pan_columns = 0U;
        g_ui.waterfall_drag_accumulator = 0;
        if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
            g_rf_box_pause_snapshot =
                g_rf_box_batches[g_ui.committed_channel];
            g_waterfall_pause_total_columns =
                g_waterfall_overlay.presented_end_pixels /
                RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
            g_waterfall_overlay.boxes_dirty[g_waterfall_active_source] = true;
            g_waterfall_overlay.visual_dirty = true;
        }
        else {
            waterfall_pause_snapshot_capture(g_ui.committed_channel);
            waterfall_render_rebuild_paused();
            if(g_ui.waterfall_image != NULL) {
                lv_image_set_src(
                    g_ui.waterfall_image,
                    &g_waterfall_image_dsc[g_waterfall_active_source]);
            }
        }
        g_ui.waterfall_pan_present_tick = lv_tick_get();
        g_ui.waterfall_dirty[g_ui.committed_channel] = false;
    }
    else {
        g_ui.running = true;
        g_ui.waterfall_pan_columns = 0U;
        g_ui.waterfall_rendered_pan_columns = 0U;
        g_ui.waterfall_drag_accumulator = 0;
        if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
            g_ui.waterfall_dirty[g_ui.committed_channel] = true;
            g_waterfall_overlay.visual_dirty = true;
        }
        else {
            waterfall_render_bootstrap_live(g_ui.committed_channel);
            g_waterfall_presented_columns[g_ui.committed_channel] =
                g_waterfall_total_columns[g_ui.committed_channel];
            if(g_ui.waterfall_image != NULL) {
                lv_image_set_src(
                    g_ui.waterfall_image,
                    &g_waterfall_image_dsc[g_waterfall_active_source]);
            }
            g_ui.waterfall_dirty[g_ui.committed_channel] = false;
        }
        g_ui.waterfall_pan_present_tick = lv_tick_get();
        g_ui.spectrum_dirty[g_ui.committed_channel] = true;
        (void)rf_ui_present_spectrum();
    }
    refresh_live_state();
    refresh_rf_box_overlays();
}

int rf_ui_is_running(void)
{
    return g_ui.running ? 1 : 0;
}

void rf_ui_toggle_running(void)
{
    rf_ui_set_running(!g_ui.running);
}

void rf_ui_set_external_spectrum_mode(bool enabled)
{
    if(g_ui.external_spectrum_mode == enabled) return;
    g_ui.external_spectrum_mode = enabled;
    refresh_source_badge();
    if(g_ui.screen != NULL) {
        g_ui.spectrum_dirty[g_ui.committed_channel] = true;
        (void) rf_ui_present_spectrum();
    }
}

void rf_ui_set_model_placeholder(bool placeholder)
{
    if(g_ui.model_placeholder == placeholder) return;
    g_ui.model_placeholder = placeholder;
    if(channel_switch_defer_metadata_refresh()) return;
    refresh_alert(true);
    refresh_rf_box_overlays();
}

void rf_ui_set_detection_ready(bool ready)
{
    if ((g_ui.detection_ready == ready) ||
        (g_ui.detection_status_label == NULL))
    {
        return;
    }
    g_ui.detection_ready = ready;
    lv_label_set_text(g_ui.detection_status_label,
                      ready ? "检测功能正常" : "背景标定中");
    lv_obj_set_style_text_color(
        g_ui.detection_status_label,
        color(ready ? RF_COLOR_GREEN : RF_COLOR_AMBER), 0);
}

bool rf_ui_set_selected_channel(uint32_t channel_index)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT) return false;
    if(!g_ui.running) return false;
    render_transaction_poll_complete();
    if(g_ui.pending_channel == channel_index) return true;

    render_transaction_abort();
    live_build_cancel(true);
    waterfall_overlay_sync_cancel();
    const uint32_t previous_pending_channel = g_ui.pending_channel;

    g_rf_ui_channel_switch_diag.requests++;
    g_rf_ui_channel_switch_diag.request_generation =
        channel_switch_next_revision(
            g_rf_ui_channel_switch_diag.request_generation);
    g_rf_ui_channel_switch_diag.switch_request_line_event =
        g_display_diag.glcdc_line_events;
    if(g_ui.pending_channel != g_ui.committed_channel) {
        g_rf_ui_channel_switch_diag.cancellations++;
    }

    g_ui.pending_channel = (uint8_t)channel_index;
    g_ui.pending_detection_index = -1;
    g_rf_ui_channel_switch_diag.pending_channel = channel_index;
    if(previous_pending_channel != g_ui.committed_channel) {
        refresh_selector_style(previous_pending_channel);
    }
    if(channel_index != g_ui.committed_channel) {
        refresh_selector_style(channel_index);
    }
    if(channel_index == g_ui.committed_channel) {
        g_channel_build.channel = UINT8_MAX;
        g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
        channel_switch_set_state(RF_UI_CHANNEL_SWITCH_IDLE);
        return true;
    }

    memset(&g_channel_build, 0, sizeof(g_channel_build));
    g_channel_build.channel = (uint8_t)channel_index;
    g_channel_build.request_generation =
        g_rf_ui_channel_switch_diag.request_generation;
    if(complete_spectrum_snapshot_ready(channel_index)) {
        /* A complete spectrum/window pair is immutable until copied into the
         * private render source.  Show it immediately; newer scan data marks
         * the committed channel dirty and replaces it without a blank frame. */
        g_channel_build.required_spectrum_revision =
            g_complete_windows[channel_index].spectrum_revision;
        g_channel_build.required_window_revision =
            g_complete_windows[channel_index].revision;
    }
    else {
        /* If a spectrum has already arrived, allow its matching final
         * waterfall row to complete the pair.  Otherwise wait for the first
         * post-request spectrum instead of accepting an unidentified buffer. */
        g_channel_build.required_spectrum_revision =
            g_spectrum_identity[channel_index].valid ?
            g_spectrum_identity[channel_index].revision :
            channel_switch_next_revision(
                g_spectrum_identity[channel_index].revision);
        g_channel_build.required_window_revision =
            (g_complete_windows[channel_index].valid &&
             !g_spectrum_identity[channel_index].valid) ?
            g_complete_windows[channel_index].revision :
            channel_switch_next_revision(
                g_complete_windows[channel_index].revision);
    }
    g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
    channel_switch_set_state(RF_UI_CHANNEL_SWITCH_WAIT_WINDOW);
    return true;
}

static void channel_switch_soak_step(void)
{
    const uint32_t command_generation =
        g_rf_ui_channel_soak.command_generation;

    if(g_rf_ui_channel_soak.magic != RF_UI_CHANNEL_SOAK_MAGIC ||
       g_rf_ui_channel_soak.version != RF_UI_CHANNEL_SOAK_VERSION) {
        return;
    }

    if(command_generation != g_rf_ui_channel_soak.active_generation) {
        g_rf_ui_channel_soak.active_generation = command_generation;
        g_rf_ui_channel_soak.completed_switches = 0U;
        g_rf_ui_channel_soak.errors = 0U;
        g_rf_ui_channel_soak.next_channel =
            (g_ui.committed_channel + 1U) % RF_UI_CHANNEL_COUNT;
        g_rf_ui_channel_soak.last_requested_channel = RF_UI_CHANNEL_NONE;
        g_rf_ui_channel_soak.running =
            g_rf_ui_channel_soak.requested_switches != 0U &&
            g_rf_ui_channel_soak.requested_switches <= 10000U;
        if(g_rf_ui_channel_soak.requested_switches > 10000U) {
            g_rf_ui_channel_soak.errors = 1U;
        }
    }

    if(g_rf_ui_channel_soak.running == 0U) return;

    if(g_rf_ui_channel_soak.last_requested_channel != RF_UI_CHANNEL_NONE) {
        if(g_render_txn.active ||
           g_ui.pending_channel != g_ui.committed_channel ||
           g_channel_build.state != RF_UI_CHANNEL_SWITCH_IDLE) {
            return;
        }
        if(g_ui.committed_channel !=
           g_rf_ui_channel_soak.last_requested_channel) {
            g_rf_ui_channel_soak.errors++;
            g_rf_ui_channel_soak.running = 0U;
            return;
        }
        g_rf_ui_channel_soak.completed_switches++;
        g_rf_ui_channel_soak.last_requested_channel = RF_UI_CHANNEL_NONE;
        if(g_rf_ui_channel_soak.completed_switches >=
           g_rf_ui_channel_soak.requested_switches) {
            g_rf_ui_channel_soak.running = 0U;
            return;
        }
    }

    if(!g_ui.running || g_render_txn.active ||
       g_ui.pending_channel != g_ui.committed_channel ||
       g_channel_build.state != RF_UI_CHANNEL_SWITCH_IDLE) {
        return;
    }

    uint32_t channel =
        g_rf_ui_channel_soak.next_channel % RF_UI_CHANNEL_COUNT;
    if(channel == g_ui.committed_channel) {
        channel = (channel + 1U) % RF_UI_CHANNEL_COUNT;
    }
    if(!rf_ui_set_selected_channel(channel)) {
        g_rf_ui_channel_soak.errors++;
        g_rf_ui_channel_soak.running = 0U;
        return;
    }

    g_rf_ui_channel_soak.last_requested_channel = channel;
    g_rf_ui_channel_soak.next_channel =
        (channel + 1U) % RF_UI_CHANNEL_COUNT;
}

void rf_ui_runtime_monitor_step(void)
{
    volatile rf_ui_runtime_monitor_t * const monitor =
        &g_rf_ui_runtime_monitor;
    const uint32_t generation = monitor->command_generation;

    if(monitor->magic != RF_UI_RUNTIME_MONITOR_MAGIC ||
       monitor->version != RF_UI_RUNTIME_MONITOR_VERSION) {
        return;
    }

    if(generation != monitor->active_generation) {
        monitor->active_generation = generation;
        monitor->running = 0U;
        monitor->errors = 0U;
        if(monitor->requested_line_events == 0U ||
           monitor->requested_line_events > 180000U) {
            monitor->errors = 1U;
            return;
        }

        monitor->start_line_event = g_display_diag.glcdc_line_events;
        monitor->end_line_event = monitor->start_line_event;
        monitor->start_underflows = g_display_diag.glcdc_underflows;
        monitor->end_underflows = monitor->start_underflows;
        monitor->start_live_commits =
            g_rf_ui_channel_switch_diag.live_atomic_commits;
        monitor->end_live_commits = monitor->start_live_commits;
        monitor->start_spectrum_presents =
            g_rf_ui_channel_switch_diag.spectrum_presents;
        monitor->end_spectrum_presents =
            monitor->start_spectrum_presents;
        monitor->start_complete_windows =
            g_rf_ui_channel_switch_diag.complete_windows;
        monitor->end_complete_windows = monitor->start_complete_windows;
        monitor->start_buffer_errors =
            g_display_diag.animation_buffer_errors;
        monitor->end_buffer_errors = monitor->start_buffer_errors;
        monitor->start_overlay_presents =
            g_rf_ui_channel_switch_diag.overlay_presents;
        monitor->end_overlay_presents = monitor->start_overlay_presents;
        monitor->start_overlay_pixels =
            g_rf_ui_channel_switch_diag.overlay_pixels_advanced;
        monitor->end_overlay_pixels = monitor->start_overlay_pixels;
        monitor->start_overlay_underflows =
            g_display_diag.overlay_underflows;
        monitor->end_overlay_underflows =
            monitor->start_overlay_underflows;
        monitor->start_overlay_fallbacks =
            g_rf_ui_channel_switch_diag.overlay_fallbacks;
        monitor->end_overlay_fallbacks = monitor->start_overlay_fallbacks;
        monitor->running = 1U;
    }

    if(monitor->running != 0U &&
       (uint32_t)(g_display_diag.glcdc_line_events -
                  monitor->start_line_event) >=
           monitor->requested_line_events) {
        monitor->end_line_event = g_display_diag.glcdc_line_events;
        monitor->end_underflows = g_display_diag.glcdc_underflows;
        monitor->end_live_commits =
            g_rf_ui_channel_switch_diag.live_atomic_commits;
        monitor->end_spectrum_presents =
            g_rf_ui_channel_switch_diag.spectrum_presents;
        monitor->end_complete_windows =
            g_rf_ui_channel_switch_diag.complete_windows;
        monitor->end_buffer_errors =
            g_display_diag.animation_buffer_errors;
        monitor->end_overlay_presents =
            g_rf_ui_channel_switch_diag.overlay_presents;
        monitor->end_overlay_pixels =
            g_rf_ui_channel_switch_diag.overlay_pixels_advanced;
        monitor->end_overlay_underflows =
            g_display_diag.overlay_underflows;
        monitor->end_overlay_fallbacks =
            g_rf_ui_channel_switch_diag.overlay_fallbacks;
        monitor->running = 0U;
    }
}

uint32_t rf_ui_get_selected_channel(void)
{
    return g_ui.committed_channel;
}

uint32_t rf_ui_get_pending_channel(void)
{
    return g_ui.pending_channel;
}

bool rf_ui_channel_switch_busy(void)
{
    return g_ui.pending_channel != g_ui.committed_channel ||
           g_channel_build.state != RF_UI_CHANNEL_SWITCH_IDLE;
}

bool rf_ui_waterfall_build_busy(void)
{
    render_transaction_poll_complete();
    return g_render_txn.active ||
           g_waterfall_overlay_sync.active ||
           g_live_build.state != RF_UI_LIVE_BUILD_IDLE ||
           g_channel_build.state == RF_UI_CHANNEL_SWITCH_WATERFALL_RENDER;
}

void rf_ui_channel_switch_diag_get(
    rf_ui_channel_switch_diag_t * diagnostics)
{
    if(diagnostics == NULL) return;
    *diagnostics = g_rf_ui_channel_switch_diag;
}

void rf_ui_set_focus_mode(bool focus_mode)
{
    if(g_ui.focus_mode == focus_mode) return;
    g_ui.focus_mode = focus_mode;
    refresh_acquisition_mode();
}

bool rf_ui_is_focus_mode(void)
{
    return g_ui.focus_mode;
}

static bool update_spectrum_internal(uint32_t channel_index,
                                     const uint8_t * bins,
                                     size_t bin_count,
                                     bool identity_valid,
                                     uint32_t session_id,
                                     uint32_t window_sequence)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || bins == NULL ||
       bin_count != RF_UI_SPECTRUM_BINS) return false;

    memcpy(g_spectrum_data[channel_index], bins, RF_UI_SPECTRUM_BINS);
    g_spectrum_identity[channel_index].revision = channel_switch_next_revision(
        g_spectrum_identity[channel_index].revision);
    g_spectrum_identity[channel_index].valid = identity_valid;
    g_spectrum_identity[channel_index].session_id = session_id;
    g_spectrum_identity[channel_index].window_sequence = window_sequence;
    if(!identity_valid) {
        g_complete_windows[channel_index].spectrum_snapshot_valid = false;
    }
    else {
        complete_spectrum_snapshot_try_capture(channel_index);
    }
    g_ui.spectrum_dirty[channel_index] = true;
    return true;
}

bool rf_ui_update_spectrum(uint32_t channel_index,
                           const uint8_t * bins,
                           size_t bin_count)
{
    return update_spectrum_internal(channel_index, bins, bin_count, false,
                                    0U, 0U);
}

bool rf_ui_update_spectrum_window(uint32_t channel_index,
                                  const uint8_t * bins,
                                  size_t bin_count,
                                  uint32_t session_id,
                                  uint32_t window_sequence)
{
    return update_spectrum_internal(channel_index, bins, bin_count, true,
                                    session_id, window_sequence);
}

bool rf_ui_note_complete_window(uint32_t channel_index,
                                uint32_t session_id,
                                uint32_t window_sequence,
                                uint32_t transport_sequence)
{
    rf_ui_complete_window_t * window;
    if(channel_index >= RF_UI_CHANNEL_COUNT) return false;
    window = &g_complete_windows[channel_index];
    if(window->valid && window->session_id == session_id &&
       (int32_t)(transport_sequence - window->transport_sequence) <= 0) {
        g_rf_ui_channel_switch_diag.stale_windows++;
        return false;
    }

    window->valid = true;
    /* A new completion invalidates the previous cached spectrum until the
     * matching spectrum event arrives.  The old snapshot remains in memory
     * only until this assignment and can never be paired with the new window. */
    window->spectrum_snapshot_valid = false;
    window->spectrum_revision = 0U;
    window->revision = channel_switch_next_revision(window->revision);
    window->session_id = session_id;
    window->window_sequence = window_sequence;
    window->transport_sequence = transport_sequence;
    rf_box_window_anchor_record(
        channel_index, session_id, window_sequence,
        g_waterfall_total_columns[channel_index]);
    g_rf_ui_channel_switch_diag.complete_windows++;
    g_rf_ui_channel_switch_diag.last_session_id = session_id;
    g_rf_ui_channel_switch_diag.last_window_sequence = window_sequence;
    complete_spectrum_snapshot_try_capture(channel_index);
    return true;
}

bool rf_ui_present_spectrum(void)
{
    const uint32_t channel = g_ui.committed_channel;
    render_transaction_poll_complete();
    if(!g_ui.running || g_ui.spectrum_image == NULL ||
       channel >= RF_DEMO_CHANNEL_COUNT ||
       g_render_txn.active ||
       !g_ui.spectrum_dirty[channel] ||
       rf_ui_channel_switch_busy()) return false;

    rasterize_spectrum_to(g_spectrum_pixels[g_spectrum_active_source],
                          g_spectrum_data[channel]);
    lv_obj_invalidate(g_ui.spectrum_image);
    g_ui.spectrum_dirty[channel] = false;
    g_rf_ui_channel_switch_diag.spectrum_presents++;
    return true;
}

bool rf_ui_update_waterfall(uint32_t channel_index, const uint8_t * intensities,
                            size_t row_stride, uint32_t row_count, uint32_t column_count)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || intensities == NULL || row_count == 0u ||
       column_count == 0u || row_stride < column_count) return false;

    waterfall_clear_channel(channel_index);
    if(row_count > RF_UI_WATERFALL_HISTORY_COLS) {
        const uint32_t skip = row_count - RF_UI_WATERFALL_HISTORY_COLS;
        intensities += (size_t)skip * row_stride;
        row_count = RF_UI_WATERFALL_HISTORY_COLS;
    }
    for(uint32_t source_row = 0U; source_row < row_count; ++source_row) {
        push_waterfall_column(channel_index,
                              intensities + (size_t)source_row * row_stride,
                              column_count);
    }
    return true;
}

bool rf_ui_update_waterfall_row(uint32_t channel_index, uint32_t row_index,
                                const uint8_t * intensities, size_t column_count)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || intensities == NULL ||
       column_count == 0u) return false;

    (void)row_index;
    push_waterfall_column(channel_index, intensities, column_count);
    return true;
}

bool rf_ui_update_waterfall_rows(uint32_t channel_index,
                                 const uint8_t * intensities,
                                 size_t row_stride,
                                 uint32_t row_count,
                                 uint32_t column_count)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || intensities == NULL ||
       row_count == 0u || row_stride < column_count || column_count == 0u) return false;

    if(row_count > RF_UI_WATERFALL_HISTORY_COLS) {
        const uint32_t skip = row_count - RF_UI_WATERFALL_HISTORY_COLS;
        intensities += (size_t) skip * row_stride;
        row_count = RF_UI_WATERFALL_HISTORY_COLS;
    }

    for(uint32_t source_row = 0u; source_row < row_count; ++source_row) {
        push_waterfall_column(channel_index,
                              intensities + (size_t)source_row * row_stride,
                              column_count);
    }
    return true;
}

bool rf_ui_append_waterfall_gap_columns(uint32_t channel_index,
                                        uint32_t column_count)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || column_count == 0U) return false;

    if(column_count > RF_UI_WATERFALL_HISTORY_COLS) {
        column_count = RF_UI_WATERFALL_HISTORY_COLS;
    }
    for(uint32_t column = 0U; column < column_count; ++column) {
        push_waterfall_gap_column(channel_index);
    }
    return true;
}

bool rf_ui_waterfall_overlay_palette_get(const uint32_t ** palette,
                                         uint32_t * color_count)
{
    if(palette == NULL || color_count == NULL ||
       !g_waterfall_overlay.requested || g_waterfall_overlay.failed) {
        return false;
    }
    prepare_waterfall_lookup_tables();
    *palette = g_waterfall_clut_palette;
    *color_count = RF_UI_WATERFALL_OVERLAY_PALETTE_COLORS;
    return true;
}

static void waterfall_overlay_pacer_reset(void)
{
    g_waterfall_overlay.pace_accumulator = 0U;
    g_waterfall_overlay.pace_last_tick = lv_tick_get();
    g_waterfall_overlay.pace_tick_valid = true;
}

static uint16_t waterfall_overlay_paced_pixels(uint64_t backlog)
{
    const uint32_t now = lv_tick_get();
    if(!g_waterfall_overlay.pace_tick_valid) {
        g_waterfall_overlay.pace_last_tick = now;
        g_waterfall_overlay.pace_tick_valid = true;
    }
    const uint32_t elapsed_ms = now - g_waterfall_overlay.pace_last_tick;
    g_waterfall_overlay.pace_last_tick = now;
    if(backlog == 0U) {
        g_waterfall_overlay.pace_accumulator = 0U;
        return 0U;
    }

    const uint32_t scan_rate_x10 = g_scan_rate_x10 != 0U ?
                                   g_scan_rate_x10 :
                                   RF_WATERFALL_OVERLAY_DEFAULT_RATE_X10;
    const uint32_t channel_divisor = g_ui.focus_mode ? 1U :
                                     RF_UI_CHANNEL_COUNT;
    const uint64_t denominator =
        (uint64_t)RF_WATERFALL_OVERLAY_RATE_DENOMINATOR * channel_divisor;
    const uint64_t window_pixels =
        (uint64_t)RF_WATERFALL_RF_ROWS_PER_WINDOW *
        RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    g_waterfall_overlay.pace_accumulator +=
        (uint64_t)scan_rate_x10 * window_pixels * elapsed_ms;

    const uint64_t maximum_accumulator = denominator *
        RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC;
    if(g_waterfall_overlay.pace_accumulator > maximum_accumulator) {
        g_waterfall_overlay.pace_accumulator = maximum_accumulator;
    }
    uint32_t paced_pixels = (uint32_t)(
        g_waterfall_overlay.pace_accumulator / denominator) & ~1U;
    uint32_t pixels = paced_pixels;

    if(backlog > RF_WATERFALL_OVERLAY_CATCHUP_TARGET_PIXELS) {
        const uint64_t excess =
            backlog - RF_WATERFALL_OVERLAY_CATCHUP_TARGET_PIXELS;
        const uint32_t catchup =
            ((uint32_t)((excess + 15U) / 16U) + 1U) & ~1U;
        if(catchup > pixels) pixels = catchup;
    }
    if(pixels > RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC) {
        pixels = RF_WATERFALL_OVERLAY_MAX_PIXELS_PER_VSYNC;
    }
    if((uint64_t)pixels > backlog) pixels = (uint32_t)backlog;
    pixels &= ~1U;
    if(paced_pixels > pixels) paced_pixels = pixels;
    if(paced_pixels != 0U) {
        g_waterfall_overlay.pace_accumulator -=
            (uint64_t)paced_pixels * denominator;
    }
    return (uint16_t)pixels;
}

bool rf_ui_waterfall_overlay_prepare_frame(
    rf_ui_waterfall_overlay_frame_t * frame)
{
    if(frame == NULL || !g_waterfall_overlay.requested ||
       g_waterfall_overlay.failed || g_waterfall_overlay.awaiting_latch) {
        return false;
    }
    if(g_waterfall_overlay.prepared_valid) {
        *frame = g_waterfall_overlay.prepared_frame;
        return true;
    }

    const uint8_t source = g_waterfall_active_source;
    if(source >= RF_CHANNEL_SOURCE_COUNT ||
       !g_waterfall_source_state[source].valid ||
       g_waterfall_source_state[source].channel != g_ui.committed_channel) {
        return false;
    }
    if(g_waterfall_overlay.boxes_dirty[source]) {
        waterfall_overlay_boxes_refresh(source);
    }

    uint64_t target_end_pixels;
    if(g_ui.running) {
        target_end_pixels = g_waterfall_source_state[source].total_columns *
                            RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    }
    else {
        const uint64_t paused_end = g_waterfall_pause_total_columns >
                                    g_ui.waterfall_pan_columns ?
            g_waterfall_pause_total_columns - g_ui.waterfall_pan_columns : 0U;
        target_end_pixels = paused_end *
                            RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
    }
    /* A CLUT4 base address cannot select the high nibble of a byte. Keeping
     * the scan head even also guarantees the GLCDC-required even hsize. */
    target_end_pixels &= ~UINT64_C(1);

    uint64_t prepared_end = g_waterfall_overlay.presented_end_pixels;
    uint16_t pixels_advanced = 0U;
    if(source != g_waterfall_overlay.display_source) {
        prepared_end = target_end_pixels;
        waterfall_overlay_pacer_reset();
    }
    else if(target_end_pixels > prepared_end) {
        const uint64_t backlog = target_end_pixels - prepared_end;
        const uint32_t backlog_u32 = backlog > UINT32_MAX ?
                                     UINT32_MAX : (uint32_t)backlog;
        if(backlog_u32 >
           g_rf_ui_channel_switch_diag.overlay_max_backlog_pixels) {
            g_rf_ui_channel_switch_diag.overlay_max_backlog_pixels =
                backlog_u32;
        }
        pixels_advanced = waterfall_overlay_paced_pixels(backlog);
        prepared_end += pixels_advanced;
    }
    else if(target_end_pixels < prepared_end) {
        prepared_end = target_end_pixels;
        waterfall_overlay_pacer_reset();
    }
    else {
        waterfall_overlay_pacer_reset();
    }

    if(prepared_end == g_waterfall_overlay.presented_end_pixels &&
       source == g_waterfall_overlay.display_source &&
       !g_waterfall_overlay.visual_dirty &&
       g_waterfall_overlay.submitted_generation != 0U) {
        return false;
    }

    const uint32_t end_in_ring =
        (uint32_t)(prepared_end % RF_WATERFALL_CLUT_RING_WIDTH);
    const uint16_t pixel_head = (uint16_t)(
        (end_in_ring + RF_WATERFALL_CLUT_RING_WIDTH -
         RF_WATERFALL_DISPLAY_WIDTH) % RF_WATERFALL_CLUT_RING_WIDTH);
    const uint8_t phase = (uint8_t)(
        (pixel_head % RF_WATERFALL_CLUT_ALIGNMENT_PIXELS) /
        RF_WATERFALL_CLUT_PHASE_OFFSET_PIXELS);
    const uint16_t physical_head = (uint16_t)
        waterfall_clut4_phase_pixel(pixel_head, phase);
    const uint16_t aligned_head = (uint16_t)(
        physical_head & ~(RF_WATERFALL_CLUT_ALIGNMENT_PIXELS - 1U));
    const uint16_t prefix_pixels =
        (uint16_t)(physical_head - aligned_head);
    uint32_t generation = g_waterfall_overlay.next_generation++;
    if(generation == 0U) {
        generation = g_waterfall_overlay.next_generation++;
    }

    rf_ui_waterfall_overlay_frame_t prepared = {
        .base = &g_waterfall_render_rings[source].clut4.phase[phase].rows[0]
                                                             [aligned_head >> 1],
        .hstride_pixels = RF_WATERFALL_CLUT_STORAGE_WIDTH,
        .generation = generation,
        .hsize = (uint16_t)(RF_WATERFALL_DISPLAY_WIDTH + prefix_pixels),
        .vsize = RF_WATERFALL_DISPLAY_HEIGHT,
        .x = (int16_t)(RF_PLOT_X - prefix_pixels),
        .y = RF_WATERFALL_OVERLAY_Y,
        .pixels_advanced = pixels_advanced,
        .transparent_prefix = prefix_pixels,
        .source = source,
        .phase = phase,
    };
    g_waterfall_overlay.prepared_valid = true;
    g_waterfall_overlay.prepared_source = source;
    g_waterfall_overlay.prepared_phase = phase;
    g_waterfall_overlay.prepared_pixels = pixels_advanced;
    g_waterfall_overlay.prepared_end_pixels = prepared_end;
    g_waterfall_overlay.prepared_frame = prepared;
    g_waterfall_overlay.visual_dirty = false;
    g_rf_ui_channel_switch_diag.overlay_frame_generation = generation;
    *frame = prepared;
    return true;
}

void rf_ui_waterfall_overlay_frame_submitted(uint32_t generation)
{
    if(!g_waterfall_overlay.prepared_valid ||
       generation != g_waterfall_overlay.prepared_frame.generation) return;

    g_waterfall_overlay.display_source =
        g_waterfall_overlay.prepared_source;
    g_waterfall_overlay.display_phase =
        g_waterfall_overlay.prepared_phase;
    g_waterfall_overlay.presented_end_pixels =
        g_waterfall_overlay.prepared_end_pixels;
    g_waterfall_overlay.submitted_generation = generation;
    g_waterfall_overlay.awaiting_latch = true;
    g_waterfall_overlay.prepared_valid = false;
    g_rf_ui_channel_switch_diag.overlay_presents++;
    g_rf_ui_channel_switch_diag.overlay_pixels_advanced +=
        g_waterfall_overlay.prepared_pixels;

    const uint32_t channel = g_ui.committed_channel;
    if(channel < RF_UI_CHANNEL_COUNT) {
        g_waterfall_presented_columns[channel] =
            g_waterfall_overlay.presented_end_pixels /
            RF_WATERFALL_CLUT_PIXELS_PER_COLUMN;
        g_ui.waterfall_dirty[channel] =
            g_waterfall_total_columns[channel] *
                RF_WATERFALL_CLUT_PIXELS_PER_COLUMN >
            g_waterfall_overlay.presented_end_pixels;
    }
}

void rf_ui_waterfall_overlay_frame_latched(uint32_t generation)
{
    if(!g_waterfall_overlay.awaiting_latch ||
       generation != g_waterfall_overlay.submitted_generation) return;
    g_waterfall_overlay.awaiting_latch = false;
    g_rf_ui_channel_switch_diag.overlay_latched_generation = generation;
}

void rf_ui_waterfall_overlay_set_enabled(bool enabled)
{
    g_waterfall_overlay.enabled = enabled;
    if(!enabled && g_waterfall_overlay.failed) {
        g_waterfall_overlay.requested = false;
        g_waterfall_overlay.fallback_disable_ready = false;
        waterfall_overlay_pacer_reset();
    }
}

void rf_ui_waterfall_overlay_fail(uint32_t error)
{
    if(g_waterfall_overlay.failed) return;
    g_waterfall_overlay.failed = true;
    g_waterfall_overlay.prepared_valid = false;
    g_waterfall_overlay.awaiting_latch = false;
    g_waterfall_overlay.fallback_rebuilding = true;
    g_waterfall_overlay.fallback_disable_ready = false;
    g_display_diag.overlay_fallback_ready = 0U;
    g_rf_ui_channel_switch_diag.overlay_fallbacks++;
    g_rf_ui_channel_switch_diag.overlay_last_fallback_error = error;

    render_transaction_abort();
    live_build_cancel(true);
    waterfall_overlay_sync_cancel();
    if(g_ui.pending_channel != g_ui.committed_channel) {
        g_ui.pending_channel = g_ui.committed_channel;
        g_ui.pending_detection_index = -1;
        g_channel_build.channel = UINT8_MAX;
        g_rf_ui_channel_switch_diag.pending_channel = g_ui.committed_channel;
        g_rf_ui_channel_switch_diag.build_channel = RF_UI_CHANNEL_NONE;
        channel_switch_set_state(RF_UI_CHANNEL_SWITCH_IDLE);
    }
    waterfall_source_state_invalidate(
        (uint8_t)(g_waterfall_active_source ^ 1U));
    g_ui.waterfall_dirty[g_ui.committed_channel] = true;
}

bool rf_ui_waterfall_overlay_requested(void)
{
    return g_waterfall_overlay.requested;
}

bool rf_ui_waterfall_overlay_disable_ready(void)
{
    return g_waterfall_overlay.fallback_disable_ready;
}

bool rf_ui_present_waterfall(void)
{
    const uint32_t channel = g_ui.committed_channel;
    render_transaction_poll_complete();
    if(g_ui.waterfall_image == NULL ||
       channel >= RF_DEMO_CHANNEL_COUNT ||
       (!g_ui.waterfall_dirty[channel] &&
         !g_ui.rf_boxes_dirty[channel]) ||
       rf_ui_channel_switch_busy()) return false;

    if(g_waterfall_overlay.requested && !g_waterfall_overlay.failed) {
        if(g_waterfall_overlay_sync.active &&
           waterfall_overlay_sync_step()) {
            return true;
        }
        if(g_live_build.state != RF_UI_LIVE_BUILD_IDLE) {
            return live_build_step();
        }
        const rf_ui_waterfall_source_state_t * const state =
            &g_waterfall_source_state[g_waterfall_active_source];
        if(g_ui.running &&
           (!state->valid || state->channel != channel ||
            state->total_columns < g_waterfall_total_columns[channel])) {
            if(waterfall_overlay_sync_start(channel)) {
                return waterfall_overlay_sync_step();
            }
            if(live_build_start(channel)) return live_build_step();
            return false;
        }
        bool refreshed = false;
        if(g_ui.rf_boxes_dirty[channel]) {
            refresh_rf_box_overlays();
            g_ui.rf_boxes_dirty[channel] = false;
            refreshed = true;
        }
        g_waterfall_overlay.visual_dirty = true;
        return refreshed;
    }

    if(!g_ui.running) return false;

    if(g_render_txn.active) {
        return render_transaction_step();
    }
    if(g_live_build.state != RF_UI_LIVE_BUILD_IDLE) {
        return live_build_step();
    }

    if(g_ui.waterfall_dirty[channel]) {
        /* Keep the descriptor stable on the currently displayed source until
         * the private build has a complete, bounded image. */
        waterfall_image_head_update();
        if(g_waterfall_total_columns[channel] >
           g_waterfall_presented_columns[channel]) {
            if(live_build_start(channel)) {
                return live_build_step();
            }
            return false;
        }
        g_ui.waterfall_dirty[channel] = false;
    }
    if(g_ui.rf_boxes_dirty[channel]) {
        refresh_rf_box_overlays();
        g_ui.rf_boxes_dirty[channel] = false;
        return true;
    }
    return false;
}

bool rf_ui_update_channel_metrics(uint32_t channel_index,
                                  const rf_ui_channel_metrics_t * metrics)
{
    if(channel_index >= RF_DEMO_CHANNEL_COUNT || metrics == NULL ||
       metrics->occupancy_percent > 100u) return false;

    const rf_ui_channel_metrics_t previous = g_channel_metrics[channel_index];
    if(previous.peak_dbfs == metrics->peak_dbfs &&
       previous.noise_floor_dbfs == metrics->noise_floor_dbfs &&
       previous.occupancy_percent == metrics->occupancy_percent &&
       previous.age_ms == metrics->age_ms) return true;
    g_channel_metrics[channel_index] = *metrics;

    if(channel_switch_defer_metadata_refresh()) return true;

    if(previous.occupancy_percent != metrics->occupancy_percent) {
        refresh_selector_styles();
    }
    if(previous.peak_dbfs != metrics->peak_dbfs &&
       g_ui.compare_overlay != NULL &&
       !lv_obj_has_flag(g_ui.compare_overlay, LV_OBJ_FLAG_HIDDEN)) {
        refresh_compare_overlay();
    }
    if(channel_index != g_ui.committed_channel) return true;

    if(previous.peak_dbfs != metrics->peak_dbfs) {
        refresh_selected_metric(RF_CHANNEL_METRIC_PEAK);
    }
    if(previous.noise_floor_dbfs != metrics->noise_floor_dbfs) {
        refresh_selected_metric(RF_CHANNEL_METRIC_NOISE);
    }
    if(previous.occupancy_percent != metrics->occupancy_percent) {
        refresh_selected_metric(RF_CHANNEL_METRIC_OCCUPANCY);
    }
    if(previous.age_ms != metrics->age_ms) {
        refresh_selected_metric(RF_CHANNEL_METRIC_AGE);
    }
    return true;
}

bool rf_ui_update_detection(uint32_t detection_index, const rf_ui_detection_t * detection)
{
    if(detection_index >= RF_DEMO_CLASS_COUNT || detection == NULL ||
       detection->state > RF_UI_DETECTION_INACTIVE ||
       detection->confidence_percent > 100u ||
       detection->channel_index >= RF_DEMO_CHANNEL_COUNT) return false;

    const rf_ui_detection_t previous = g_detections[detection_index];
    if(previous.state == detection->state &&
       previous.confidence_percent == detection->confidence_percent &&
       previous.channel_index == detection->channel_index) return true;

    g_detections[detection_index] = *detection;
    if(channel_switch_defer_metadata_refresh()) return true;
    refresh_alert(false);
    return true;
}

static uint32_t rf_box_observation_next(void)
{
    g_rf_box_observation_generation++;
    if(g_rf_box_observation_generation == 0U) {
        g_rf_box_observation_generation++;
    }
    return g_rf_box_observation_generation;
}

static bool rf_box_window_identity_matches(
    const rf_ui_fusion_round_t * round,
    const rf_ui_pending_box_batch_t * batch)
{
    if(round == NULL || batch == NULL || !batch->valid ||
       batch->channel >= RF_UI_CHANNEL_COUNT) return false;

    const uint8_t channel_bit = (uint8_t)(1U << batch->channel);
    return (round->identity_mask & channel_bit) != 0U &&
           round->session_id[batch->channel] == batch->session_id &&
           round->window_sequence[batch->channel] == batch->window_sequence;
}

static bool rf_box_window_is_latest(
    const rf_ui_pending_box_batch_t * batch)
{
    return batch != NULL && batch->channel < RF_UI_CHANNEL_COUNT &&
           g_latest_box_window_valid[batch->channel] &&
           g_latest_box_session_id[batch->channel] == batch->session_id &&
           g_latest_box_window_sequence[batch->channel] ==
               batch->window_sequence;
}

static bool rf_box_history_is_retained(
    uint32_t channel,
    uint64_t anchor_end_column,
    const rf_ui_rf_box_t * box)
{
    rf_ui_waterfall_box_bounds_t bounds;
    if(channel >= RF_UI_CHANNEL_COUNT ||
       !waterfall_box_history_bounds(anchor_end_column, box, &bounds)) {
        return false;
    }

    const uint64_t current_end = g_waterfall_total_columns[channel];
    const uint64_t retained_start = current_end > RF_UI_WATERFALL_HISTORY_COLS ?
        current_end - RF_UI_WATERFALL_HISTORY_COLS : 0U;
    return anchor_end_column <= current_end &&
           bounds.start_column >= retained_start &&
           bounds.end_column <= current_end;
}

static void rf_box_pending_release(rf_ui_pending_box_batch_t * batch)
{
    if(batch == NULL || !batch->valid) return;
    if(batch->queued &&
       g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion != 0U) {
        g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion--;
    }
    memset(batch, 0, sizeof(*batch));
}

static void rf_box_pending_drop_stale(rf_ui_pending_box_batch_t * batch)
{
    if(batch == NULL || !batch->valid) return;
    g_rf_ui_channel_switch_diag.history_boxes_dropped_stale += batch->count;
    rf_box_pending_release(batch);
}

static void rf_box_detail_update(uint32_t channel,
                                 const rf_ui_rf_box_t * box,
                                 uint32_t observation_generation,
                                 uint64_t anchor_end_column,
                                 uint32_t round_index)
{
    if(channel >= RF_UI_CHANNEL_COUNT || box == NULL ||
       box->detection_index >= RF_UI_DETECTION_COUNT) return;

    const uint32_t detection = box->detection_index;
    const uint8_t detection_bit = (uint8_t)(1U << detection);
    if((g_last_detail_round_valid_mask[channel] & detection_bit) != 0U &&
       (int32_t)(round_index -
                 g_last_detail_round_index[channel][detection]) < 0) {
        return;
    }

    rf_ui_rf_box_batch_t * const detail = &g_rf_box_batches[channel];
    uint32_t destination = detail->count;
    for(uint32_t index = 0U; index < detail->count; ++index) {
        if(detail->boxes[index].detection_index == detection) {
            destination = index;
            break;
        }
    }
    if(destination >= RF_UI_MAX_RF_BOXES) return;
    if(destination == detail->count) detail->count++;
    detail->boxes[destination] = *box;
    detail->observation_generation[destination] = observation_generation;
    detail->anchor_end_columns[destination] = anchor_end_column;
    g_last_detail_round_index[channel][detection] = round_index;
    g_last_detail_round_valid_mask[channel] |= detection_bit;
}

static void rf_box_batch_resolve(
    rf_ui_fusion_decision_cache_t * decision,
    rf_ui_pending_box_batch_t * batch)
{
    if(decision == NULL || batch == NULL || !batch->valid ||
       !rf_box_window_identity_matches(&decision->round, batch)) return;

    const uint8_t channel_bit = (uint8_t)(1U << batch->channel);
    if((decision->round.identity_conflict_mask & channel_bit) != 0U) {
        g_rf_ui_channel_switch_diag.history_boxes_dropped_identity_mismatch +=
            batch->count;
        decision->processed_channel_mask |= channel_bit;
        rf_box_pending_release(batch);
        return;
    }
    if((decision->round.flags & RF_UI_FUSION_ROUND_OUTPUT_VALID) == 0U) {
        g_rf_ui_channel_switch_diag.history_boxes_dropped_uncertain +=
            batch->count;
        decision->processed_channel_mask |= channel_bit;
        rf_box_pending_release(batch);
        return;
    }

    rf_ui_rf_box_batch_t spectrum_next = {0};
    uint32_t observation_generation = 0U;
    bool visual_dirty = rf_box_window_is_latest(batch);
    bool committed = false;
    for(uint32_t index = 0U; index < batch->count; ++index) {
        const rf_ui_rf_box_t * const box = &batch->boxes[index];
        const uint32_t detection = box->detection_index;
        const uint8_t activity = decision->round.activity_state[detection];
        if(activity == RF_UI_FUSION_WORKING) {
            if(observation_generation == 0U) {
                observation_generation = rf_box_observation_next();
            }
            if(visual_dirty && spectrum_next.count < RF_UI_MAX_RF_BOXES) {
                spectrum_next.boxes[spectrum_next.count] = *box;
                spectrum_next.observation_generation[spectrum_next.count] =
                    observation_generation;
                spectrum_next.anchor_end_columns[spectrum_next.count] =
                    batch->anchor_end_column;
                spectrum_next.count++;
            }
            if(!rf_box_history_is_retained(
                   batch->channel, batch->anchor_end_column, box)) {
                g_rf_ui_channel_switch_diag
                    .history_boxes_dropped_out_of_history++;
                continue;
            }
            if(waterfall_history_box_raster(
                   batch->channel, batch->anchor_end_column, box) == 0U) {
                g_rf_ui_channel_switch_diag
                    .history_boxes_dropped_out_of_history++;
                continue;
            }
            waterfall_overlay_box_raster_matching_sources(
                batch->channel, batch->anchor_end_column, box);
            rf_box_detail_update(
                batch->channel, box, observation_generation,
                batch->anchor_end_column, decision->round.round_index);
            g_rf_ui_channel_switch_diag.history_boxes_committed_working++;
            committed = true;
            visual_dirty = true;
        }
        else if(activity == RF_UI_FUSION_NO_RF) {
            g_rf_ui_channel_switch_diag.history_boxes_dropped_idle++;
        }
        else {
            g_rf_ui_channel_switch_diag.history_boxes_dropped_uncertain++;
        }
    }

    if(rf_box_window_is_latest(batch)) {
        g_spectrum_rf_box_batches[batch->channel] = spectrum_next;
    }
    if(committed) {
        g_rf_ui_channel_switch_diag.last_committed_round_index =
            decision->round.round_index;
    }
    if(visual_dirty) {
        g_ui.rf_boxes_dirty[batch->channel] = true;
        (void)channel_switch_defer_metadata_refresh();
    }
    decision->processed_channel_mask |= channel_bit;
    rf_box_pending_release(batch);
}

static rf_ui_fusion_decision_cache_t * rf_box_decision_find(
    const rf_ui_pending_box_batch_t * batch)
{
    for(uint32_t index = 0U;
        index < RF_UI_FUSION_DECISION_CACHE_CAPACITY; ++index) {
        rf_ui_fusion_decision_cache_t * const cached =
            &g_fusion_decision_cache[index];
        if(cached->valid &&
           rf_box_window_identity_matches(&cached->round, batch)) {
            return cached;
        }
    }
    return NULL;
}

static rf_ui_pending_box_batch_t * rf_box_pending_slot_get(
    uint32_t channel,
    uint32_t session_id,
    uint32_t window_sequence)
{
    rf_ui_pending_box_batch_t * free_slot = NULL;
    rf_ui_pending_box_batch_t * oldest = NULL;
    uint32_t oldest_age = 0U;
    for(uint32_t index = 0U;
        index < RF_UI_PENDING_BOX_BATCH_CAPACITY; ++index) {
        rf_ui_pending_box_batch_t * const candidate =
            &g_pending_box_batches[index];
        if(candidate->valid && candidate->channel == channel &&
           candidate->session_id == session_id &&
           candidate->window_sequence == window_sequence) {
            return candidate;
        }
        if(!candidate->valid && free_slot == NULL) free_slot = candidate;
        if(candidate->valid) {
            const uint32_t age = g_fusion_decision_generation -
                                 candidate->staged_decision_generation;
            if(oldest == NULL || age > oldest_age) {
                oldest = candidate;
                oldest_age = age;
            }
        }
    }
    return free_slot != NULL ? free_slot : oldest;
}

bool rf_ui_update_rf_boxes(uint32_t channel_index,
                           const rf_ui_rf_box_t * boxes,
                           size_t box_count,
                           uint32_t session_id,
                           uint32_t window_sequence)
{
    uint64_t waterfall_end_column;
    if(channel_index >= RF_UI_CHANNEL_COUNT || session_id == 0U ||
       box_count > RF_UI_MAX_RF_BOXES ||
       (box_count != 0U && boxes == NULL)) return false;

    for(size_t index = 0U; index < box_count; ++index) {
        const rf_ui_rf_box_t * box = &boxes[index];
        const uint32_t frequency_end =
            (uint32_t)box->frequency_start_q8 + box->frequency_span_q8;
        const uint32_t time_end =
            (uint32_t)box->time_start_q8 + box->time_span_q8;
        if((box->flags & RF_UI_RF_BOX_FLAG_VALID) == 0U ||
           box->frequency_span_q8 == 0U || box->time_span_q8 == 0U ||
           frequency_end > RF_UI_RF_COORD_SCALE ||
           time_end > RF_UI_RF_COORD_SCALE ||
           box->detection_index >= RF_UI_DETECTION_COUNT ||
           box->confidence_percent > 100U) return false;
    }
    if(!rf_box_window_anchor_find(channel_index, session_id, window_sequence,
                                  &waterfall_end_column)) return false;

    g_rf_ui_channel_switch_diag.raw_boxes_received += (uint32_t)box_count;
    g_latest_box_window_valid[channel_index] = true;
    g_latest_box_session_id[channel_index] = session_id;
    g_latest_box_window_sequence[channel_index] = window_sequence;
    memset(&g_spectrum_rf_box_batches[channel_index], 0,
           sizeof(g_spectrum_rf_box_batches[channel_index]));
    g_ui.rf_boxes_dirty[channel_index] = true;
    (void)channel_switch_defer_metadata_refresh();

    rf_ui_pending_box_batch_t incoming = {0};
    incoming.valid = true;
    incoming.channel = (uint8_t)channel_index;
    incoming.count = (uint8_t)box_count;
    incoming.session_id = session_id;
    incoming.window_sequence = window_sequence;
    incoming.staged_decision_generation = g_fusion_decision_generation;
    incoming.anchor_end_column = waterfall_end_column;
    if(box_count != 0U) {
        memcpy(incoming.boxes, boxes, box_count * sizeof(boxes[0]));
    }

    rf_ui_fusion_decision_cache_t * const cached = rf_box_decision_find(
        &incoming);
    if(cached != NULL) {
        const uint8_t channel_bit = (uint8_t)(1U << channel_index);
        if((cached->processed_channel_mask & channel_bit) != 0U) {
            g_rf_ui_channel_switch_diag.history_boxes_dropped_stale +=
                (uint32_t)box_count;
            return true;
        }
    }

    rf_ui_pending_box_batch_t * const slot = rf_box_pending_slot_get(
        channel_index, session_id, window_sequence);
    if(slot == NULL) {
        g_rf_ui_channel_switch_diag.history_boxes_dropped_stale +=
            (uint32_t)box_count;
        return true;
    }
    const bool replacing = slot->valid;
    if(replacing) {
        g_rf_ui_channel_switch_diag.history_boxes_dropped_stale += slot->count;
    }
    else {
        g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion++;
        if(g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion >
           g_rf_ui_channel_switch_diag.pending_box_batch_high_water) {
            g_rf_ui_channel_switch_diag.pending_box_batch_high_water =
                g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion;
        }
    }
    incoming.queued = true;
    *slot = incoming;
    return true;
}

bool rf_ui_box_fusion_step(void)
{
    for(uint32_t priority = 0U; priority < 2U; ++priority) {
        for(uint32_t index = 0U;
            index < RF_UI_PENDING_BOX_BATCH_CAPACITY; ++index) {
            rf_ui_pending_box_batch_t * const batch =
                &g_pending_box_batches[index];
            if(!batch->valid ||
               ((priority == 0U) !=
                (batch->channel == g_ui.committed_channel))) continue;

            rf_ui_fusion_decision_cache_t * const decision =
                rf_box_decision_find(batch);
            if(decision == NULL) continue;
            const uint8_t channel_bit = (uint8_t)(1U << batch->channel);
            if((decision->processed_channel_mask & channel_bit) != 0U) {
                rf_box_pending_drop_stale(batch);
                return true;
            }
            rf_box_batch_resolve(decision, batch);
            return true;
        }
    }
    return false;
}

void rf_ui_reset_rf_box_fusion(void)
{
    for(uint32_t index = 0U;
        index < RF_UI_PENDING_BOX_BATCH_CAPACITY; ++index) {
        if(g_pending_box_batches[index].valid) {
            g_rf_ui_channel_switch_diag.history_boxes_dropped_stale +=
                g_pending_box_batches[index].count;
        }
    }
    memset(g_pending_box_batches, 0, sizeof(g_pending_box_batches));
    memset(g_fusion_decision_cache, 0, sizeof(g_fusion_decision_cache));
    memset(g_window_anchors, 0, sizeof(g_window_anchors));
    g_window_anchor_write_index = 0U;
    memset(g_latest_box_window_valid, 0, sizeof(g_latest_box_window_valid));
    memset(g_latest_box_session_id, 0, sizeof(g_latest_box_session_id));
    memset(g_latest_box_window_sequence, 0,
           sizeof(g_latest_box_window_sequence));
    memset(g_last_detail_round_index, 0,
           sizeof(g_last_detail_round_index));
    memset(g_last_detail_round_valid_mask, 0,
           sizeof(g_last_detail_round_valid_mask));
    memset(g_spectrum_rf_box_batches, 0,
           sizeof(g_spectrum_rf_box_batches));
    g_fusion_decision_generation = 0U;
    g_rf_ui_channel_switch_diag.box_batches_waiting_for_fusion = 0U;
    for(uint32_t channel = 0U; channel < RF_UI_CHANNEL_COUNT; ++channel) {
        g_ui.rf_boxes_dirty[channel] = true;
    }
    (void)channel_switch_defer_metadata_refresh();
}

void rf_ui_apply_fusion_round(const rf_ui_fusion_round_t * round)
{
    if(round == NULL) return;
    if((round->flags & RF_UI_FUSION_ROUND_CPU0_RESET) != 0U) {
        rf_ui_reset_rf_box_fusion();
    }
    if((round->flags & RF_UI_FUSION_ROUND_HAS_MESSAGE) == 0U ||
       round->round_index == 0U || round->message_sequence == 0U) return;

    rf_ui_fusion_round_t normalized = *round;
    for(uint32_t channel = 0U; channel < RF_UI_CHANNEL_COUNT; ++channel) {
        const uint8_t bit = (uint8_t)(1U << channel);
        if((normalized.identity_mask & bit) != 0U &&
           normalized.session_id[channel] == 0U) {
            normalized.identity_conflict_mask |= bit;
        }
    }
    if((normalized.flags & RF_UI_FUSION_ROUND_OUTPUT_VALID) != 0U) {
        for(uint32_t detection = 0U;
            detection < RF_UI_DETECTION_COUNT; ++detection) {
            if(normalized.activity_state[detection] > RF_UI_FUSION_WORKING) {
                normalized.flags &= (uint8_t)~RF_UI_FUSION_ROUND_OUTPUT_VALID;
                break;
            }
        }
    }

    g_fusion_decision_generation++;
    if(g_fusion_decision_generation == 0U) g_fusion_decision_generation++;

    rf_ui_fusion_decision_cache_t * destination = NULL;
    rf_ui_fusion_decision_cache_t * oldest = NULL;
    uint32_t oldest_age = 0U;
    for(uint32_t index = 0U;
        index < RF_UI_FUSION_DECISION_CACHE_CAPACITY; ++index) {
        rf_ui_fusion_decision_cache_t * const candidate =
            &g_fusion_decision_cache[index];
        if(candidate->valid &&
           candidate->round.round_index == normalized.round_index) {
            if((candidate->round.flags &
                RF_UI_FUSION_ROUND_OUTPUT_VALID) != 0U &&
               (normalized.flags & RF_UI_FUSION_ROUND_OUTPUT_VALID) == 0U) {
                destination = candidate;
                break;
            }
            destination = candidate;
            break;
        }
        if(!candidate->valid && destination == NULL) destination = candidate;
        if(candidate->valid) {
            const uint32_t age = g_fusion_decision_generation -
                                 candidate->generation;
            if(oldest == NULL || age > oldest_age) {
                oldest = candidate;
                oldest_age = age;
            }
        }
    }
    if(destination == NULL) destination = oldest;
    if(destination == NULL) return;

    if(destination->valid &&
       destination->round.round_index == normalized.round_index &&
       (destination->round.flags & RF_UI_FUSION_ROUND_OUTPUT_VALID) != 0U &&
       (normalized.flags & RF_UI_FUSION_ROUND_OUTPUT_VALID) == 0U) {
        /* Duplicate/stale invalid messages cannot replace an accepted round. */
    }
    else {
        memset(destination, 0, sizeof(*destination));
        destination->valid = true;
        destination->generation = g_fusion_decision_generation;
        destination->round = normalized;
    }

    for(uint32_t index = 0U;
        index < RF_UI_PENDING_BOX_BATCH_CAPACITY; ++index) {
        rf_ui_pending_box_batch_t * const batch =
            &g_pending_box_batches[index];
        if(batch->valid &&
           (g_fusion_decision_generation -
            batch->staged_decision_generation) >=
               RF_UI_FUSION_DECISION_CACHE_CAPACITY) {
            rf_box_pending_drop_stale(batch);
        }
    }
}

void rf_ui_mark_channel_result(uint32_t channel_index,
                               uint32_t window_sequence,
                               uint8_t confidence_percent)
{
    (void) window_sequence;
    if(channel_index >= RF_DEMO_CHANNEL_COUNT ||
       g_ui.selector_buttons[channel_index] == NULL ||
       g_ui.selector_bars[channel_index] == NULL) return;

    if(confidence_percent > 100u) confidence_percent = 100u;
    const uint32_t result_color = confidence_percent >= 50u ? RF_COLOR_RED :
                                  (confidence_percent >= 25u ? RF_COLOR_AMBER :
                                   RF_COLOR_GREEN);
    g_ui.selector_pulse[channel_index] = !g_ui.selector_pulse[channel_index];
    if(channel_switch_defer_metadata_refresh()) return;
    lv_obj_set_style_bg_color(g_ui.selector_bars[channel_index],
                              color(result_color), 0);
    lv_obj_set_style_opa(g_ui.selector_bars[channel_index],
                         g_ui.selector_pulse[channel_index] ?
                         LV_OPA_COVER : (lv_opa_t) 176u, 0);
    lv_obj_invalidate(g_ui.selector_buttons[channel_index]);
}

void rf_ui_force_channel_result_redraw(uint32_t channel_index)
{
    if(channel_index < RF_DEMO_CHANNEL_COUNT &&
       g_ui.selector_buttons[channel_index] != NULL) {
        if(channel_index == g_ui.committed_channel) {
            g_ui.spectrum_dirty[channel_index] = true;
        }
        if(channel_switch_defer_metadata_refresh()) return;
        lv_obj_invalidate(g_ui.selector_buttons[channel_index]);
    }
}

void rf_ui_set_scan_rate_x10(uint16_t rate_x10)
{
    if(g_scan_rate_x10 == rate_x10) return;
    g_scan_rate_x10 = rate_x10;
    refresh_scan_rate();
}

void rf_ui_set_global_tile_rate_millihz(uint32_t global_rate_millihz)
{
    /* Retained for source compatibility.  Transport cadence is diagnostic
     * only and must not trigger text redraws or distort the RF-time axis. */
    (void)global_rate_millihz;
}

void rf_ui_set_render_metrics(uint32_t panel_millihz, uint32_t presented_millihz,
                               uint32_t render_max_us, uint32_t underflow_count)
{
    const bool panel_changed = !g_render_metrics.valid ||
                               g_render_metrics.panel_millihz != panel_millihz;
    const bool presented_changed = !g_render_metrics.valid ||
                                   g_render_metrics.presented_millihz != presented_millihz;
    const bool render_max_changed = !g_render_metrics.valid ||
                                    g_render_metrics.render_max_us != render_max_us;
    const bool underflow_changed = !g_render_metrics.valid ||
                                   g_render_metrics.underflow_count != underflow_count;
    const bool underflow_state_changed = !g_render_metrics.valid ||
                                         (g_render_metrics.underflow_count == 0u) !=
                                         (underflow_count == 0u);

    g_render_metrics.panel_millihz = panel_millihz;
    g_render_metrics.presented_millihz = presented_millihz;
    g_render_metrics.render_max_us = render_max_us;
    g_render_metrics.underflow_count = underflow_count;
    g_render_metrics.valid = true;

    char text[24];
    if(panel_changed && g_ui.performance_labels[RF_METRIC_PANEL] != NULL) {
        format_millihz(text, sizeof(text), "Panel", panel_millihz);
        lv_label_set_text(g_ui.performance_labels[RF_METRIC_PANEL], text);
    }
    if(presented_changed && g_ui.performance_labels[RF_METRIC_PRESENTED] != NULL) {
        format_millihz(text, sizeof(text), "FPS", presented_millihz);
        lv_label_set_text(g_ui.performance_labels[RF_METRIC_PRESENTED], text);
    }
    if(presented_changed && g_ui.side_refresh_label != NULL) {
        lv_label_set_text_fmt(g_ui.side_refresh_label, "%u.%u FPS",
                              (unsigned)(presented_millihz / 1000u),
                              (unsigned)((presented_millihz % 1000u) / 100u));
    }
    if(render_max_changed && g_ui.performance_labels[RF_METRIC_RENDER_MAX] != NULL) {
        format_render_max(text, sizeof(text), render_max_us);
        lv_label_set_text(g_ui.performance_labels[RF_METRIC_RENDER_MAX], text);
    }
    if(underflow_changed && g_ui.performance_labels[RF_METRIC_UNDERFLOW] != NULL) {
        snprintf(text, sizeof(text), "UF %u", (unsigned) underflow_count);
        lv_label_set_text(g_ui.performance_labels[RF_METRIC_UNDERFLOW], text);
    }
    if(underflow_state_changed && g_ui.performance_labels[RF_METRIC_UNDERFLOW] != NULL) {
        lv_obj_set_style_text_color(g_ui.performance_labels[RF_METRIC_UNDERFLOW],
                                    color(underflow_count == 0u ? RF_COLOR_MUTED : RF_COLOR_RED), 0);
    }
}
