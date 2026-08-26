#ifndef DISPLAY_APP_H
#define DISPLAY_APP_H

#include <stdbool.h>
#include <stdint.h>

/* Monotonic count of continuous-scan recoveries caused by a lack of new
 * semantically valid display frames. It is exported for detached J-Link
 * diagnostics. */
extern volatile uint32_t g_display_app_stall_recoveries;

void display_app_init(void);
void display_app_step(void);
/* Drain derived display tiles into CPU1-owned history. LVGL calls this while
 * waiting for VSync so shared row slots cannot be stranded for a frame. */
void display_app_drain_tiles(void);
/* Bound SDRAM writes while GLCDC is actively scanning a submitted frame. */
void display_app_drain_tiles_bounded(uint32_t max_tiles);
bool display_app_request_capture(uint64_t center_a_hz,
                                 uint64_t center_b_hz,
                                 uint32_t sample_rate_hz,
                                 uint32_t iq_format,
                                 uint32_t channel_mask,
                                 uint32_t fft_interval_samples);
bool display_app_request_capture_with_controls(
    uint64_t center_a_hz,
    uint64_t center_b_hz,
    uint32_t sample_rate_hz,
    uint32_t iq_format,
    uint32_t channel_mask,
    uint32_t fft_interval_samples,
    uint32_t target_payload_mbps_x1000,
    uint32_t test_fault_flags);
/* Queue a live single-center mode transition.  The display command service
 * sends STOP first and waits for the matching STOPPED response before START. */
bool display_app_request_focus(uint32_t center_index);
/* Return to the repeated four-center sweep through the same STOPPED gate. */
bool display_app_request_scan(void);
bool display_app_campaign_command_start(uint32_t center_index,
                                        bool scan_all,
                                        bool continuous_scan,
                                        uint32_t target_payload_mbps_x1000,
                                        uint32_t test_fault_flags);
bool display_app_campaign_command_stop(void);
void display_app_campaign_takeover(void);
uint32_t display_app_last_issued_command_sequence(void);
uint32_t display_app_last_visible_session_id(void);
void display_app_set_alarm_muted(bool muted);
bool display_app_alarm_muted(void);

#endif
