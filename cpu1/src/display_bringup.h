#ifndef DISPLAY_BRINGUP_H
#define DISPLAY_BRINGUP_H

#include "hal_data.h"

typedef enum e_display_stage
{
    DISPLAY_STAGE_RESET = 0,
    DISPLAY_STAGE_FRAMEBUFFER_READY = 1,
    DISPLAY_STAGE_HOST_OPEN = 2,
    DISPLAY_STAGE_PANEL_RESET = 3,
    DISPLAY_STAGE_PANEL_CONFIGURED = 4,
    DISPLAY_STAGE_VIDEO_STARTED = 5,
    DISPLAY_STAGE_BACKLIGHT_ON = 6,
    DISPLAY_STAGE_FAILED = 0xFF
} display_stage_t;

typedef enum e_display_underflow_context
{
    DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_RESYNC  = (1UL << 0),
    DISPLAY_UNDERFLOW_CONTEXT_CHANNEL_SWITCH   = (1UL << 1),
    DISPLAY_UNDERFLOW_CONTEXT_SPECTRUM_PRESENT = (1UL << 2),
    DISPLAY_UNDERFLOW_CONTEXT_WATERFALL_PRESENT = (1UL << 3),
    DISPLAY_UNDERFLOW_CONTEXT_LVGL_REFRESH     = (1UL << 4),
    DISPLAY_UNDERFLOW_CONTEXT_FLUSH_WAIT       = (1UL << 5),
    DISPLAY_UNDERFLOW_CONTEXT_TILE_DRAIN       = (1UL << 6),
    DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_DRAW    = (1UL << 7),
    DISPLAY_UNDERFLOW_CONTEXT_DEFERRED_COMMIT  = (1UL << 8),
    DISPLAY_UNDERFLOW_CONTEXT_NORMAL_REFRESH   = (1UL << 9),
} display_underflow_context_t;

