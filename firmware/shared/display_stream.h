#ifndef RA8P1_DISPLAY_STREAM_H
#define RA8P1_DISPLAY_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include "resource_layout.h"
#include "system_protocol.h"

/* The display stream carries derived data only. Raw IQ never enters shared SRAM. */
#define RA8P1_DISPLAY_STREAM_MAGIC          (0x44535046UL) /* DSPF */
#define RA8P1_DISPLAY_STREAM_VERSION        (4U)
#define RA8P1_DISPLAY_PEAK_CHANNEL_COUNT    (2U)
#define RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT (1U)
#define RA8P1_DISPLAY_SPECTRUM_BINS         (256U)
/* Compatibility alias for existing peak/dual-RX code.  Spectrum storage has
 * its own channel count and must not use this alias. */
#define RA8P1_DISPLAY_CHANNEL_COUNT         (RA8P1_DISPLAY_PEAK_CHANNEL_COUNT)
#define RA8P1_DISPLAY_STREAM_SLOT_COUNT     (4U)
#define RA8P1_DISPLAY_STREAM_SLOT_BYTES     (512U)
#define RA8P1_DISPLAY_STREAM_CONTROL_BYTES  (512U)
#define RA8P1_DISPLAY_MASK_WIDTH            (32U)
#define RA8P1_DISPLAY_MASK_HEIGHT           (16U)
#define RA8P1_DISPLAY_MASK_BYTES            ((RA8P1_DISPLAY_MASK_WIDTH * RA8P1_DISPLAY_MASK_HEIGHT) / 8U)
#define RA8P1_DISPLAY_MAX_BOXES             (4U)
#define RA8P1_DISPLAY_RF_COORD_SCALE         (256U)

/* V4 boxes carry physical RF geometry. Frequency coordinates are normalized
 * across the reliable analysis bandwidth; time coordinates are normalized
 * across analysis.window_sample_count. Both ranges are half-open [0, 256).
 * analysis.window_sequence identifies the matching waterfall window. */
#define RA8P1_DISPLAY_BOX_SOURCE_CLASS_MASK  (0x00FFU)
#define RA8P1_DISPLAY_BOX_FLAGS_SHIFT        (8U)
#define RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID  (1U << 0)
#define RA8P1_DISPLAY_BOX_FLAG_TIME_CLIPPED       (1U << 1)
#define RA8P1_DISPLAY_BOX_FLAG_FREQUENCY_CLIPPED  (1U << 2)
#define RA8P1_DISPLAY_BOX_FLAG_VIDEO_20MHZ        (1U << 3)
#define RA8P1_DISPLAY_BOX_FLAG_BANDWIDTH_AMBIGUOUS (1U << 4)
#define RA8P1_DISPLAY_BOX_FLAG_NEEDS_REVIEW       (1U << 5)

#define RA8P1_DISPLAY_FLAG_SYNTHETIC        (1UL << 0)
#define RA8P1_DISPLAY_FLAG_WINDOW_COMPLETE  (1UL << 1)
#define RA8P1_DISPLAY_FLAG_MODEL_MASK_VALID (1UL << 2)
#define RA8P1_DISPLAY_FLAG_DISCONTINUITY    (1UL << 3)
#define RA8P1_DISPLAY_FLAG_REAL_STREAM      (1UL << 4)
#define RA8P1_DISPLAY_FLAG_MODEL_PLACEHOLDER (1UL << 5)
#define RA8P1_DISPLAY_FLAG_PREPROCESS_INVALID (1UL << 6)

#define RA8P1_DISPLAY_TIMING_STFT_VALID     (1UL << 0)
#define RA8P1_DISPLAY_TIMING_NPU_VALID      (1UL << 1)
#define RA8P1_DISPLAY_TIMING_E2E_VALID      (1UL << 2)

typedef struct st_ra8p1_display_stream_control
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    volatile uint32_t session_id;
    uint8_t reserved[RA8P1_DISPLAY_STREAM_CONTROL_BYTES - 12U];
} ra8p1_display_stream_control_t;

