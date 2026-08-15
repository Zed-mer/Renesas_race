#ifndef ANALYSIS_PIPELINE_H
#define ANALYSIS_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include "analysis_contract.h"

#define ANALYSIS_FFT_SIZE             (RA8P1_ANALYSIS_FFT_SIZE)
#define ANALYSIS_HOP_SIZE             (RA8P1_ANALYSIS_HOP_SIZE)
#define ANALYSIS_FREQ_BINS            (RA8P1_ANALYSIS_POOLED_FREQ_BINS)
#define ANALYSIS_TIME_BINS            (RA8P1_ANALYSIS_POOLED_TIME_BINS)
#define ANALYSIS_TIME_POOL            (RA8P1_ANALYSIS_TIME_POOL)
#define ANALYSIS_FREQ_REBIN_NUMERATOR \
    (RA8P1_ANALYSIS_FREQ_REBIN_NUMERATOR)
#define ANALYSIS_FREQ_REBIN_DENOMINATOR \
    (RA8P1_ANALYSIS_FREQ_REBIN_DENOMINATOR)
#define ANALYSIS_STFT_EDGE_CROP_FRAMES \
    (RA8P1_ANALYSIS_STFT_EDGE_CROP_FRAMES)
#define ANALYSIS_DISPLAY_FREQ_BINS    (RA8P1_ANALYSIS_DISPLAY_FREQ_BINS)
#define ANALYSIS_DISPLAY_TIME_BINS    (RA8P1_ANALYSIS_DISPLAY_TIME_BINS)
#define ANALYSIS_DISPLAY_FREQ_POOL    (RA8P1_ANALYSIS_DISPLAY_FREQ_POOL)
#define ANALYSIS_DISPLAY_TIME_POOL    (RA8P1_ANALYSIS_DISPLAY_TIME_POOL)
#define ANALYSIS_STFT_FRAMES_PER_TILE (RA8P1_ANALYSIS_STFT_FRAMES)
#define ANALYSIS_MODEL_WINDOW_SAMPLES (RA8P1_ANALYSIS_TILE_SAMPLES)
#define ANALYSIS_MODEL_STRIDE_SAMPLES (RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES)
#define ANALYSIS_FORMAL_SAMPLE_RATE_HZ (RA8P1_ANALYSIS_SAMPLE_RATE_HZ)
#define ANALYSIS_FORMAL_BANDWIDTH_HZ  (RA8P1_ANALYSIS_BANDWIDTH_HZ)
#define ANALYSIS_DEFAULT_SAMPLE_RATE  (ANALYSIS_FORMAL_SAMPLE_RATE_HZ)
#define ANALYSIS_DEFAULT_WINDOW_MS    (10U)

/* The boot proof is intentionally a one-shot synthetic compute benchmark.  It
 * runs before the IQ consumer thread starts, then the caller restores the
 * normal real-stream configuration.  Production builds can disable the boot
 * delay with -DRA8P1_STFT_BOOT_PROOF_ENABLE=0 while retaining the proof ABI. */
#ifndef RA8P1_STFT_BOOT_PROOF_ENABLE
#define RA8P1_STFT_BOOT_PROOF_ENABLE  (0)
#endif

#define ANALYSIS_STFT_PROOF_MAGIC              (0x53544650U) /* STFP */
#define ANALYSIS_STFT_PROOF_VERSION            (1U)
#define ANALYSIS_STFT_PROOF_DONE_MAGIC         (0x5354444EU) /* STDN */
#define ANALYSIS_STFT_PROOF_RUNS               (5U)
#define ANALYSIS_STFT_PROOF_STATUS_IDLE        (0U)
#define ANALYSIS_STFT_PROOF_STATUS_RUNNING     (1U)
#define ANALYSIS_STFT_PROOF_STATUS_PASS        (2U)
#define ANALYSIS_STFT_PROOF_STATUS_DWT_ERROR   (0xE001U)
#define ANALYSIS_STFT_PROOF_STATUS_INIT_ERROR  (0xE002U)
#define ANALYSIS_STFT_PROOF_STATUS_RUN_ERROR   (0xE003U)
#define ANALYSIS_STFT_PROOF_STATUS_CHECK_ERROR (0xE004U)
#define ANALYSIS_STFT_PROOF_STATUS_FAULT       (0xE005U)

#define ANALYSIS_STFT_PROOF_FLAG_SYNTHETIC             (1UL << 0U)
#define ANALYSIS_STFT_PROOF_FLAG_INPUT_GEN_EXCLUDED     (1UL << 1U)
#define ANALYSIS_STFT_PROOF_FLAG_NETWORK_EXCLUDED       (1UL << 2U)
#define ANALYSIS_STFT_PROOF_FLAG_CRC_EXCLUDED           (1UL << 3U)
#define ANALYSIS_STFT_PROOF_FLAG_NPU_EXCLUDED           (1UL << 4U)
#define ANALYSIS_STFT_PROOF_FLAG_IPC_EXCLUDED           (1UL << 5U)
#define ANALYSIS_STFT_PROOF_FLAG_PRODUCTION_STFT_PATH   (1UL << 6U)
#define ANALYSIS_STFT_PROOF_FLAG_BOOT_ONCE              (1UL << 7U)
#define ANALYSIS_STFT_PROOF_FLAG_S16_CONVERT_INCLUDED   (1UL << 8U)
#define ANALYSIS_STFT_PROOF_FLAG_STAGE_PROBES_INCLUDED  (1UL << 9U)