typedef struct st_display_diag
{
    uint32_t magic;
    uint32_t stage;
    int32_t  last_error;
    uint32_t last_command_index;
    uint32_t commands_sent;
    uint32_t command_callbacks;
    uint32_t last_tx_status;
    uint32_t video_status;
    uint32_t fatal_status;
    uint32_t phy_status;
    uint32_t glcdc_line_events;
    uint32_t glcdc_underflows;
    uint32_t running;
    uint32_t heartbeat;
    uint32_t reset_pin;
    uint32_t reset_active_level;
    uint32_t reset_sequence_done;
    uint32_t reset_idle_level;
    uint32_t panel_read_error;
    uint32_t panel_read_tx_status;
    uint32_t panel_receive_status;
    uint32_t panel_read_result;
    uint32_t panel_power_mode;
    uint32_t panel_read_buffer;
    uint32_t panel_receive_events;
    uint32_t dsi_link_status;
    uint32_t dsi_ack_latest;
    uint32_t dsi_ack_accumulated;
    uint32_t sdram_test_base;
    uint32_t sdram_test_offset;
    uint32_t sdram_alias_test_passed;
    uint32_t external_video_timing_applied;
    uint32_t bist_disable_error;
    uint32_t dsi_error_read_error;
    uint32_t panel_dsi_error_count;
    uint32_t video_status_before_diagnostic;
    uint32_t video_stop_error;
    uint32_t video_restart_error;
    uint32_t active_dsi_lanes;
    uint32_t panel_clock_divisor;
    uint32_t panel_clock_hz;
    uint32_t horizontal_total_cyc;
    uint32_t vertical_total_cyc;
    uint32_t refresh_millihz;
    uint32_t panel_lane_read_error;
    uint32_t panel_lane_config;
    uint32_t panel_lane_control;
    uint32_t animation_frames;
    uint32_t animation_buffer_changes;
    uint32_t animation_buffer_errors;
    uint32_t animation_last_error;
    uint32_t animation_last_x;
    uint32_t animation_visible_buffer;
    uint32_t animation_next_buffer;
    uint32_t animation_last_line_event;
    uint32_t measured_refresh_millihz;
    uint32_t fps_measure_frames;
    uint32_t fps_measure_cycles;
    uint32_t fps_updates;
    uint32_t fps_core_clock_hz;
    uint32_t fps_counter_enabled;
    uint32_t fps_display_millihz;
    uint32_t lvgl_refresh_avg_cycles;
    uint32_t lvgl_refresh_max_cycles;
    uint32_t lvgl_flush_wait_avg_cycles;
    uint32_t lvgl_flush_wait_max_cycles;
    uint32_t lvgl_profile_updates;
    uint32_t lvgl_deferred_begins;
    uint32_t lvgl_deferred_flushes;
    uint32_t lvgl_deferred_commits;
    uint32_t lvgl_deferred_aborts;
    uint32_t lvgl_deferred_max_area_bytes;
    uint32_t lvgl_deferred_resync_starts;
    uint32_t lvgl_deferred_resync_chunks;
    uint32_t lvgl_deferred_resync_completions;
    uint32_t lvgl_deferred_resync_last_bytes;
    uint32_t lvgl_deferred_resync_max_bytes;
    uint32_t lvgl_deferred_resync_total_bytes;
    uint32_t lvgl_sdram_work_slots;
    uint32_t lvgl_refresh_skips_after_resync;
    uint32_t underflow_context;
    uint32_t underflow_last_context;
    uint32_t underflow_unattributed;
    uint32_t underflow_deferred_resync;
    uint32_t underflow_channel_switch;
    uint32_t underflow_spectrum_present;
    uint32_t underflow_waterfall_present;
    uint32_t underflow_lvgl_refresh;
    uint32_t underflow_flush_wait;
    uint32_t underflow_tile_drain;
    uint32_t underflow_deferred_draw;
    uint32_t underflow_deferred_commit;
    uint32_t underflow_normal_refresh;
    uint32_t overlay_updates;
    uint32_t overlay_errors;
    uint32_t overlay_last_error;
    uint32_t overlay_enabled;
    uint32_t overlay_underflows;
    uint32_t overlay_state;
    uint32_t overlay_startup_underflows_tolerated;
    uint32_t overlay_enable_clean_vsyncs;
    uint32_t overlay_fallback_ready;
    uint32_t overlay_fallback_completions;
    uint32_t overlay_monitor_windows;
    uint32_t overlay_monitor_start_line;
    uint32_t overlay_monitor_end_line;
    uint32_t overlay_monitor_start_underflows;
    uint32_t overlay_monitor_end_underflows;
    uint32_t overlay_monitor_start_layer2_underflows;
    uint32_t overlay_monitor_end_layer2_underflows;
    uint32_t dsi_timing_verified;
    uint32_t horizontal_sync_cyc;
    uint32_t horizontal_back_porch_cyc;
    uint32_t horizontal_front_porch_cyc;
    uint32_t vertical_sync_cyc;
    uint32_t vertical_back_porch_cyc;
    uint32_t vertical_front_porch_cyc;
    uint32_t dsi_video_mode_delay;
    /* Append-only startup diagnostics. Keep legacy offsets above stable. */
    uint32_t startup_pin_levels_valid;
    uint32_t startup_backlight_initial_level;
    uint32_t startup_reset_initial_level;
    uint32_t startup_backlight_low_asserted;
    uint32_t startup_reset_asserted;
    uint32_t startup_reset_released;
    uint32_t startup_black_framebuffer_ready;
    uint32_t startup_panel_configured;
    uint32_t startup_video_started;
    uint32_t startup_gate_steps;
    uint32_t startup_gate_waits;
    uint32_t startup_wait_flags;
    uint32_t startup_clean_vsync_required;
    uint32_t startup_clean_vsync_count;
    uint32_t startup_clean_vsync_restarts;
    uint32_t startup_clean_last_line_event;
    uint32_t startup_backlight_enable_attempts;
    uint32_t startup_backlight_enabled;
    uint32_t startup_backlight_line_event;
    uint32_t startup_backlight_readback;
    uint32_t startup_backlight_transitions;
    uint32_t startup_gate_last_error;
    uint32_t startup_before_underflows;
    uint32_t startup_before_layer2_underflows;
    uint32_t startup_before_buffer_errors;
    uint32_t startup_before_overlay_errors;
    uint32_t startup_before_video_status;
    uint32_t startup_before_fatal_status;
    uint32_t startup_before_phy_status;
    uint32_t startup_last_underflows;
    uint32_t startup_last_layer2_underflows;
    uint32_t startup_last_buffer_errors;
    uint32_t startup_last_overlay_errors;
    uint32_t startup_last_video_status;
    uint32_t startup_last_fatal_status;
    uint32_t startup_last_phy_status;
    uint32_t startup_warmstart_ioport_error;
    uint32_t startup_warmstart_backlight_cfg_error;
    uint32_t startup_warmstart_backlight_write_error;
    uint32_t startup_warmstart_reset_cfg_error;
    uint32_t startup_warmstart_reset_write_error;
    uint32_t startup_warmstart_reset_read_error;
    uint32_t startup_warmstart_reset_level;
    uint32_t startup_event_sequence;
    uint32_t startup_reset_assert_sequence;
    uint32_t startup_reset_release_sequence;
    uint32_t startup_first_dsi_command_sequence;
    uint32_t startup_first_dsi_command;
    uint32_t startup_backlight_enable_sequence;
    uint32_t startup_sequence_valid;
    uint32_t startup_reset_low_hold_ms;
    uint32_t startup_reset_release_wait_ms;
} display_diag_t;

extern volatile display_diag_t g_display_diag;

void display_bringup_run(void);
fsp_err_t display_backlight_startup_step(void);
void display_startup_diag_note_reset_asserted(void);
void display_startup_diag_note_first_dsi_command(uint8_t command);
void display_underflow_context_enter(uint32_t context_mask);
void display_underflow_context_leave(uint32_t context_mask);

#endif
