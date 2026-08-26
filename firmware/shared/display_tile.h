#ifndef RA8P1_DISPLAY_TILE_H
#define RA8P1_DISPLAY_TILE_H

#include <stdint.h>
#include "resource_layout.h"

#define RA8P1_DISPLAY_TILE_MAGIC       (0x454C4954UL) /* TILE */
#define RA8P1_DISPLAY_TILE_VERSION     (7U)
#define RA8P1_DISPLAY_TILE_WIDTH       (192U)
#define RA8P1_DISPLAY_TILE_HEIGHT      (16U)
#define RA8P1_DISPLAY_TILE_MATRIX_BYTES \
    (RA8P1_DISPLAY_TILE_WIDTH * RA8P1_DISPLAY_TILE_HEIGHT)
#define RA8P1_DISPLAY_TILE_ROW_BYTES   (RA8P1_DISPLAY_TILE_WIDTH)
#define RA8P1_DISPLAY_TILE_PAYLOAD_BYTES (RA8P1_DISPLAY_TILE_SLOT_BYTES - 8U)
#define RA8P1_DISPLAY_TILE_CACHE_LINE_BYTES (32U)

/* Each payload carries exactly one completed 192-bin STFT power row. Rows are
 * sequenced independently from completed NPU frames so real partial-window
 * data can reach the waterfall while the rest of the analysis is running.
 * novel_time_start retains the row's position in the logical 16-row window;
 * levels is frequency ordered from the lowest bin to the highest bin. The
 * display-only reducer maps all 955 valid FFT1024 bins in the configured
 * 56 MHz band into 192 independent groups. The NPU input remains the separate
 * fixed 1 x 204 x 115 x 4 INT8 V12 model contract. */
typedef struct st_ra8p1_display_tile_payload
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t window_sequence;
    uint32_t width_height;
    uint32_t flags;
    uint8_t center_index;
    uint8_t tile_index;
    uint8_t tile_count;
    uint8_t novel_time_start;
    uint8_t novel_time_count;
    uint8_t reserved8[3];
    uint8_t levels[RA8P1_DISPLAY_TILE_ROW_BYTES];
    uint8_t reserved[RA8P1_DISPLAY_TILE_PAYLOAD_BYTES - 36U -
                     RA8P1_DISPLAY_TILE_ROW_BYTES];
} ra8p1_display_tile_payload_t;

typedef struct st_ra8p1_display_tile_slot
{
    volatile uint32_t begin_sequence;
    ra8p1_display_tile_payload_t payload;
    volatile uint32_t end_sequence;
} ra8p1_display_tile_slot_t;

#define RA8P1_DISPLAY_TILE_SLOTS \
    ((volatile ra8p1_display_tile_slot_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_DISPLAY_TILE_OFFSET))

typedef char ra8p1_display_tile_payload_size_must_be_slot_minus_header[
    (sizeof(ra8p1_display_tile_payload_t) == (RA8P1_DISPLAY_TILE_SLOT_BYTES - 8U)) ? 1 : -1];
typedef char ra8p1_display_tile_row_must_fit_payload[
    (RA8P1_DISPLAY_TILE_ROW_BYTES <=
     (RA8P1_DISPLAY_TILE_PAYLOAD_BYTES - 36U)) ? 1 : -1];
typedef char ra8p1_display_tile_slot_size_must_match_layout[
    (sizeof(ra8p1_display_tile_slot_t) == RA8P1_DISPLAY_TILE_SLOT_BYTES) ? 1 : -1];
typedef char ra8p1_display_tile_slot_count_must_be_power_of_two[
    ((RA8P1_DISPLAY_TILE_SLOT_COUNT & (RA8P1_DISPLAY_TILE_SLOT_COUNT - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_display_tile_slot_count_must_retain_one_complete_window[
    (RA8P1_DISPLAY_TILE_SLOT_COUNT >= RA8P1_DISPLAY_TILE_HEIGHT) ? 1 : -1];
typedef char ra8p1_display_tile_slot_size_must_be_cache_line_aligned[
    ((RA8P1_DISPLAY_TILE_SLOT_BYTES % RA8P1_DISPLAY_TILE_CACHE_LINE_BYTES) == 0U) ? 1 : -1];
typedef char ra8p1_display_tile_base_must_be_cache_line_aligned[
    ((RA8P1_DISPLAY_TILE_OFFSET % RA8P1_DISPLAY_TILE_CACHE_LINE_BYTES) == 0U) ? 1 : -1];
typedef char ra8p1_display_tile_region_must_match_slots[
    ((RA8P1_DISPLAY_TILE_SLOT_COUNT * RA8P1_DISPLAY_TILE_SLOT_BYTES) ==
     RA8P1_DISPLAY_TILE_BYTES) ? 1 : -1];
typedef char ra8p1_display_tile_after_display_stream[
    (RA8P1_DISPLAY_TILE_OFFSET >=
     (RA8P1_DISPLAY_STREAM_OFFSET + RA8P1_DISPLAY_STREAM_BYTES)) ? 1 : -1];
typedef char ra8p1_display_tile_before_commands[
    ((RA8P1_DISPLAY_TILE_OFFSET + RA8P1_DISPLAY_TILE_BYTES) <=
     RA8P1_IPC_COMMAND_OFFSET) ? 1 : -1];

#endif