typedef struct st_ra8p1_detection_box
{
    uint8_t frequency_start_q8;
    uint8_t time_start_q8;
    uint8_t frequency_span_q8;
    uint8_t time_span_q8;
    uint8_t class_id;
    uint8_t score;
    uint16_t metadata;
} ra8p1_detection_box_t;

typedef struct st_ra8p1_analysis_extension
{
    uint32_t window_sequence;
    uint32_t sample_index_low;
    uint32_t sample_index_high;
    uint32_t window_sample_count;
    uint32_t stft_frame_count;
    uint32_t stft_cycles;
    uint32_t npu_cycles;
    uint32_t end_to_end_cycles;
    uint32_t npu_inference_count;
    uint32_t npu_class;
    int32_t  npu_score_q15;
    uint32_t queue_depth;
    uint32_t ingress_drops;
    uint32_t npu_ready;
    uint32_t mask_width_height;
    uint32_t box_count;
    uint32_t center_frequency_low;
    uint32_t center_frequency_high;
    uint32_t source_sample_rate_hz;
    uint32_t valid_bits;
    uint8_t  mask_bits[RA8P1_DISPLAY_MASK_BYTES];
    ra8p1_detection_box_t boxes[RA8P1_DISPLAY_MAX_BOXES];
    uint32_t timing_flags;
    uint16_t presence_q15[RA8P1_CENTER_COUNT];
    uint8_t  center_index;
    uint8_t  tile_index;
    uint8_t  tile_count;
    uint8_t  reserved8;
    uint32_t model_flags;
} ra8p1_analysis_extension_t;

typedef struct st_ra8p1_display_frame
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t sample_rate_hz;
    uint32_t fft_size;
    uint32_t channel_mask;
    uint32_t flags;
    uint32_t peak_bin[RA8P1_DISPLAY_PEAK_CHANNEL_COUNT];
    uint32_t peak_power_q16[RA8P1_DISPLAY_PEAK_CHANNEL_COUNT];
    uint8_t spectrum[RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT][RA8P1_DISPLAY_SPECTRUM_BINS];
    uint32_t publish_tick;
    ra8p1_analysis_extension_t analysis;
} ra8p1_display_frame_t;

typedef struct st_ra8p1_display_stream_slot
{
    volatile uint32_t begin_sequence;
    ra8p1_display_frame_t payload;
    volatile uint32_t end_sequence;
} ra8p1_display_stream_slot_t;

#define RA8P1_DISPLAY_STREAM_CONTROL \
    ((volatile ra8p1_display_stream_control_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_DISPLAY_STREAM_OFFSET))

#define RA8P1_DISPLAY_STREAM_SLOTS \
    ((volatile ra8p1_display_stream_slot_t *) \
     (RA8P1_SHARED_RAM_BASE + RA8P1_DISPLAY_STREAM_OFFSET + \
      RA8P1_DISPLAY_STREAM_CONTROL_BYTES))

typedef char ra8p1_display_stream_control_size_must_be_512[
    (sizeof(ra8p1_display_stream_control_t) == RA8P1_DISPLAY_STREAM_CONTROL_BYTES) ? 1 : -1];
typedef char ra8p1_detection_box_size_must_be_8[(sizeof(ra8p1_detection_box_t) == 8U) ? 1 : -1];
typedef char ra8p1_analysis_extension_size_must_be_196[(sizeof(ra8p1_analysis_extension_t) == 196U) ? 1 : -1];
typedef char ra8p1_display_peak_channel_count_must_be_2[
    (RA8P1_DISPLAY_PEAK_CHANNEL_COUNT == 2U) ? 1 : -1];
typedef char ra8p1_display_spectrum_channel_count_must_be_1[
    (RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT == 1U) ? 1 : -1];
typedef char ra8p1_display_spectrum_bin_count_must_be_256[
    (RA8P1_DISPLAY_SPECTRUM_BINS == 256U) ? 1 : -1];
typedef char ra8p1_display_frame_magic_offset_must_be_0[
    (offsetof(ra8p1_display_frame_t, magic) == 0U) ? 1 : -1];
typedef char ra8p1_display_frame_version_offset_must_be_4[
    (offsetof(ra8p1_display_frame_t, version) == 4U) ? 1 : -1];