typedef struct st_analysis_stft_proof_series
{
    uint32_t samples[ANALYSIS_STFT_PROOF_RUNS];
    uint32_t minimum_cycles;
    uint32_t median_cycles;
    uint32_t maximum_cycles;
} analysis_stft_proof_series_t;

typedef struct st_analysis_stft_proof
{
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t completion_magic;
    uint32_t flags;
    uint32_t core_clock_hz;
    uint32_t warmup_runs;
    uint32_t measured_runs;
    uint32_t complex_samples;
    uint32_t input_format;
    uint32_t valid_bits;
    uint32_t fft_size;
    uint32_t hop_size;
    uint32_t stft_frames;
    uint32_t frequency_pool;
    uint32_t time_pool;
    uint32_t input_block_samples;
    uint32_t checksum;
    uint32_t peak_bin;
    uint32_t peak_power;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t checksum_mismatches;
    uint32_t frame_mismatches;
    analysis_stft_proof_series_t full_window;
    analysis_stft_proof_series_t stft_hot;
    analysis_stft_proof_series_t window_apply;
    analysis_stft_proof_series_t fft;
    analysis_stft_proof_series_t power_reduce;
    analysis_stft_proof_series_t pool_and_quantize;
    analysis_stft_proof_series_t ingest_and_schedule;
} analysis_stft_proof_t;

extern volatile analysis_stft_proof_t g_analysis_stft_proof;

typedef struct st_analysis_stats
{
    uint32_t window_sequence;
    uint32_t windows_completed;
    uint32_t stft_frames;
    uint32_t stft_cycles;
    uint32_t npu_cycles;
    uint32_t end_to_end_cycles;
    uint32_t peak_bin;
    uint32_t peak_power_q16;
    uint32_t sample_rate_hz;
    uint32_t source_sample_rate_hz;
    uint32_t window_samples;
    uint32_t queue_depth;
    uint32_t ingress_drops;
    uint32_t npu_ready;
    uint32_t synthetic;
    uint32_t partial_windows_dropped;
    uint32_t discontinuities;
    uint32_t tile_count;
    uint32_t center_index;
    uint32_t preprocessing_valid;
    uint32_t log_fallbacks;
    uint32_t log_values;
} analysis_stats_t;

void analysis_pipeline_init(void);
void analysis_pipeline_configure(uint32_t source_sample_rate_hz,
                                 uint32_t sample_rate_hz,
                                 uint64_t center_frequency_hz,
                                 uint32_t bandwidth_hz,
                                 uint32_t window_samples,
                                 uint32_t valid_bits,
                                 uint32_t flags);
void analysis_pipeline_set_session(uint32_t session_id);
void analysis_pipeline_set_stream_info(uint64_t total_samples, uint32_t center_index);
void analysis_pipeline_set_queue(uint32_t queue_depth, uint32_t ingress_drops);
void analysis_pipeline_ingest_s16(const int16_t *iq,
                                  uint32_t complex_samples,
                                  uint64_t sample_index,
                                  uint32_t flags);
/* Consume an already converted interleaved Q15 block.  The RF worker uses
 * this entry point when CRC and S12 conversion were fused into one payload
 * traversal. */
void analysis_pipeline_ingest_q15(const int16_t *iq,
                                  uint32_t complex_samples,
                                  uint64_t sample_index,
                                  uint32_t flags);
void analysis_pipeline_ingest_s8(const int8_t *iq,
                                 uint32_t complex_samples,
                                 uint64_t sample_index,
                                 uint32_t flags);
void analysis_pipeline_synthetic_step(uint32_t complex_samples);
int analysis_pipeline_run_stft_proof(void);
/* A one-tile real capture remains frozen until the IQSC whole-window quality
 * evidence is available. Only a committed capture may train a background or
 * enter the V2/V3 NPU pair. */
bool analysis_pipeline_window_ready(void);
bool analysis_pipeline_commit_stream(void);
void analysis_pipeline_reject_stream(uint8_t tile_flags);
void analysis_pipeline_finish_stream(void);
/* Drop an incomplete real-input window at an explicit stream boundary.  A
 * partial capture must never be completed with synthetic samples. */
void analysis_pipeline_abort_stream(void);
void analysis_pipeline_copy_display_tile(uint8_t *destination, uint32_t destination_bytes);
void analysis_pipeline_get_stats(analysis_stats_t *stats);

#endif