typedef char ra8p1_display_frame_size_offset_must_be_6[
    (offsetof(ra8p1_display_frame_t, size) == 6U) ? 1 : -1];
typedef char ra8p1_display_frame_session_id_offset_must_be_8[
    (offsetof(ra8p1_display_frame_t, session_id) == 8U) ? 1 : -1];
typedef char ra8p1_display_frame_sequence_offset_must_be_12[
    (offsetof(ra8p1_display_frame_t, sequence) == 12U) ? 1 : -1];
typedef char ra8p1_display_frame_sample_rate_hz_offset_must_be_16[
    (offsetof(ra8p1_display_frame_t, sample_rate_hz) == 16U) ? 1 : -1];
typedef char ra8p1_display_frame_fft_size_offset_must_be_20[
    (offsetof(ra8p1_display_frame_t, fft_size) == 20U) ? 1 : -1];
typedef char ra8p1_display_frame_channel_mask_offset_must_be_24[
    (offsetof(ra8p1_display_frame_t, channel_mask) == 24U) ? 1 : -1];
typedef char ra8p1_display_frame_flags_offset_must_be_28[
    (offsetof(ra8p1_display_frame_t, flags) == 28U) ? 1 : -1];
typedef char ra8p1_display_frame_peak_bin_offset_must_be_32[
    (offsetof(ra8p1_display_frame_t, peak_bin) == 32U) ? 1 : -1];
typedef char ra8p1_display_frame_peak_power_offset_must_be_40[
    (offsetof(ra8p1_display_frame_t, peak_power_q16) == 40U) ? 1 : -1];
typedef char ra8p1_display_frame_spectrum_offset_must_be_48[
    (offsetof(ra8p1_display_frame_t, spectrum) == 48U) ? 1 : -1];
typedef char ra8p1_display_frame_publish_tick_offset_must_be_304[
    (offsetof(ra8p1_display_frame_t, publish_tick) == 304U) ? 1 : -1];
typedef char ra8p1_display_frame_analysis_offset_must_be_308[
    (offsetof(ra8p1_display_frame_t, analysis) == 308U) ? 1 : -1];
typedef char ra8p1_display_frame_size_must_be_504[(sizeof(ra8p1_display_frame_t) == 504U) ? 1 : -1];
typedef char ra8p1_display_stream_slot_payload_offset_must_be_4[
    (offsetof(ra8p1_display_stream_slot_t, payload) == 4U) ? 1 : -1];
typedef char ra8p1_display_stream_slot_end_offset_must_be_508[
    (offsetof(ra8p1_display_stream_slot_t, end_sequence) == 508U) ? 1 : -1];
typedef char ra8p1_display_stream_slot_count_must_be_power_of_two[
    ((RA8P1_DISPLAY_STREAM_SLOT_COUNT & (RA8P1_DISPLAY_STREAM_SLOT_COUNT - 1U)) == 0U) ? 1 : -1];
typedef char ra8p1_display_stream_slot_size_must_be_512[
    (sizeof(ra8p1_display_stream_slot_t) == RA8P1_DISPLAY_STREAM_SLOT_BYTES) ? 1 : -1];
typedef char ra8p1_display_stream_slots_must_match_region[
    ((RA8P1_DISPLAY_STREAM_CONTROL_BYTES +
      (RA8P1_DISPLAY_STREAM_SLOT_COUNT * RA8P1_DISPLAY_STREAM_SLOT_BYTES)) ==
     RA8P1_DISPLAY_STREAM_BYTES) ? 1 : -1];
typedef char ra8p1_display_stream_must_precede_command_slot[
    ((RA8P1_DISPLAY_STREAM_OFFSET + RA8P1_DISPLAY_STREAM_BYTES) <=
     RA8P1_IPC_COMMAND_OFFSET) ? 1 : -1];
typedef char ra8p1_display_stream_must_fit_shared_ram[
    ((RA8P1_DISPLAY_STREAM_OFFSET + RA8P1_DISPLAY_STREAM_BYTES) <=
     RA8P1_SHARED_RAM_BYTES) ? 1 : -1];

#endif
