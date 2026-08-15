#include "analysis_pipeline.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rtthread.h>
#include "arm_math.h"
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0)
#include <arm_mve.h>
#endif
#if defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)
#include "arm_vec_math.h"
#define ANALYSIS_HAS_MVEF (1)
#else
#define ANALYSIS_HAS_MVEF (0)
#endif
#include "hal_data.h"

#include "display_stream.h"
#include "display_tile.h"
#include "ipc_bridge.h"
#include "iq_protocol.h"
#include "iq_ring.h"
#include "npu_runner.h"
#include "cpu0_trace.h"
#include "rf_v12_detector.h"
#include "rf_v12_preprocess.h"
#include "rf_v13_round_builder.h"

#define ANALYSIS_POWER_LOG_EPSILON         (-39.863136F) /* log2(1e-12) */
#define ANALYSIS_Q15_FFT_SHIFT             (1U)
#define ANALYSIS_Q15_POWER_EXPONENT        (26)
#define ANALYSIS_Q15_POWER_SCALE           (1.490116119384765625e-8F) /* 2^-26 */
#define ANALYSIS_LOG2_INV_LN2              (1.44269504088896340736F)
#define ANALYSIS_LOG_FALLBACK_MARGIN      (6.0e-5F)
#define ANALYSIS_MODEL_INPUT_BYTES        (NPU_RUNNER_INPUT_BYTES)
#define ANALYSIS_MODEL_TILE_COUNT_MAX     (255U)
#define ANALYSIS_DISPLAY_LOG2_FLOOR        (-32.0F)
#define ANALYSIS_DISPLAY_LOG2_CEILING      (-2.0F)
#define ANALYSIS_TWO_PI                   (6.28318530717958647692F)
#define ANALYSIS_STFT_PROOF_BLOCK_SAMPLES (256U)
#define ANALYSIS_INGEST_S16_SCALARS       \
    (IQ_RING_PAYLOAD_BYTES / sizeof(int16_t))
#define ANALYSIS_DISPLAY_TIME_BINS_PER_ROW \
    (ANALYSIS_DISPLAY_TIME_BINS / RA8P1_DISPLAY_TILE_HEIGHT)
#define ANALYSIS_DISPLAY_FRAMES_PER_ROW \
    (ANALYSIS_DISPLAY_TIME_BINS_PER_ROW * ANALYSIS_DISPLAY_TIME_POOL)
#define ANALYSIS_SPECTRUM_AVERAGE_FRAMES  (ANALYSIS_TIME_POOL)
#define ANALYSIS_DISPLAY_ROWS_PER_PUBLISH (1U)
#define ANALYSIS_DISPLAY_OVERLAP_ROWS     (RA8P1_DISPLAY_TILE_HEIGHT / 2U)
#define ANALYSIS_DTCM                     __attribute__((section(".dtcm"), aligned(32)))
#define ANALYSIS_RAM                      __attribute__((section(".sdram_noinit"), aligned(32), used))
#define ANALYSIS_HOT_CODE                 __attribute__((section(".itcm_code_from_flash"), noinline, optimize("Ofast")))

typedef struct st_analysis_lane_hot
{
    q15_t frame_iq[ANALYSIS_FFT_SIZE * 2U];
    uint64_t display_power_sum[RA8P1_DISPLAY_TILE_WIDTH];
    uint64_t spectrum_power_sum[RA8P1_DISPLAY_SPECTRUM_BINS];
} analysis_lane_hot_t;

typedef struct st_analysis_lane
{
    uint32_t active;
    uint32_t complete_pending;
    uint32_t tile_index;
    uint64_t start_sample;
    uint32_t sample_count;
    uint32_t frame_fill;
    uint32_t frame_head;
    uint32_t stft_frames;
    uint32_t display_frame_count;
    uint32_t display_row_count;
    uint32_t display_rows_published;
    uint32_t peak_bin;
    uint32_t peak_power;
    uint32_t stft_cycles;
    uint32_t start_cycles;
    uint32_t complete_cycles;
    uint32_t dwt_epoch;
    uint32_t timing_valid;
    uint32_t discontinuity;
    analysis_lane_hot_t *hot;
    int8_t model_input[ANALYSIS_MODEL_INPUT_BYTES];
    rf_v12_preprocess_tile_t preprocess;
    /* Keep the real STFT visualization independent from the trained tensor. */
    uint8_t display_tile[RA8P1_DISPLAY_TILE_MATRIX_BYTES];
    uint8_t last_spectrum[RA8P1_DISPLAY_SPECTRUM_BINS];
} analysis_lane_t;

typedef struct st_analysis_state
{
    uint32_t source_sample_rate_hz;
    uint32_t sample_rate_hz;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    uint32_t window_samples;
    uint32_t stride_samples;
    uint32_t valid_bits;
    uint32_t stream_flags;
    uint32_t session_id;
    uint32_t center_index;
    uint32_t result_round_index;
    uint64_t stream_origin_sample;
    uint64_t next_sample_index;
    uint64_t total_samples;
    uint32_t sample_index_valid;
    uint32_t expected_tile_count;
    uint32_t next_tile_index;
    uint32_t windows_completed;
    uint32_t stft_frames_total;
    uint32_t stft_cycles_last;
    uint32_t npu_cycles_last;
    uint32_t end_to_end_cycles_last;
    uint32_t partial_windows_dropped;
    uint32_t discontinuities;
    uint32_t queue_depth;
    uint32_t ingress_drops;
    uint32_t npu_ready;
    uint32_t synthetic;
    uint32_t configured;
    uint32_t preprocessing_valid;
    uint32_t started;
    uint32_t discontinuity_pending;
    uint32_t dwt_epoch;
    uint32_t log_fallbacks;
    uint32_t log_values;
    uint32_t capture_result_submitted;
    uint64_t capture_start_time_us;
    uint64_t capture_end_time_us;
} analysis_state_t;

typedef struct st_analysis_stage_probe
{
    uint32_t active;
    uint32_t completed;
    uint32_t complete_cycles;
    uint32_t checksum;
    uint32_t stft_frames;
    uint32_t stft_hot_cycles;
    uint32_t window_cycles;
    uint32_t fft_cycles;
    uint32_t reduce_cycles;
    uint32_t pool_cycles;
    uint32_t peak_bin;
    uint32_t peak_power;
} analysis_stage_probe_t;

static analysis_state_t g_analysis;
static analysis_lane_t g_lanes[2] ANALYSIS_RAM;
static analysis_lane_hot_t g_lane_hot[2] ANALYSIS_DTCM;
static arm_cfft_instance_q15 g_cfft;
static bool g_cfft_ready;
static q15_t g_fft_iq[ANALYSIS_FFT_SIZE * 2U] ANALYSIS_DTCM;
static uint32_t g_fft_power[ANALYSIS_FFT_SIZE] ANALYSIS_DTCM;
static q15_t g_window[ANALYSIS_FFT_SIZE] ANALYSIS_DTCM;
/* The output-oriented exact-area map is immutable after initialization. */
static rf_v12_rebin_output_map_t
    g_v12_output_map[RF_V12_FEATURE_FREQUENCY_BINS] ANALYSIS_DTCM;
static uint8_t g_display_raw_bin_map[ANALYSIS_FFT_SIZE];
static uint32_t g_display_power_divisor[RA8P1_DISPLAY_TILE_WIDTH];
static uint16_t g_spectrum_raw_bin_map[ANALYSIS_FFT_SIZE];
static uint32_t g_spectrum_power_divisor[RA8P1_DISPLAY_SPECTRUM_BINS];
/* This final copy is not used by the FFT hot path. Keep scarce DTCM for the
 * lane accumulators and FFT working sets. */
static uint8_t g_display_tile[RA8P1_DISPLAY_TILE_MATRIX_BYTES]
    __attribute__((aligned(32)));
static uint32_t g_last_peak_bin;
static uint32_t g_last_peak_power;
static uint32_t g_last_tile_index;
static uint32_t g_v12_tile_sequence;
static uint32_t g_v13_round_index;
static uint32_t g_v13_round_center_mask;
static uint32_t g_v13_round_last_center;
static uint64_t g_last_capture_end_time_us;
static analysis_stage_probe_t g_stft_probe;
static int16_t g_stft_proof_input[ANALYSIS_STFT_PROOF_BLOCK_SAMPLES * 2U]
    __attribute__((aligned(32)));

volatile analysis_stft_proof_t g_analysis_stft_proof
    __attribute__((used, aligned(32)));

typedef char analysis_stft_proof_abi_size_must_be_320[
    (sizeof(analysis_stft_proof_t) == 320U) ? 1 : -1];
typedef char analysis_model_input_size_must_match_v12[
    (ANALYSIS_MODEL_INPUT_BYTES == RF_V12_FEATURE_BYTES) ? 1 : -1];
typedef char analysis_model_frequency_size_must_match_v12[
    (ANALYSIS_FREQ_BINS == RF_V12_FEATURE_FREQUENCY_BINS) ? 1 : -1];
typedef char analysis_model_time_size_must_match_v12[
    (ANALYSIS_TIME_BINS == RF_V12_FEATURE_TIME_BINS) ? 1 : -1];
typedef char analysis_display_time_bins_must_map_evenly_to_rows[
    ((ANALYSIS_DISPLAY_TIME_BINS % RA8P1_DISPLAY_TILE_HEIGHT) == 0U) ? 1 : -1];
typedef char analysis_display_frames_must_cover_stft_tile[
    ((ANALYSIS_DISPLAY_FRAMES_PER_ROW * RA8P1_DISPLAY_TILE_HEIGHT) ==
     ANALYSIS_STFT_FRAMES_PER_TILE) ? 1 : -1];
typedef char analysis_display_frequency_bins_must_cover_tile[
    (ANALYSIS_FFT_SIZE >= RA8P1_DISPLAY_TILE_WIDTH) ? 1 : -1];
typedef char analysis_display_frequency_map_must_reserve_invalid_sentinel[
    (RA8P1_DISPLAY_TILE_WIDTH <= UINT8_MAX) ? 1 : -1];
typedef char analysis_spectrum_frequency_map_must_reserve_invalid_sentinel[
    (RA8P1_DISPLAY_SPECTRUM_BINS < UINT16_MAX) ? 1 : -1];
typedef char analysis_spectrum_frequency_bins_must_cover_spectrum[
    (ANALYSIS_FFT_SIZE >= RA8P1_DISPLAY_SPECTRUM_BINS) ? 1 : -1];
typedef char analysis_spectrum_bins_must_support_mve_groups[
    ((RA8P1_DISPLAY_SPECTRUM_BINS % 4U) == 0U) ? 1 : -1];
typedef char analysis_display_publish_rows_must_cover_tile[
    ((RA8P1_DISPLAY_TILE_HEIGHT % ANALYSIS_DISPLAY_ROWS_PER_PUBLISH) == 0U) ? 1 : -1];
/* A production IQ slot is far shorter than one display-row interval, so one
 * ingest can publish at most one row from each of the two active lanes.  Ring
 * capacity must cover that actual burst; total rows in a window are streamed
 * over time and do not need to coexist in shared RAM. */
typedef char analysis_iq_slot_cannot_cross_two_display_row_boundaries[
    ((IQ_RING_PAYLOAD_BYTES / (2U * sizeof(int16_t))) <
     (ANALYSIS_DISPLAY_FRAMES_PER_ROW * ANALYSIS_HOP_SIZE)) ? 1 : -1];
typedef char analysis_display_slots_must_retain_two_lane_publish_burst[
    (RA8P1_DISPLAY_TILE_SLOT_COUNT >= 2U) ? 1 : -1];
typedef char analysis_ingest_s16_block_must_cover_one_ring_payload[
    ((ANALYSIS_INGEST_S16_SCALARS * sizeof(int16_t)) ==
     IQ_RING_PAYLOAD_BYTES) ? 1 : -1];
typedef char analysis_ingest_s16_block_must_hold_whole_complex_samples[
    ((ANALYSIS_INGEST_S16_SCALARS & 1U) == 0U) ? 1 : -1];

static bool analysis_dwt_enabled(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

static bool analysis_enable_dwt(void)
{
    if (analysis_dwt_enabled())
    {
        return true;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0UL) = 0xC5ACCE55UL;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    if (!analysis_dwt_enabled())
    {
        return false;
    }
    g_analysis.dwt_epoch++;
    return true;
}

static uint32_t analysis_cycle_now(void)
{
    uint32_t value;
    if (!analysis_enable_dwt())
    {
        return 0U;
    }
    __DSB();
    value = DWT->CYCCNT;
    __ISB();
    return value;
}

static uint32_t analysis_checksum_bytes(const uint8_t *data, uint32_t bytes)
{
    uint32_t checksum = 2166136261U;
    if (data == NULL)
    {
        return 0U;
    }
    for (uint32_t i = 0U; i < bytes; ++i)
    {
        checksum = (checksum ^ data[i]) * 16777619U;
    }
    return checksum;
}

static inline uint32_t analysis_cycle_now_fast(void)
{
    /* DWT is enabled once when the lane is opened.  Volatile CYCCNT reads are
     * sufficient for relative frame timing; per-frame DSB/ISB would become
     * part of all 1152 measurements without ordering any shared ownership. */
    __asm volatile ("" ::: "memory");
    const uint32_t value = DWT->CYCCNT;
    __asm volatile ("" ::: "memory");
    return value;
}

static int16_t analysis_to_q15(int32_t value)
{
    int32_t shift = (g_analysis.valid_bits < 16U) ?
                    (int32_t)(16U - g_analysis.valid_bits) : 0;
    if (shift > 0)
    {
        value <<= shift;
    }
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

static ANALYSIS_HOT_CODE void analysis_convert_s12_q15(const int16_t *source,
                                                        q15_t *destination,
                                                        uint32_t scalar_count)
{
    uint32_t converted = 0U;
    if ((source == NULL) || (destination == NULL) || (scalar_count == 0U))
    {
        return;
    }
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0)
    while ((scalar_count - converted) >= 8U)
    {
        const q15x8_t samples = vld1q(&source[converted]);
        /* Saturating left shift by four is bit-identical to the scalar int32
         * shift followed by INT16_MIN/INT16_MAX clamping. */
        vst1q(&destination[converted], vqshlq_n_s16(samples, 4));
        converted += 8U;
    }
#endif
    while (converted < scalar_count)
    {
        int32_t value = (int32_t)source[converted] << 4U;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        destination[converted] = (q15_t)value;
        converted++;
    }
}

static float analysis_log_power(uint64_t integer_power, uint32_t divisor)
{
    float power;
    if ((integer_power == 0U) || (divisor == 0U))
    {
        return ANALYSIS_POWER_LOG_EPSILON;
    }
    power = (float)integer_power / (float)divisor;
    /* The Q15 CFFT is forward-scaled by 1/N and the implementation shifts
     * each component once before squaring: P_ref = P_integer / 2^26. */
    power = ldexpf(power, -ANALYSIS_Q15_POWER_EXPONENT);
    return log2f(power + 1.0e-12F);
}

static bool analysis_fft_bin_valid(uint32_t shifted_bin)
{
    uint32_t distance;
    uint64_t bandwidth = (g_analysis.bandwidth_hz == 0U) ?
                        ANALYSIS_FORMAL_BANDWIDTH_HZ : g_analysis.bandwidth_hz;
    if (shifted_bin >= (ANALYSIS_FFT_SIZE / 2U))
        distance = shifted_bin - (ANALYSIS_FFT_SIZE / 2U);
    else
        distance = (ANALYSIS_FFT_SIZE / 2U) - shifted_bin;
    /* |k-F/2| * Fs/1024 <= bandwidth/2, without floating point. */
    return ((uint64_t)distance * g_analysis.sample_rate_hz) <=
           (bandwidth * (ANALYSIS_FFT_SIZE / 2U));
}

static void analysis_rebuild_fft_valid_mask(void)
{
    uint32_t valid_raw_bin_count = 0U;
    uint32_t valid_raw_bin_index = 0U;

    memset(g_display_raw_bin_map, UINT8_MAX, sizeof(g_display_raw_bin_map));
    memset(g_display_power_divisor, 0, sizeof(g_display_power_divisor));
    memset(g_spectrum_raw_bin_map, UINT8_MAX, sizeof(g_spectrum_raw_bin_map));
    memset(g_spectrum_power_divisor, 0, sizeof(g_spectrum_power_divisor));
    for (uint32_t shifted_bin = 0U;
         shifted_bin < ANALYSIS_FFT_SIZE;
         ++shifted_bin)
    {
        if (analysis_fft_bin_valid(shifted_bin))
        {
            valid_raw_bin_count++;
        }
    }

    /* The UI reducer is independent from the V12 exact-area model input. It
     * uses every reliable raw FFT bin and its own 128x128/pool8x9 timing
     * contract to feed the compact 192x16 waterfall and 256-bin spectrum. */
    if (valid_raw_bin_count != 0U)
    {
        for (uint32_t shifted_bin = 0U;
             shifted_bin < ANALYSIS_FFT_SIZE;
             ++shifted_bin)
        {
            uint32_t display_bin;
            uint32_t spectrum_bin;
            if (!analysis_fft_bin_valid(shifted_bin))
            {
                continue;
            }
            display_bin = (valid_raw_bin_index * RA8P1_DISPLAY_TILE_WIDTH) /
                          valid_raw_bin_count;
            if (display_bin >= RA8P1_DISPLAY_TILE_WIDTH)
            {
                display_bin = RA8P1_DISPLAY_TILE_WIDTH - 1U;
            }
            g_display_raw_bin_map[shifted_bin] = (uint8_t)display_bin;
            g_display_power_divisor[display_bin] +=
                ANALYSIS_DISPLAY_FRAMES_PER_ROW;

            spectrum_bin =
                (valid_raw_bin_index * RA8P1_DISPLAY_SPECTRUM_BINS) /
                valid_raw_bin_count;
            if (spectrum_bin >= RA8P1_DISPLAY_SPECTRUM_BINS)
            {
                spectrum_bin = RA8P1_DISPLAY_SPECTRUM_BINS - 1U;
            }
            g_spectrum_raw_bin_map[shifted_bin] = (uint16_t)spectrum_bin;
            g_spectrum_power_divisor[spectrum_bin] +=
                ANALYSIS_SPECTRUM_AVERAGE_FRAMES;
            valid_raw_bin_index++;
        }
    }
}

/* The UI has a separate display transfer function.  It is intentionally not
 * the NPU quantizer: model calibration is a model contract, while the display
 * must show real RF power even before trained metadata is available.  The
 * selected range is -32..-2 log2 power, approximately -96..-6 dBFS. */
static inline uint8_t analysis_display_level(float log2_power)
{
    const float span = ANALYSIS_DISPLAY_LOG2_CEILING - ANALYSIS_DISPLAY_LOG2_FLOOR;

    if (!(log2_power > ANALYSIS_DISPLAY_LOG2_FLOOR))
    {
        return 0U;
    }
    if (log2_power >= ANALYSIS_DISPLAY_LOG2_CEILING)
    {
        return UINT8_MAX;
    }
    return (uint8_t)(((log2_power - ANALYSIS_DISPLAY_LOG2_FLOOR) * 255.0F /
                      span) + 0.5F);
}

static inline uint8_t analysis_display_level_guarded(uint64_t integer_power,
                                                     uint32_t divisor,
                                                     float log2_power)
{
    const float span = ANALYSIS_DISPLAY_LOG2_CEILING - ANALYSIS_DISPLAY_LOG2_FLOOR;
    const float level_scale = 255.0F / span;

    if ((integer_power == 0U) || (divisor == 0U))
    {
        return 0U;
    }

    /* CMSIS vlog is comfortably inside one display level, but values within
     * its error margin of a rounding or clipping boundary could otherwise
     * differ by one between MVE and scalar builds.  The spectrum runs this
     * guard only once per completed window, so exact boundary fallbacks do
     * not enter the per-FFT hot path. */
    if ((fabsf(log2_power - ANALYSIS_DISPLAY_LOG2_FLOOR) <=
         ANALYSIS_LOG_FALLBACK_MARGIN) ||
        (fabsf(log2_power - ANALYSIS_DISPLAY_LOG2_CEILING) <=
         ANALYSIS_LOG_FALLBACK_MARGIN))
    {
        return analysis_display_level(analysis_log_power(integer_power, divisor));
    }
    if ((log2_power > ANALYSIS_DISPLAY_LOG2_FLOOR) &&
        (log2_power < ANALYSIS_DISPLAY_LOG2_CEILING))
    {
        const float scaled =
            (log2_power - ANALYSIS_DISPLAY_LOG2_FLOOR) * level_scale;
        const float rounded = (float)((uint32_t)(scaled + 0.5F));
        const float boundary_margin = ANALYSIS_LOG_FALLBACK_MARGIN * level_scale;
        if ((fabsf(scaled - (rounded - 0.5F)) <= boundary_margin) ||
            (fabsf(scaled - (rounded + 0.5F)) <= boundary_margin))
        {
            return analysis_display_level(analysis_log_power(integer_power, divisor));
        }
    }
    return analysis_display_level(log2_power);
}

#if ANALYSIS_HAS_MVEF
static ANALYSIS_HOT_CODE void analysis_log_power_mve4(const uint64_t *integer_power,
                                                      const uint32_t *divisor,
                                                      float *destination)
{
    float input[4] __attribute__((aligned(16)));
    uint32_t zeros = 0U;
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if ((integer_power[i] == 0U) || (divisor[i] == 0U))
        {
            input[i] = 1.0e-12F;
            zeros |= 1UL << i;
        }
        else
        {
            /* Multiplication by 2^-26 is exact for the normal values used by
             * the power accumulator and avoids one scalar ldexpf call. */
            input[i] = ((float)integer_power[i] / (float)divisor[i]) *
                       ANALYSIS_Q15_POWER_SCALE + 1.0e-12F;
        }
    }
    {
        const f32x4_t values = vld1q(input);
        f32x4_t logs = vlogq_f32(values);
        logs = vmulq_n_f32(logs, ANALYSIS_LOG2_INV_LN2);
        vst1q(destination, logs);
    }
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if ((zeros & (1UL << i)) != 0U)
            destination[i] = ANALYSIS_POWER_LOG_EPSILON;
    }
}

static ANALYSIS_HOT_CODE void analysis_log_power_max_mve4(const uint32_t *integer_power,
                                                          float *destination)
{
    uint32_t zeros = 0U;
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if (integer_power[i] == 0U)
        {
            zeros |= 1UL << i;
        }
    }
    {
        const uint32x4_t powers = vld1q_u32(integer_power);
        f32x4_t values = vcvtq_f32_u32(powers);
        values = vfmaq_n_f32(vdupq_n_f32(1.0e-12F),
                             values,
                             ANALYSIS_Q15_POWER_SCALE);
        f32x4_t logs = vlogq_f32(values);
        logs = vmulq_n_f32(logs, ANALYSIS_LOG2_INV_LN2);
        vst1q(destination, logs);
    }
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if ((zeros & (1UL << i)) != 0U)
            destination[i] = ANALYSIS_POWER_LOG_EPSILON;
    }
}
#endif

static ANALYSIS_HOT_CODE void analysis_store_display_spectrum(analysis_lane_t *lane)
{
    analysis_lane_hot_t *hot;
    if ((lane == NULL) || (lane->hot == NULL))
    {
        return;
    }
    hot = lane->hot;
#if ANALYSIS_HAS_MVEF
    {
        uint64_t sums[4] __attribute__((aligned(16)));
        uint32_t divisors[4] __attribute__((aligned(16)));
        float log2_power[4] __attribute__((aligned(16)));

        for (uint32_t base = 0U;
             base < RA8P1_DISPLAY_SPECTRUM_BINS;
             base += 4U)
        {
            for (uint32_t index = 0U; index < 4U; ++index)
            {
                sums[index] = hot->spectrum_power_sum[base + index];
                divisors[index] = g_spectrum_power_divisor[base + index];
            }
            analysis_log_power_mve4(sums, divisors, log2_power);
            for (uint32_t index = 0U; index < 4U; ++index)
            {
                lane->last_spectrum[base + index] =
                    analysis_display_level_guarded(sums[index],
                                                   divisors[index],
                                                   log2_power[index]);
            }
        }
    }
#else
    for (uint32_t spectrum_bin = 0U;
         spectrum_bin < RA8P1_DISPLAY_SPECTRUM_BINS;
         ++spectrum_bin)
    {
        lane->last_spectrum[spectrum_bin] =
            analysis_display_level(analysis_log_power(
                hot->spectrum_power_sum[spectrum_bin],
                g_spectrum_power_divisor[spectrum_bin]));
    }
#endif
    g_analysis.log_values += RA8P1_DISPLAY_SPECTRUM_BINS;
    memset(hot->spectrum_power_sum, 0, sizeof(hot->spectrum_power_sum));
}

static ANALYSIS_HOT_CODE void analysis_store_display_row(analysis_lane_t *lane,
                                                         uint32_t display_y)
{
    analysis_lane_hot_t *hot;
    if ((lane == NULL) || (lane->hot == NULL) ||
        (display_y >= RA8P1_DISPLAY_TILE_HEIGHT))
    {
        return;
    }
    hot = lane->hot;
#if ANALYSIS_HAS_MVEF
    {
        uint64_t sums[4] __attribute__((aligned(16)));
        uint32_t divisors[4] __attribute__((aligned(16)));
        float log2_power[4] __attribute__((aligned(16)));

        for (uint32_t base = 0U;
             base < RA8P1_DISPLAY_TILE_WIDTH;
             base += 4U)
        {
            for (uint32_t index = 0U; index < 4U; ++index)
            {
                sums[index] = hot->display_power_sum[base + index];
                divisors[index] = g_display_power_divisor[base + index];
            }
            analysis_log_power_mve4(sums, divisors, log2_power);
            for (uint32_t index = 0U; index < 4U; ++index)
            {
                lane->display_tile[
                    (display_y * RA8P1_DISPLAY_TILE_WIDTH) + base + index] =
                    analysis_display_level(log2_power[index]);
            }
        }
    }
#else
    for (uint32_t display_bin = 0U;
         display_bin < RA8P1_DISPLAY_TILE_WIDTH;
         ++display_bin)
    {
        lane->display_tile[(display_y * RA8P1_DISPLAY_TILE_WIDTH) + display_bin] =
            analysis_display_level(analysis_log_power(
                hot->display_power_sum[display_bin],
                g_display_power_divisor[display_bin]));
    }
#endif
    g_analysis.log_values += RA8P1_DISPLAY_TILE_WIDTH;
    memset(hot->display_power_sum, 0, sizeof(hot->display_power_sum));
}

static void analysis_lane_reset(analysis_lane_t *lane)
{
    analysis_lane_hot_t *hot;
    if (lane == NULL) return;
    hot = (lane == &g_lanes[1]) ? &g_lane_hot[1] : &g_lane_hot[0];
    memset(lane, 0, sizeof(*lane));
    memset(hot, 0, sizeof(*hot));
    lane->hot = hot;
    rf_v12_preprocess_tile_reset(&lane->preprocess, lane->model_input);
}

static void analysis_reset_schedule(uint64_t origin, uint32_t discontinuity)
{
    g_analysis.stream_origin_sample = origin;
    g_analysis.next_sample_index = origin;
    g_analysis.sample_index_valid = 1U;
    g_analysis.next_tile_index = 0U;
    g_analysis.started = 1U;
    for (uint32_t i = 0U; i < 2U; ++i)
    {
        analysis_lane_reset(&g_lanes[i]);
        g_lanes[i].discontinuity = discontinuity;
    }
}

static void analysis_resync_after_gap(uint64_t gap_sample)
{
    uint64_t relative = (gap_sample >= g_analysis.stream_origin_sample) ?
                        (gap_sample - g_analysis.stream_origin_sample) : 0U;
    uint64_t stride = g_analysis.stride_samples;
    uint64_t next = (relative + stride - 1U) / stride;
    g_analysis.next_tile_index = (next > UINT32_MAX) ? UINT32_MAX : (uint32_t)next;
    /* next_sample_index is the ingress frontier, not the next tile boundary.
     * analysis_feed_q15 updates it after consuming this contiguous block. */
    g_analysis.started = 1U;
    g_analysis.discontinuity_pending = 1U;
    for (uint32_t i = 0U; i < 2U; ++i) analysis_lane_reset(&g_lanes[i]);
}

static uint32_t analysis_compute_tile_count(uint64_t total_samples)
{
    if (total_samples < ANALYSIS_MODEL_WINDOW_SAMPLES)
    {
        return 0U;
    }
    return (uint32_t)(1U + ((total_samples - ANALYSIS_MODEL_WINDOW_SAMPLES) /
                            ANALYSIS_MODEL_STRIDE_SAMPLES));
}

static analysis_lane_t *analysis_find_lane(uint32_t tile_index)
{
    for (uint32_t i = 0U; i < 2U; ++i)
    {
        if (g_lanes[i].active == 0U)
        {
            const uint32_t discontinuity = g_analysis.discontinuity_pending;
            analysis_lane_reset(&g_lanes[i]);
            g_lanes[i].active = 1U;
            g_lanes[i].tile_index = tile_index;
            /* The first complete window after a gap has no valid predecessor.
             * Publish all 16 rows even when its formal tile index is nonzero. */
            g_lanes[i].display_rows_published =
                ((tile_index == 0U) || (discontinuity != 0U)) ?
                0U : ANALYSIS_DISPLAY_OVERLAP_ROWS;
            g_lanes[i].start_sample = g_analysis.stream_origin_sample +
                                       ((uint64_t)tile_index * g_analysis.stride_samples);
            g_lanes[i].start_cycles = analysis_cycle_now();
            g_lanes[i].dwt_epoch = g_analysis.dwt_epoch;
            g_lanes[i].timing_valid = analysis_dwt_enabled() ? 1U : 0U;
            g_lanes[i].discontinuity = discontinuity;
            g_analysis.discontinuity_pending = 0U;
            return &g_lanes[i];
        }
    }
    return NULL;
}

static uint8_t analysis_display_tile_count(void)
{
    return (uint8_t)((g_analysis.expected_tile_count >
                      ANALYSIS_MODEL_TILE_COUNT_MAX) ?
                     ANALYSIS_MODEL_TILE_COUNT_MAX :
                     g_analysis.expected_tile_count);
}

static void analysis_publish_ready_display_rows(analysis_lane_t *lane, bool final)
{
    uint32_t ready_rows;
    uint32_t novel_count;
    uint32_t flags;

    if ((lane == NULL) || (g_stft_probe.active != 0U) ||
        (g_analysis.synthetic != 0U))
    {
        return;
    }

    ready_rows = lane->display_row_count;
    if (ready_rows > RA8P1_DISPLAY_TILE_HEIGHT)
    {
        ready_rows = RA8P1_DISPLAY_TILE_HEIGHT;
    }
    if (ready_rows <= lane->display_rows_published)
    {
        return;
    }

    novel_count = ready_rows - lane->display_rows_published;
    if (!final)
    {
        if (novel_count < ANALYSIS_DISPLAY_ROWS_PER_PUBLISH)
        {
            return;
        }
        novel_count = ANALYSIS_DISPLAY_ROWS_PER_PUBLISH;
    }

    flags = RA8P1_DISPLAY_FLAG_REAL_STREAM;
    if (g_analysis.center_index >= RF_V12_CENTER_COUNT)
    {
        flags |= RA8P1_DISPLAY_FLAG_PREPROCESS_INVALID;
    }
    if (lane->discontinuity != 0U)
    {
        flags |= RA8P1_DISPLAY_FLAG_DISCONTINUITY;
    }
    ipc_bridge_cpu0_display_tile_publish_ex(
        lane->display_tile,
        lane->tile_index,
        flags,
        g_analysis.center_index,
        (uint8_t)(lane->tile_index & 0xFFU),
        analysis_display_tile_count(),
        (uint8_t)lane->display_rows_published,
        (uint8_t)novel_count);
    lane->display_rows_published += novel_count;
}

#if 0
static ANALYSIS_HOT_CODE void analysis_store_pool_row(analysis_lane_t *lane)
{
    analysis_lane_hot_t *hot;
    uint32_t time_bin;
    if ((lane == NULL) ||
        (lane->hot == NULL) ||
        (lane->time_bin >= ANALYSIS_TIME_BINS) ||
        (lane->pool_frame_count != ANALYSIS_TIME_POOL))
    {
        return;
    }
    hot = lane->hot;
    time_bin = lane->time_bin;
#if ANALYSIS_HAS_MVEF
    {
        uint64_t sums[4] __attribute__((aligned(16)));
        uint32_t max_powers[4] __attribute__((aligned(16)));
        uint32_t divisors[4] __attribute__((aligned(16)));
        float c0_values[4] __attribute__((aligned(16)));
        float c1_values[4] __attribute__((aligned(16)));

        for (uint32_t base = 0U; base < ANALYSIS_FREQ_BINS;
             base += 4U)
        {
            for (uint32_t lane_index = 0U; lane_index < 4U; ++lane_index)
            {
                const uint32_t freq_bin = base + lane_index;
                sums[lane_index] = hot->pool_power_sum[freq_bin];
                max_powers[lane_index] = hot->pool_power_max[freq_bin];
                divisors[lane_index] = g_pool_divisor[freq_bin];
            }
            analysis_log_power_mve4(sums, divisors, c0_values);
            analysis_log_power_max_mve4(max_powers, c1_values);
            g_analysis.log_values += 8U;

            for (uint32_t lane_index = 0U; lane_index < 4U; ++lane_index)
            {
                const uint32_t freq_bin = base + lane_index;
                const uint64_t sum = sums[lane_index];
                const uint32_t divisor = divisors[lane_index];
                const uint32_t maximum = max_powers[lane_index];
                float c0 = c0_values[lane_index];
                bool c0_exact = false;
                bool c2_near = false;
                bool c0_near = false;
                bool c1_near = false;
                int8_t q0 = analysis_quantize_guarded(c0, 0U, &c0_near);
                int8_t q1 = analysis_quantize_guarded(c1_values[lane_index],
                                                     1U, &c1_near);

                if (c0_near)
                {
                    c0 = analysis_log_power(sum, divisor);
                    c0_exact = true;
                    q0 = analysis_quantize(c0, 0U);
                    g_analysis.log_fallbacks++;
                }
                if (c1_near)
                {
                    /* The maximum path always has divisor one. */
                    c1_values[lane_index] = analysis_log_power(
                        (uint64_t)maximum, 1U);
                    q1 = analysis_quantize(c1_values[lane_index], 1U);
                    g_analysis.log_fallbacks++;
                }

                float c2 = (time_bin == 0U) ? 0.0F :
                           fmaxf(c0 - hot->previous_c0[freq_bin], 0.0F);
                int8_t q2 = analysis_quantize_guarded(c2, 2U, &c2_near);
                if ((time_bin != 0U) && c2_near)
                {
                    const float exact_c0 = c0_exact ? c0 :
                        analysis_log_power(sum, divisor);
                    const float exact_previous = analysis_log_power(
                        hot->previous_power_sum[freq_bin], divisor);
                    c0 = exact_c0;
                    c2 = fmaxf(exact_c0 - exact_previous, 0.0F);
                    q0 = analysis_quantize(c0, 0U);
                    q2 = analysis_quantize(c2, 2U);
                    g_analysis.log_fallbacks += c0_exact ? 1U : 2U;
                }

                /* HWC with frequency as the first axis: [freq][time][channel]. */
                const uint32_t offset = ((freq_bin * ANALYSIS_TIME_BINS) +
                                         time_bin) * 3U;
                lane->model_input[offset] = q0;
                lane->model_input[offset + 1U] = q1;
                lane->model_input[offset + 2U] = q2;
                hot->previous_c0[freq_bin] = c0;
                hot->previous_power_sum[freq_bin] = sum;
                hot->pool_power_sum[freq_bin] = 0U;
                hot->pool_power_max[freq_bin] = 0U;
            }
        }
    }
#else
    for (uint32_t freq_bin = 0U; freq_bin < ANALYSIS_FREQ_BINS; ++freq_bin)
    {
        const uint64_t sum = hot->pool_power_sum[freq_bin];
        const uint32_t maximum = hot->pool_power_max[freq_bin];
        const uint32_t divisor = g_pool_divisor[freq_bin];
        const float c0 = analysis_log_power(sum, divisor);
        const float c1 = analysis_log_power((uint64_t)maximum, 1U);
        const float c2 = (time_bin == 0U) ? 0.0F :
                         fmaxf(c0 - hot->previous_c0[freq_bin], 0.0F);
        g_analysis.log_values += 2U;
        /* HWC with frequency as the first axis: [freq][time][channel]. */
        const uint32_t offset = ((freq_bin * ANALYSIS_TIME_BINS) + time_bin) * 3U;
        lane->model_input[offset] = analysis_quantize(c0, 0U);
        lane->model_input[offset + 1U] = analysis_quantize(c1, 1U);
        lane->model_input[offset + 2U] = analysis_quantize(c2, 2U);
        hot->previous_c0[freq_bin] = c0;
        hot->previous_power_sum[freq_bin] = sum;
        hot->pool_power_sum[freq_bin] = 0U;
        hot->pool_power_max[freq_bin] = 0U;
    }
#endif
    if (time_bin == (ANALYSIS_TIME_BINS - 1U))
    {
        analysis_store_display_spectrum(lane);
    }
    if (((time_bin + 1U) % ANALYSIS_DISPLAY_TIME_BINS_PER_ROW) == 0U)
    {
        analysis_store_display_row(
            lane,
            time_bin / ANALYSIS_DISPLAY_TIME_BINS_PER_ROW);
    }
    lane->pool_frame_count = 0U;
    lane->time_bin++;
    if ((lane->time_bin % ANALYSIS_DISPLAY_TIME_BINS_PER_PUBLISH) == 0U)
    {
        analysis_publish_ready_display_rows(lane, false);
    }
}
#endif

static inline __attribute__((always_inline)) void analysis_window_apply_segment(
    const q15_t *source,
    q15_t *destination,
    const q15_t *window,
    uint32_t complex_samples)
{
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0)
    for (uint32_t i = 0U; i < complex_samples; i += 8U)
    {
        q15x8x2_t samples = vld2q(source);
        const q15x8_t coefficients = vld1q(&window[i]);
        /* vqdmulhq is (2*a*b)>>16 with saturation, exactly matching the
         * scalar (a*b)>>15 for this non-negative Hann window.  The sole Q15
         * saturation corner requires a == b == INT16_MIN; b is 0..32767. */
        samples.val[0] = vqdmulhq(samples.val[0], coefficients);
        samples.val[1] = vqdmulhq(samples.val[1], coefficients);
        vst2q(destination, samples);
        source += 16U;
        destination += 16U;
    }
#else
    for (uint32_t i = 0U; i < complex_samples; ++i)
    {
        destination[2U * i] = (q15_t)(((int32_t)source[2U * i] *
                                        window[i]) >> 15);
        destination[(2U * i) + 1U] = (q15_t)(((int32_t)source[(2U * i) + 1U] *
                                               window[i]) >> 15);
    }
#endif
}

static ANALYSIS_HOT_CODE void analysis_copy_window_apply(const analysis_lane_t *lane)
{
    const analysis_lane_hot_t *hot = lane->hot;
    const uint32_t first_samples = ANALYSIS_FFT_SIZE - lane->frame_head;
    analysis_window_apply_segment(&hot->frame_iq[2U * lane->frame_head],
                                  g_fft_iq,
                                  g_window,
                                  first_samples);
    if (first_samples < ANALYSIS_FFT_SIZE)
    {
        analysis_window_apply_segment(hot->frame_iq,
                                      &g_fft_iq[2U * first_samples],
                                      &g_window[first_samples],
                                      ANALYSIS_FFT_SIZE - first_samples);
    }
}

static inline __attribute__((always_inline)) void analysis_fft_power_segment(
    const q15_t *source,
    uint32_t *destination,
    uint32_t complex_samples)
{
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0)
    for (uint32_t i = 0U; i < complex_samples; i += 8U)
    {
        q15x8x2_t samples = vld2q(source);
        samples.val[0] =
            vshrq_n_s16(samples.val[0], ANALYSIS_Q15_FFT_SHIFT);
        samples.val[1] =
            vshrq_n_s16(samples.val[1], ANALYSIS_Q15_FFT_SHIFT);
        int32x4_t low = vaddq_s32(
            vmullbq_int_s16(samples.val[0], samples.val[0]),
            vmullbq_int_s16(samples.val[1], samples.val[1]));
        int32x4_t high = vaddq_s32(
            vmulltq_int_s16(samples.val[0], samples.val[0]),
            vmulltq_int_s16(samples.val[1], samples.val[1]));
        vst1q_u32(destination, vreinterpretq_u32_s32(low));
        vst1q_u32(destination + 4U, vreinterpretq_u32_s32(high));
        source += 16U;
        destination += 8U;
    }
#else
    for (uint32_t i = 0U; i < complex_samples; ++i)
    {
        const int32_t real =
            source[2U * i] >> ANALYSIS_Q15_FFT_SHIFT;
        const int32_t imag =
            source[(2U * i) + 1U] >> ANALYSIS_Q15_FFT_SHIFT;
        destination[i] = (uint32_t)((real * real) + (imag * imag));
    }
#endif
}

static inline __attribute__((always_inline)) void analysis_fft_power_all(void)
{
    analysis_fft_power_segment(
        &g_fft_iq[ANALYSIS_FFT_SIZE],
        g_fft_power,
        ANALYSIS_FFT_SIZE / 2U);
    analysis_fft_power_segment(
        g_fft_iq,
        &g_fft_power[ANALYSIS_FFT_SIZE / 2U],
        ANALYSIS_FFT_SIZE / 2U);
}

static inline __attribute__((always_inline)) void analysis_accumulate_spectrum_power(
    analysis_lane_hot_t *hot,
    uint32_t shifted_bin,
    uint32_t power)
{
    const uint32_t spectrum_bin = g_spectrum_raw_bin_map[shifted_bin];
    if (spectrum_bin < RA8P1_DISPLAY_SPECTRUM_BINS)
    {
        hot->spectrum_power_sum[spectrum_bin] += power;
    }
}

static ANALYSIS_HOT_CODE bool analysis_reduce_fft_power(analysis_lane_t *lane)
{
    analysis_lane_hot_t *hot = lane->hot;
    const bool capture_spectrum =
        lane->stft_frames >=
        (ANALYSIS_STFT_FRAMES_PER_TILE -
         ANALYSIS_SPECTRUM_AVERAGE_FRAMES);
    analysis_fft_power_all();

    for (uint32_t shifted_bin = 0U;
         shifted_bin < ANALYSIS_FFT_SIZE;
         ++shifted_bin)
    {
        const uint32_t fft_index =
            (shifted_bin + (ANALYSIS_FFT_SIZE / 2U)) &
            (ANALYSIS_FFT_SIZE - 1U);
        const uint32_t power = g_fft_power[shifted_bin];
        const uint32_t display_bin = g_display_raw_bin_map[shifted_bin];

        /* The map is rebuilt whenever the sample rate or bandwidth changes.
         * Its sentinel is the cached validity decision for this hot loop. */
        if (display_bin >= RA8P1_DISPLAY_TILE_WIDTH)
        {
            continue;
        }
        hot->display_power_sum[display_bin] += power;
        if (capture_spectrum)
        {
            analysis_accumulate_spectrum_power(hot, shifted_bin, power);
        }
        if (power > lane->peak_power)
        {
            lane->peak_power = power;
            lane->peak_bin = fft_index;
        }
    }

    return rf_v12_preprocess_frame_gathered(
        &lane->preprocess,
        g_v12_output_map,
        g_fft_power,
        lane->stft_frames,
        ANALYSIS_Q15_POWER_SCALE);
}

static void analysis_finish_processed_frame(analysis_lane_t *lane)
{
    lane->stft_frames++;
    g_analysis.stft_frames_total++;
    lane->display_frame_count++;
    if (lane->display_frame_count == ANALYSIS_DISPLAY_FRAMES_PER_ROW)
    {
        analysis_store_display_row(lane, lane->display_row_count);
        lane->display_frame_count = 0U;
        lane->display_row_count++;
        analysis_publish_ready_display_rows(lane, false);
    }
    if (lane->stft_frames == ANALYSIS_STFT_FRAMES_PER_TILE)
    {
        analysis_store_display_spectrum(lane);
    }
}

static ANALYSIS_HOT_CODE void analysis_process_frame(analysis_lane_t *lane)
{
    uint32_t start;
    uint32_t frame_elapsed;
    if ((lane == NULL) || (lane->hot == NULL) || !g_cfft_ready)
    {
        return;
    }
    start = analysis_cycle_now_fast();
    if (g_stft_probe.active != 0U)
    {
        uint32_t stage_start = start;
        uint32_t stage_end;

        analysis_copy_window_apply(lane);
        stage_end = analysis_cycle_now_fast();
        g_stft_probe.window_cycles += stage_end - stage_start;

        stage_start = stage_end;
        arm_cfft_q15(&g_cfft, g_fft_iq, 0U, 1U);
        stage_end = analysis_cycle_now_fast();
        g_stft_probe.fft_cycles += stage_end - stage_start;

        stage_start = stage_end;
        if (!analysis_reduce_fft_power(lane))
        {
            lane->discontinuity = 1U;
        }
        stage_end = analysis_cycle_now_fast();
        g_stft_probe.reduce_cycles += stage_end - stage_start;

        stage_start = stage_end;
        analysis_finish_processed_frame(lane);
        stage_end = analysis_cycle_now_fast();
        g_stft_probe.pool_cycles += stage_end - stage_start;
        frame_elapsed = stage_end - start;
    }
    else
    {
        analysis_copy_window_apply(lane);
        arm_cfft_q15(&g_cfft, g_fft_iq, 0U, 1U);
        if (!analysis_reduce_fft_power(lane))
        {
            lane->discontinuity = 1U;
        }
        analysis_finish_processed_frame(lane);
        frame_elapsed = analysis_cycle_now_fast() - start;
    }
    if ((lane->timing_valid != 0U) && (lane->dwt_epoch == g_analysis.dwt_epoch))
    {
        lane->stft_cycles += frame_elapsed;
    }
    else
    {
        lane->timing_valid = 0U;
    }
}

static ANALYSIS_HOT_CODE void analysis_lane_feed(analysis_lane_t *lane,
                                                  const q15_t *iq,
                                                  uint32_t complex_samples)
{
    analysis_lane_hot_t *hot;
    uint32_t consumed = 0U;
    if ((lane == NULL) || (lane->hot == NULL) || (iq == NULL))
    {
        return;
    }
    hot = lane->hot;
    while ((consumed < complex_samples) &&
           (lane->sample_count < ANALYSIS_MODEL_WINDOW_SAMPLES))
    {
        uint32_t room = ANALYSIS_FFT_SIZE - lane->frame_fill;
        uint32_t window_room = ANALYSIS_MODEL_WINDOW_SAMPLES - lane->sample_count;
        uint32_t take = complex_samples - consumed;
        if (take > room) take = room;
        if (take > window_room) take = window_room;
        const uint32_t write_index = (lane->frame_head + lane->frame_fill) &
                                     (ANALYSIS_FFT_SIZE - 1U);
        uint32_t first = take;
        const uint32_t physical_room = ANALYSIS_FFT_SIZE - write_index;
        if (first > physical_room) first = physical_room;
        memcpy(&hot->frame_iq[2U * write_index],
               &iq[2U * consumed], first * 2U * sizeof(q15_t));
        if (first < take)
        {
            memcpy(hot->frame_iq,
                   &iq[2U * (consumed + first)],
                   (take - first) * 2U * sizeof(q15_t));
        }
        lane->frame_fill += take;
        lane->sample_count += take;
        consumed += take;
        if (lane->frame_fill == ANALYSIS_FFT_SIZE)
        {
            analysis_process_frame(lane);
            if (lane->sample_count < ANALYSIS_MODEL_WINDOW_SAMPLES)
            {
                lane->frame_head = (lane->frame_head + ANALYSIS_HOP_SIZE) &
                                   (ANALYSIS_FFT_SIZE - 1U);
                lane->frame_fill = ANALYSIS_FFT_SIZE - ANALYSIS_HOP_SIZE;
            }
        }
    }
}

static void analysis_build_display_tile(const analysis_lane_t *lane)
{
    if (lane != NULL)
    {
        memcpy(g_display_tile, lane->display_tile, sizeof(g_display_tile));
    }
}

#if 0
static void analysis_decode_mask(ra8p1_display_frame_t *frame, bool inferred)
{
    const int8_t *mask = npu_runner_mask_logits();
    const int8_t *presence = npu_runner_device_logits();
    if (frame == NULL) return;
    memset(frame->analysis.mask_bits, 0, sizeof(frame->analysis.mask_bits));
    memset(frame->analysis.boxes, 0, sizeof(frame->analysis.boxes));
    frame->analysis.box_count = 0U;
    memset(frame->analysis.presence_q15, 0, sizeof(frame->analysis.presence_q15));
    for (uint32_t class_id = 0U; class_id < 4U; ++class_id)
    {
        float probability = 0.0F;
        if (inferred)
        {
            const float logit = ((float)presence[class_id] -
                                 (float)IQ_NPU_DEVICE_LOGITS_ZERO_POINT) *
                                IQ_NPU_DEVICE_LOGITS_SCALE;
            probability = 1.0F / (1.0F + expf(-logit));
        }
        frame->analysis.presence_q15[class_id] =
            (uint16_t)((probability <= 0.0F) ? 0U :
                       ((probability >= 1.0F) ? RA8P1_PROBABILITY_ONE_Q15 :
                        (uint32_t)(probability * RA8P1_PROBABILITY_ONE_Q15 + 0.5F)));
    }
    if (!inferred) return;
    /* The checked-in model is a 32x32 placeholder.  Classes remain
     * independent: no Softmax or argmax is used for mask decoding. */
    for (uint32_t class_id = 0U; class_id < 4U; ++class_id)
    {
        uint8_t visited[32U * 32U / 8U] = {0};
        uint16_t queue[32U * 32U];
        uint32_t best_min_x = RA8P1_DISPLAY_MASK_WIDTH;
        uint32_t best_min_y = RA8P1_DISPLAY_MASK_HEIGHT;
        uint32_t best_max_x = 0U;
        uint32_t best_max_y = 0U;
        uint32_t best_count = 0U;
        for (uint32_t y = 0U; y < 32U; ++y)
        {
            for (uint32_t x = 0U; x < 32U; ++x)
            {
                const uint32_t index = ((y * 32U + x) * 4U) + class_id;
                const uint32_t pixel = (y * 32U) + x;
                /* The model H axis is frequency and W is time because the
                 * input contract is [frequency][time][channel]. */
                const bool valid_frequency = analysis_fft_bin_valid((y * 32U) + 16U);
                const bool on = valid_frequency &&
                    (mask[index] >= IQ_NPU_MASK_LOGITS_ZERO_POINT) &&
                    (g_analysis_class_threshold[class_id] <= 0.5F);
                if (on)
                {
                    const uint32_t display_y = x / 2U;
                    const uint32_t display_x = y;
                    frame->analysis.mask_bits[(display_y * 32U + display_x) >> 3U] |=
                        (uint8_t)(1U << ((display_y * 32U + display_x) & 7U));
                }
                if (!on || ((visited[pixel >> 3U] &
                             (uint8_t)(1U << (pixel & 7U))) != 0U))
                {
                    continue;
                }
                uint32_t head = 0U;
                uint32_t tail = 0U;
                uint32_t min_x = x;
                uint32_t max_x = x;
                uint32_t min_y = y;
                uint32_t max_y = y;
                queue[tail++] = (uint16_t)pixel;
                visited[pixel >> 3U] |= (uint8_t)(1U << (pixel & 7U));
                while (head < tail)
                {
                    const uint32_t current = queue[head++];
                    const uint32_t cx = current & 31U;
                    const uint32_t cy = current >> 5U;
                    static const int8_t dx[4] = {-1, 1, 0, 0};
                    static const int8_t dy[4] = {0, 0, -1, 1};
                    if (cx < min_x) min_x = cx;
                    if (cx > max_x) max_x = cx;
                    if (cy < min_y) min_y = cy;
                    if (cy > max_y) max_y = cy;
                    for (uint32_t direction = 0U; direction < 4U; ++direction)
                    {
                        const int32_t nx = (int32_t)cx + dx[direction];
                        const int32_t ny = (int32_t)cy + dy[direction];
                        if ((nx < 0) || (nx >= 32) || (ny < 0) || (ny >= 32))
                            continue;
                        const uint32_t neighbour = ((uint32_t)ny * 32U) + (uint32_t)nx;
                        if ((visited[neighbour >> 3U] &
                             (uint8_t)(1U << (neighbour & 7U))) != 0U)
                            continue;
                        const uint32_t neighbour_index = (neighbour * 4U) + class_id;
                        if (!analysis_fft_bin_valid(((uint32_t)ny * 32U) + 16U) ||
                            (mask[neighbour_index] < IQ_NPU_MASK_LOGITS_ZERO_POINT))
                            continue;
                        visited[neighbour >> 3U] |=
                            (uint8_t)(1U << (neighbour & 7U));
                        queue[tail++] = (uint16_t)neighbour;
                    }
                }
                if (tail > best_count)
                {
                    best_count = tail;
                    best_min_x = min_y;
                    best_max_x = max_y;
                    best_min_y = min_x / 2U;
                    best_max_y = max_x / 2U;
                }
            }
        }
        if ((best_count >= 4U) &&
            (frame->analysis.box_count < RA8P1_DISPLAY_MAX_BOXES))
        {
            ra8p1_detection_box_t *box =
                &frame->analysis.boxes[frame->analysis.box_count++];
            box->x = (uint8_t)best_min_x;
            box->y = (uint8_t)best_min_y;
            box->width = (uint8_t)((best_max_x - best_min_x) + 1U);
            box->height = (uint8_t)((best_max_y - best_min_y) + 1U);
            box->class_id = (uint8_t)class_id;
            box->score = (uint8_t)(((uint32_t)frame->analysis.presence_q15[class_id] * 255U) /
                                   RA8P1_PROBABILITY_ONE_Q15);
        }
    }
}
#endif

static uint32_t analysis_scale_half_open(int64_t value,
                                         int64_t minimum,
                                         uint32_t output_size,
                                         int64_t span,
                                         bool upper)
{
    int64_t relative = value - minimum;
    int64_t scaled;

    if (relative <= 0)
    {
        return 0U;
    }
    if (relative >= span)
    {
        return output_size;
    }
    scaled = relative * (int64_t)output_size;
    if (upper)
    {
        scaled += span - 1;
    }
    return (uint32_t)(scaled / span);
}

static bool analysis_event_to_display_box(
    const rf_v12_visible_event_t *event,
    ra8p1_detection_box_t *box)
{
    const int64_t frequency_span_hz = RF_V12_RELIABLE_BANDWIDTH_HZ;
    const int64_t frequency_min_hz = -(frequency_span_hz / 2LL);
    const int64_t frequency_max_hz = frequency_min_hz + frequency_span_hz;
    uint32_t frequency_start;
    uint32_t frequency_end;
    uint32_t time_start;
    uint32_t time_end;
    uint8_t object_id;
    uint8_t geometry_flags = RA8P1_DISPLAY_BOX_FLAG_RF_GEOMETRY_VALID;

    if ((event == NULL) || (box == NULL))
    {
        return false;
    }
    if (((int64_t)event->frequency_high_offset_hz <= frequency_min_hz) ||
        ((int64_t)event->frequency_low_offset_hz >= frequency_max_hz) ||
        (event->visible_start_sample >= RF_V12_TILE_SAMPLES) ||
        (event->visible_end_sample == 0U))
    {
        return false;
    }

    frequency_start = analysis_scale_half_open(
        event->frequency_low_offset_hz,
        frequency_min_hz,
        RA8P1_DISPLAY_RF_COORD_SCALE,
        frequency_span_hz,
        false);
    frequency_end = analysis_scale_half_open(
        event->frequency_high_offset_hz,
        frequency_min_hz,
        RA8P1_DISPLAY_RF_COORD_SCALE,
        frequency_span_hz,
        true);
    time_start = analysis_scale_half_open(
        event->visible_start_sample,
        0LL,
        RA8P1_DISPLAY_RF_COORD_SCALE,
        RF_V12_TILE_SAMPLES,
        false);
    time_end = analysis_scale_half_open(
        event->visible_end_sample,
        0LL,
        RA8P1_DISPLAY_RF_COORD_SCALE,
        RF_V12_TILE_SAMPLES,
        true);
    object_id = rf_v12_class_to_object(event->class_id);

    if ((object_id >= RF_V12_OBJECT_COUNT) ||
        (frequency_start >= RA8P1_DISPLAY_RF_COORD_SCALE) ||
        (time_start >= RA8P1_DISPLAY_RF_COORD_SCALE))
    {
        return false;
    }
    if (frequency_end <= frequency_start)
    {
        frequency_end = frequency_start + 1U;
    }
    if (time_end <= time_start)
    {
        time_end = time_start + 1U;
    }
    if (frequency_end > RA8P1_DISPLAY_RF_COORD_SCALE)
    {
        frequency_end = RA8P1_DISPLAY_RF_COORD_SCALE;
    }
    if (time_end > RA8P1_DISPLAY_RF_COORD_SCALE)
    {
        time_end = RA8P1_DISPLAY_RF_COORD_SCALE;
    }
    if (((int64_t)event->frequency_low_offset_hz < frequency_min_hz) ||
        ((int64_t)event->frequency_high_offset_hz > frequency_max_hz) ||
        ((event->flags & RF_V12_EVENT_FREQUENCY_CLIPPED) != 0U))
    {
        geometry_flags |= RA8P1_DISPLAY_BOX_FLAG_FREQUENCY_CLIPPED;
    }
    if ((event->flags & RF_V12_EVENT_TIME_CLIPPED) != 0U)
    {
        geometry_flags |= RA8P1_DISPLAY_BOX_FLAG_TIME_CLIPPED;
    }
    if ((event->flags & RF_V12_EVENT_VIDEO_20MHZ) != 0U)
    {
        geometry_flags |= RA8P1_DISPLAY_BOX_FLAG_VIDEO_20MHZ;
    }
    if ((event->flags & RF_V12_EVENT_BANDWIDTH_AMBIGUOUS) != 0U)
    {
        geometry_flags |= RA8P1_DISPLAY_BOX_FLAG_BANDWIDTH_AMBIGUOUS;
    }
    if ((event->flags & RF_V12_EVENT_NEEDS_REVIEW) != 0U)
    {
        geometry_flags |= RA8P1_DISPLAY_BOX_FLAG_NEEDS_REVIEW;
    }

    memset(box, 0, sizeof(*box));
    box->frequency_start_q8 = (uint8_t)frequency_start;
    box->time_start_q8 = (uint8_t)time_start;
    box->frequency_span_q8 =
        (uint8_t)(frequency_end - frequency_start);
    box->time_span_q8 = (uint8_t)(time_end - time_start);
    box->class_id = object_id;
    box->score = (uint8_t)(((uint32_t)event->confidence_q15 * 255U +
                            (RF_V12_CONFIDENCE_Q15_ONE / 2U)) /
                           RF_V12_CONFIDENCE_Q15_ONE);
    box->metadata =
        ((uint16_t)event->class_id & RA8P1_DISPLAY_BOX_SOURCE_CLASS_MASK) |
        ((uint16_t)geometry_flags << RA8P1_DISPLAY_BOX_FLAGS_SHIFT);
    return true;
}

static void analysis_apply_detector_result(
    ra8p1_display_frame_t *frame,
    const rf_v12_detector_result_t *result,
    bool inferred)
{
    if ((frame == NULL) || (result == NULL))
    {
        return;
    }
    memset(frame->analysis.mask_bits, 0,
           sizeof(frame->analysis.mask_bits));
    memset(frame->analysis.boxes, 0,
           sizeof(frame->analysis.boxes));
    memset(frame->analysis.presence_q15, 0,
           sizeof(frame->analysis.presence_q15));
    frame->analysis.box_count = 0U;
    if (!inferred)
    {
        return;
    }

    memcpy(frame->analysis.mask_bits,
           result->display_mask,
           sizeof(frame->analysis.mask_bits));
    memcpy(frame->analysis.presence_q15,
           result->object_presence_q15,
           sizeof(frame->analysis.presence_q15));
    for (uint32_t index = 0U;
         (index < result->tile.event_count) &&
         (frame->analysis.box_count < RA8P1_DISPLAY_MAX_BOXES);
         ++index)
    {
        ra8p1_detection_box_t box;
        if (analysis_event_to_display_box(&result->tile.events[index], &box))
        {
            frame->analysis.boxes[frame->analysis.box_count++] = box;
        }
    }
    frame->analysis.npu_class =
        rf_v12_class_to_object(result->best_class_id);
    frame->analysis.npu_score_q15 = result->best_score_q15;
}

static void analysis_capture_proof_lane(analysis_lane_t *lane)
{
    if ((lane == NULL) || (g_stft_probe.active == 0U))
    {
        return;
    }
    if (!rf_v12_preprocess_finalize_synthetic(&lane->preprocess))
    {
        return;
    }
    g_stft_probe.complete_cycles = analysis_cycle_now();
    g_stft_probe.checksum = analysis_checksum_bytes(
        (const uint8_t *)lane->model_input,
        ANALYSIS_MODEL_INPUT_BYTES);
    g_stft_probe.stft_frames = lane->stft_frames;
    g_stft_probe.stft_hot_cycles = lane->stft_cycles;
    g_stft_probe.peak_bin = lane->peak_bin;
    g_stft_probe.peak_power = lane->peak_power;
    __DMB();
    g_stft_probe.completed = 1U;
}

#if 0
static void analysis_publish_lane(analysis_lane_t *lane)
{
    ra8p1_display_frame_t frame;
    npu_runner_stats_t npu;
    bool inferred;
    bool latency_timing_valid;
    uint32_t complete_cycles;
    uint32_t npu_start_cycles;
    uint32_t end_cycles;
    uint32_t cpu0_load_permille = 0U;
    if ((lane == NULL) || (lane->sample_count != ANALYSIS_MODEL_WINDOW_SAMPLES) ||
        (lane->stft_frames != ANALYSIS_STFT_FRAMES_PER_TILE) ||
        (lane->time_bin != ANALYSIS_TIME_BINS))
    {
        if (lane != NULL) g_analysis.partial_windows_dropped++;
        return;
    }
    complete_cycles = analysis_cycle_now();
    latency_timing_valid = (lane->timing_valid != 0U) &&
                           (lane->dwt_epoch == g_analysis.dwt_epoch) &&
                           analysis_dwt_enabled();
    ipc_bridge_cpu0_latency_window_complete(g_analysis.session_id,
                                             lane->tile_index,
                                             complete_cycles,
                                             latency_timing_valid);
    npu_start_cycles = analysis_cycle_now();
    inferred = npu_runner_infer(lane->model_input, NPU_RUNNER_INPUT_BYTES);
    npu_runner_stats_get(&npu);
    end_cycles = analysis_cycle_now();
    memset(&frame, 0, sizeof(frame));
    frame.sample_rate_hz = g_analysis.sample_rate_hz;
    frame.fft_size = ANALYSIS_FFT_SIZE;
    frame.channel_mask = RA8P1_RF_CHANNEL_A_MASK;
    frame.flags = RA8P1_DISPLAY_FLAG_WINDOW_COMPLETE |
                  RA8P1_DISPLAY_FLAG_MODEL_PLACEHOLDER |
                  RA8P1_DISPLAY_FLAG_PREPROCESS_INVALID;
    if (g_analysis.synthetic != 0U)
        frame.flags |= RA8P1_DISPLAY_FLAG_SYNTHETIC;
    else
        frame.flags |= RA8P1_DISPLAY_FLAG_REAL_STREAM;
    if (lane->discontinuity != 0U) frame.flags |= RA8P1_DISPLAY_FLAG_DISCONTINUITY;
    if (inferred) frame.flags |= RA8P1_DISPLAY_FLAG_MODEL_MASK_VALID;
    frame.publish_tick = (uint32_t)rt_tick_get();
    frame.peak_bin[0] = (lane->peak_bin + (ANALYSIS_FFT_SIZE / 2U)) &
                        (ANALYSIS_FFT_SIZE - 1U);
    frame.peak_power_q16[0] = lane->peak_power;
    memcpy(frame.spectrum[0], lane->last_spectrum, sizeof(lane->last_spectrum));
    frame.session_id = g_analysis.session_id;
    frame.analysis.window_sequence = lane->tile_index;
    frame.analysis.sample_index_low = (uint32_t)lane->start_sample;
    frame.analysis.sample_index_high = (uint32_t)(lane->start_sample >> 32U);
    frame.analysis.window_sample_count = ANALYSIS_MODEL_WINDOW_SAMPLES;
    frame.analysis.stft_frame_count = lane->stft_frames;
    frame.analysis.stft_cycles = lane->stft_cycles;
    frame.analysis.npu_cycles = npu.last_cycles;
    frame.analysis.end_to_end_cycles = end_cycles - lane->start_cycles;
    frame.analysis.timing_flags = 0U;
    if (npu.last_timing_valid != 0U)
        frame.analysis.timing_flags |= RA8P1_DISPLAY_TIMING_NPU_VALID;
    if ((lane->timing_valid != 0U) &&
        (lane->dwt_epoch == g_analysis.dwt_epoch) &&
        (npu.last_dwt_recovered == 0U))
        frame.analysis.timing_flags |= RA8P1_DISPLAY_TIMING_STFT_VALID |
                                       RA8P1_DISPLAY_TIMING_E2E_VALID;
    if (frame.analysis.end_to_end_cycles != 0U)
    {
        uint64_t busy_cycles = (uint64_t)lane->stft_cycles + npu.last_cycles;
        uint64_t load = (busy_cycles * 1000U) /
                        frame.analysis.end_to_end_cycles;
        cpu0_load_permille = (load > 1000U) ? 1000U : (uint32_t)load;
    }
    cpu0_trace_analysis(g_analysis.session_id,
                        lane->tile_index,
                        lane->start_cycles,
                        complete_cycles,
                        npu_start_cycles,
                        end_cycles,
                        cpu0_load_permille,
                        NULL);
    frame.analysis.npu_inference_count = npu.inference_count;
    frame.analysis.npu_class = npu.last_class;
    frame.analysis.npu_score_q15 = npu.last_score_q15;
    frame.analysis.queue_depth = g_analysis.queue_depth;
    frame.analysis.ingress_drops = g_analysis.ingress_drops;
    frame.analysis.npu_ready = npu.ready;
    frame.analysis.mask_width_height = (RA8P1_DISPLAY_MASK_WIDTH << 16U) |
                                       RA8P1_DISPLAY_MASK_HEIGHT;
    frame.analysis.center_frequency_low = (uint32_t)g_analysis.center_frequency_hz;
    frame.analysis.center_frequency_high = (uint32_t)(g_analysis.center_frequency_hz >> 32U);
    frame.analysis.source_sample_rate_hz = g_analysis.source_sample_rate_hz;
    frame.analysis.valid_bits = g_analysis.valid_bits;
    frame.analysis.center_index = (uint8_t)g_analysis.center_index;
    frame.analysis.tile_index = (uint8_t)(lane->tile_index & 0xFFU);
    frame.analysis.tile_count = analysis_display_tile_count();
    frame.analysis.model_flags = RA8P1_MODEL_FLAG_PLACEHOLDER |
                                 RA8P1_MODEL_FLAG_MASK_32X32 |
                                 RA8P1_MODEL_FLAG_PREPROCESS_PLACEHOLDER |
                                 RA8P1_MODEL_FLAG_NOISE_FLOOR_ZERO |
                                 RA8P1_MODEL_FLAG_NO_ACCURACY_CLAIM;
    analysis_decode_mask(&frame, inferred);
    analysis_build_display_tile(lane);
    if (g_analysis.synthetic != 0U)
    {
        /* Preserve the explicit synthetic test hook's legacy final tile.  The
         * boot STFT proof is completed without entering this function. */
        ipc_bridge_cpu0_display_tile_publish_ex(
            g_display_tile,
            lane->tile_index,
            frame.flags,
            g_analysis.center_index,
            (uint8_t)(lane->tile_index & 0xFFU),
            frame.analysis.tile_count,
            (uint8_t)((lane->tile_index == 0U) ? 0U :
                      ANALYSIS_DISPLAY_OVERLAP_ROWS),
            (uint8_t)((lane->tile_index == 0U) ?
                      RA8P1_DISPLAY_TILE_HEIGHT :
                      ANALYSIS_DISPLAY_OVERLAP_ROWS));
    }
    else
    {
        /* Normal full windows land exactly on a row publication boundary.  This call
         * only publishes a remainder if a future display contract does not. */
        analysis_publish_ready_display_rows(lane, true);
    }
    ipc_bridge_cpu0_display_publish(&frame, NULL);
    g_analysis.npu_cycles_last = frame.analysis.npu_cycles;
    g_analysis.stft_cycles_last = frame.analysis.stft_cycles;
    g_analysis.end_to_end_cycles_last = frame.analysis.end_to_end_cycles;
    g_analysis.npu_ready = npu.ready;
    g_analysis.windows_completed++;
    g_last_peak_bin = frame.peak_bin[0];
    g_last_peak_power = frame.peak_power_q16[0];
    g_last_tile_index = lane->tile_index;
}
#endif

static uint32_t analysis_take_v12_tile_sequence(void)
{
    const uint32_t sequence = g_v12_tile_sequence;
    g_v12_tile_sequence++;
    return sequence;
}

static uint64_t analysis_tile_duration_us(void)
{
    return (((uint64_t)RF_V12_TILE_SAMPLES * UINT64_C(1000000)) +
            RF_V12_SAMPLE_RATE_HZ - 1U) /
           RF_V12_SAMPLE_RATE_HZ;
}

static void analysis_publish_lane(analysis_lane_t *lane,
                                  bool capture_valid,
                                  uint8_t tile_flags)
{
    ra8p1_display_frame_t frame;
    npu_runner_stats_t npu;
    cpu0_trace_inference_phases_t trace_phases = {0};
    rf_v12_detector_input_t detector_input;
    rf_v12_detector_result_t detector_result;
    rf_v12_preprocess_result_t preprocess_result =
        RF_V12_PREPROCESS_INVALID;
    bool inferred = false;
    bool latency_timing_valid;
    uint32_t complete_cycles;
    uint32_t npu_start_cycles;
    uint32_t postprocess_start_cycles;
    uint32_t postprocess_end_cycles;
    uint32_t postprocess_dwt_epoch;
    uint32_t end_cycles;
    uint32_t cpu0_load_permille = 0U;
    uint8_t tile_validity = RF_V12_TILE_INVALID;
    uint64_t capture_start_time_us;
    uint64_t capture_end_time_us;

    if ((lane == NULL) ||
        (lane->sample_count != ANALYSIS_MODEL_WINDOW_SAMPLES) ||
        (lane->stft_frames != ANALYSIS_STFT_FRAMES_PER_TILE) ||
        (lane->display_frame_count != 0U) ||
        (lane->display_row_count != RA8P1_DISPLAY_TILE_HEIGHT) ||
        !rf_v12_preprocess_tile_complete(&lane->preprocess))
    {
        if (lane != NULL)
        {
            g_analysis.partial_windows_dropped++;
        }
        return;
    }

    if (lane->discontinuity != 0U)
    {
        capture_valid = false;
        tile_flags |= RF_V12_TILE_PACKET_GAP;
    }
    complete_cycles = (lane->complete_cycles != 0U) ?
                      lane->complete_cycles : analysis_cycle_now();
    latency_timing_valid = (lane->timing_valid != 0U) &&
                           (lane->dwt_epoch == g_analysis.dwt_epoch) &&
                           analysis_dwt_enabled();
    ipc_bridge_cpu0_latency_window_complete(g_analysis.session_id,
                                             lane->tile_index,
                                             complete_cycles,
                                             latency_timing_valid);

    if (g_analysis.synthetic != 0U)
    {
        if (rf_v12_preprocess_finalize_synthetic(&lane->preprocess))
        {
            preprocess_result = RF_V12_PREPROCESS_READY;
            tile_validity = RF_V12_TILE_VALID;
        }
    }
    else
    {
        preprocess_result = rf_v12_preprocess_finalize(
            &lane->preprocess,
            capture_valid && (tile_flags == 0U));
        if (preprocess_result == RF_V12_PREPROCESS_READY)
        {
            tile_validity = RF_V12_TILE_VALID;
        }
    }

    npu_start_cycles = analysis_cycle_now();
    if (tile_validity == RF_V12_TILE_VALID)
    {
        inferred = npu_runner_infer(lane->model_input,
                                    NPU_RUNNER_INPUT_BYTES);
        if (!inferred)
        {
            tile_validity = RF_V12_TILE_INVALID;
            tile_flags |= RF_V12_TILE_CAPTURE_TIMEOUT;
        }
    }
    npu_runner_stats_get(&npu);

    capture_start_time_us = g_analysis.capture_start_time_us +
        (((uint64_t)lane->tile_index * ANALYSIS_MODEL_STRIDE_SAMPLES *
          UINT64_C(1000000)) / RF_V12_SAMPLE_RATE_HZ);
    capture_end_time_us = capture_start_time_us +
                          analysis_tile_duration_us();
    memset(&detector_input, 0, sizeof(detector_input));
    detector_input.tile_sequence =
        (g_analysis.synthetic != 0U) ? 0U :
        analysis_take_v12_tile_sequence();
    detector_input.round_index = g_analysis.result_round_index;
    detector_input.center_index = g_analysis.center_index;
    detector_input.center_frequency_hz = g_analysis.center_frequency_hz;
    detector_input.capture_start_time_us = capture_start_time_us;
    detector_input.capture_end_time_us = capture_end_time_us;
    detector_input.background_generation = 0U;
    detector_input.sdr_gain_db_q8 = 0;
    detector_input.tile_validity = tile_validity;
    detector_input.tile_flags = tile_flags;
    detector_input.c0_db = rf_v12_preprocess_c0(&lane->preprocess);
    detector_input.model_input = lane->model_input;
    postprocess_start_cycles = analysis_cycle_now();
    postprocess_dwt_epoch = g_analysis.dwt_epoch;
    rf_v12_detector_decode(&detector_input, &detector_result);
    postprocess_end_cycles = analysis_cycle_now();
    if (inferred && analysis_dwt_enabled() &&
        (postprocess_dwt_epoch == g_analysis.dwt_epoch))
    {
        trace_phases.postprocess_cycles =
            postprocess_end_cycles - postprocess_start_cycles;
        trace_phases.flags |= CPU0_TRACE_PHASE_POSTPROCESS_VALID;
    }
    if (inferred)
    {
        npu_runner_result_set(
            rf_v12_class_to_object(detector_result.best_class_id),
                              detector_result.best_score_q15);
        npu_runner_stats_get(&npu);
    }
    if ((g_analysis.synthetic == 0U) &&
        (g_analysis.center_index < RF_V12_CENTER_COUNT) &&
        (g_analysis.result_round_index != 0U))
    {
        (void)rf_v13_round_builder_submit_processed(
            &detector_result.tile,
            detector_result.state_confidence_q15,
            detector_result.state_roi_decision,
            detector_result.state_quality_tier,
            g_analysis.session_id,
            lane->tile_index);
        g_v13_round_center_mask |= 1UL << g_analysis.center_index;
        g_analysis.capture_result_submitted = 1U;
        if (capture_end_time_us > g_last_capture_end_time_us)
        {
            g_last_capture_end_time_us = capture_end_time_us;
        }
    }

    end_cycles = analysis_cycle_now();
    memset(&frame, 0, sizeof(frame));
    frame.sample_rate_hz = g_analysis.sample_rate_hz;
    frame.fft_size = ANALYSIS_FFT_SIZE;
    frame.channel_mask = RA8P1_RF_CHANNEL_A_MASK;
    frame.flags = RA8P1_DISPLAY_FLAG_WINDOW_COMPLETE;
    if (g_analysis.synthetic != 0U)
    {
        frame.flags |= RA8P1_DISPLAY_FLAG_SYNTHETIC;
    }
    else
    {
        frame.flags |= RA8P1_DISPLAY_FLAG_REAL_STREAM;
    }
    if (lane->discontinuity != 0U)
    {
        frame.flags |= RA8P1_DISPLAY_FLAG_DISCONTINUITY;
    }
    if (inferred)
    {
        frame.flags |= RA8P1_DISPLAY_FLAG_MODEL_MASK_VALID;
    }
    if (preprocess_result != RF_V12_PREPROCESS_READY)
    {
        frame.flags |= RA8P1_DISPLAY_FLAG_PREPROCESS_INVALID;
    }
    frame.publish_tick = (uint32_t)rt_tick_get();
    frame.peak_bin[0] =
        (lane->peak_bin + (ANALYSIS_FFT_SIZE / 2U)) &
        (ANALYSIS_FFT_SIZE - 1U);
    frame.peak_power_q16[0] = lane->peak_power;
    memcpy(frame.spectrum[0], lane->last_spectrum,
           sizeof(lane->last_spectrum));
    frame.session_id = g_analysis.session_id;
    frame.analysis.window_sequence = lane->tile_index;
    frame.analysis.sample_index_low = (uint32_t)lane->start_sample;
    frame.analysis.sample_index_high =
        (uint32_t)(lane->start_sample >> 32U);
    frame.analysis.window_sample_count = ANALYSIS_MODEL_WINDOW_SAMPLES;
    frame.analysis.stft_frame_count = lane->stft_frames;
    frame.analysis.stft_cycles = lane->stft_cycles;
    frame.analysis.npu_cycles = inferred ? npu.last_cycles : 0U;
    frame.analysis.end_to_end_cycles = end_cycles - lane->start_cycles;
    if (inferred && (npu.last_timing_valid != 0U))
    {
        frame.analysis.timing_flags |= RA8P1_DISPLAY_TIMING_NPU_VALID;
    }
    if ((lane->timing_valid != 0U) &&
        (lane->dwt_epoch == g_analysis.dwt_epoch) &&
        (npu.last_dwt_recovered == 0U))
    {
        frame.analysis.timing_flags |=
            RA8P1_DISPLAY_TIMING_STFT_VALID |
            RA8P1_DISPLAY_TIMING_E2E_VALID;
    }
    if (frame.analysis.end_to_end_cycles != 0U)
    {
        const uint64_t busy_cycles = (uint64_t)lane->stft_cycles +
                                     frame.analysis.npu_cycles;
        const uint64_t load =
            (busy_cycles * 1000U) / frame.analysis.end_to_end_cycles;
        cpu0_load_permille = (load > 1000U) ? 1000U : (uint32_t)load;
    }
    if (inferred && (npu.last_stage_timing_valid != 0U))
    {
        trace_phases.v2_input_copy_cycles =
            npu.last_v2_input_copy_cycles;
        trace_phases.v2_invoke_cycles = npu.last_v2_invoke_cycles;
        trace_phases.v2_output_copy_cycles =
            npu.last_v2_output_copy_cycles;
        trace_phases.v3_input_copy_cycles =
            npu.last_v3_input_copy_cycles;
        trace_phases.v3_invoke_cycles = npu.last_v3_invoke_cycles;
        trace_phases.v3_output_copy_cycles =
            npu.last_v3_output_copy_cycles;
        trace_phases.flags |= CPU0_TRACE_PHASE_NPU_VALID;
    }
    cpu0_trace_analysis(g_analysis.session_id,
                        lane->tile_index,
                        lane->start_cycles,
                        complete_cycles,
                        npu_start_cycles,
                        end_cycles,
                        cpu0_load_permille,
                        &trace_phases);
    frame.analysis.npu_inference_count = npu.inference_count;
    frame.analysis.queue_depth = g_analysis.queue_depth;
    frame.analysis.ingress_drops = g_analysis.ingress_drops;
    frame.analysis.npu_ready = npu.ready;
    frame.analysis.mask_width_height =
        (RA8P1_DISPLAY_MASK_WIDTH << 16U) |
        RA8P1_DISPLAY_MASK_HEIGHT;
    frame.analysis.center_frequency_low =
        (uint32_t)g_analysis.center_frequency_hz;
    frame.analysis.center_frequency_high =
        (uint32_t)(g_analysis.center_frequency_hz >> 32U);
    frame.analysis.source_sample_rate_hz =
        g_analysis.source_sample_rate_hz;
    frame.analysis.valid_bits = g_analysis.valid_bits;
    frame.analysis.center_index = (uint8_t)g_analysis.center_index;
    frame.analysis.tile_index = (uint8_t)(lane->tile_index & 0xFFU);
    frame.analysis.tile_count = analysis_display_tile_count();
    frame.analysis.model_flags =
        RA8P1_MODEL_FLAG_TRAINED_INT8 |
        RA8P1_MODEL_FLAG_CENTER_HEATMAP |
        RA8P1_MODEL_FLAG_FIVE_CLASS_FUSED_UI |
        RA8P1_MODEL_FLAG_CONSERVATIVE_ALERT_GUARD |
        RA8P1_MODEL_FLAG_DUAL_NPU_MODELS |
        RA8P1_MODEL_FLAG_VIDEO_VISIBLE_MASK |
        RA8P1_MODEL_FLAG_NO_ACCURACY_CLAIM;
    analysis_apply_detector_result(&frame, &detector_result, inferred);
    analysis_build_display_tile(lane);
    if (g_analysis.synthetic != 0U)
    {
        ipc_bridge_cpu0_display_tile_publish_ex(
            g_display_tile,
            lane->tile_index,
            frame.flags,
            g_analysis.center_index,
            (uint8_t)(lane->tile_index & 0xFFU),
            frame.analysis.tile_count,
            (uint8_t)((lane->tile_index == 0U) ? 0U :
                      ANALYSIS_DISPLAY_OVERLAP_ROWS),
            (uint8_t)((lane->tile_index == 0U) ?
                      RA8P1_DISPLAY_TILE_HEIGHT :
                      ANALYSIS_DISPLAY_OVERLAP_ROWS));
    }
    else
    {
        analysis_publish_ready_display_rows(lane, true);
    }
    ipc_bridge_cpu0_display_publish(&frame, NULL);

    g_analysis.preprocessing_valid =
        preprocess_result == RF_V12_PREPROCESS_READY;
    g_analysis.npu_cycles_last = frame.analysis.npu_cycles;
    g_analysis.stft_cycles_last = frame.analysis.stft_cycles;
    g_analysis.end_to_end_cycles_last = frame.analysis.end_to_end_cycles;
    g_analysis.npu_ready = npu.ready;
    g_analysis.windows_completed++;
    g_last_peak_bin = frame.peak_bin[0];
    g_last_peak_power = frame.peak_power_q16[0];
    g_last_tile_index = lane->tile_index;
}

static void analysis_complete_lanes(void)
{
    for (uint32_t i = 0U; i < 2U; ++i)
    {
        if (g_lanes[i].active != 0U &&
            (g_lanes[i].complete_pending == 0U) &&
            (g_lanes[i].sample_count == ANALYSIS_MODEL_WINDOW_SAMPLES))
        {
            g_lanes[i].complete_cycles = analysis_cycle_now();
            if (g_stft_probe.active != 0U)
            {
                /* Keep the synthetic proof out of analysis_publish_lane().
                 * That production function owns a 2928-byte display frame;
                 * entering it only to return early needlessly consumed that
                 * stack on every proof window. */
                analysis_capture_proof_lane(&g_lanes[i]);
                g_lanes[i].active = 0U;
            }
            else if (g_analysis.synthetic != 0U)
            {
                analysis_publish_lane(&g_lanes[i], true, 0U);
                g_lanes[i].active = 0U;
            }
            else if (g_analysis.expected_tile_count == 1U)
            {
                g_lanes[i].complete_pending = 1U;
            }
            else
            {
                /* Compatibility captures longer than one V12 tile lack a
                 * per-tile whole-window CRC. Publish them as invalid instead
                 * of training a background from unverifiable data. */
                analysis_publish_lane(&g_lanes[i],
                                      false,
                                      RF_V12_TILE_CAPTURE_TIMEOUT);
                g_lanes[i].active = 0U;
            }
        }
    }
}

static void analysis_feed_q15(const q15_t *iq,
                              uint32_t complex_samples,
                              uint64_t sample_index,
                              uint32_t flags)
{
    uint32_t consumed = 0U;
    if (!g_analysis.configured || (iq == NULL) || (complex_samples == 0U)) return;
    if (!g_analysis.sample_index_valid)
    {
        analysis_reset_schedule(sample_index, 0U);
    }
    else if ((sample_index != g_analysis.next_sample_index) ||
             ((flags & RA8P1_IQ_FLAG_DISCONTINUITY) != 0U))
    {
        /* A gap invalidates every tile crossing it.  Resume on a new formal
         * origin; never synthesize missing IQ to complete a tile. */
        g_analysis.discontinuities++;
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            if (g_lanes[i].active != 0U && g_lanes[i].sample_count != 0U)
                g_analysis.partial_windows_dropped++;
        }
        analysis_resync_after_gap(sample_index);
    }
    while (consumed < complex_samples)
    {
        const uint64_t current = sample_index + consumed;
        uint32_t take = complex_samples - consumed;
        if ((g_analysis.expected_tile_count == 0U) ||
            (g_analysis.next_tile_index < g_analysis.expected_tile_count))
        {
            const uint64_t next_start = g_analysis.stream_origin_sample +
                ((uint64_t)g_analysis.next_tile_index * g_analysis.stride_samples);
            if (current == next_start)
            {
                if (analysis_find_lane(g_analysis.next_tile_index) != NULL)
                    g_analysis.next_tile_index++;
                else
                    break;
            }
            else if (next_start > current)
            {
                const uint64_t distance = next_start - current;
                if (distance < take) take = (uint32_t)distance;
            }
        }
        if (take == 0U) continue;
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            analysis_lane_t *lane = &g_lanes[i];
            if ((lane->active != 0U) && (current >= lane->start_sample) &&
                (current < (lane->start_sample + ANALYSIS_MODEL_WINDOW_SAMPLES)))
            {
                uint64_t lane_end = lane->start_sample + ANALYSIS_MODEL_WINDOW_SAMPLES;
                uint64_t available = lane_end - current;
                uint32_t lane_take = (available < take) ? (uint32_t)available : take;
                analysis_lane_feed(lane, &iq[2U * consumed], lane_take);
            }
        }
        consumed += take;
        analysis_complete_lanes();
    }
    g_analysis.next_sample_index = sample_index + complex_samples;
    g_analysis.sample_index_valid = 1U;
}

static uint64_t analysis_now_us(void)
{
    return ((uint64_t)rt_tick_get() * UINT64_C(1000000)) /
           RT_TICK_PER_SECOND;
}

static uint32_t analysis_next_v13_round_index(void)
{
    uint32_t next = g_v13_round_index + 1U;
    if (next == 0U)
    {
        next = 1U;
    }
    g_v13_round_index = next;
    return next;
}

static void analysis_begin_result_round(uint32_t center_index)
{
    const uint32_t center_bit = 1UL << center_index;
    const bool first_center = g_v13_round_center_mask == 0U;
    const bool previous_complete =
        g_v13_round_center_mask == RF_V12_COMPLETE_ROUND_CENTER_MASK;
    const bool expected_next =
        !first_center && !previous_complete &&
        (g_v13_round_last_center < (RF_V12_CENTER_COUNT - 1U)) &&
        (center_index == (g_v13_round_last_center + 1U)) &&
        ((g_v13_round_center_mask & center_bit) == 0U);

    if (first_center || previous_complete || !expected_next)
    {
        if (!first_center && !previous_complete)
        {
            rf_v13_round_builder_flush();
        }
        g_v13_round_center_mask = 0U;
        (void)analysis_next_v13_round_index();
    }
    g_v13_round_last_center = center_index;
    g_analysis.result_round_index = g_v13_round_index;
}

void analysis_pipeline_init(void)
{
    memset(&g_analysis, 0, sizeof(g_analysis));
    g_analysis.source_sample_rate_hz = ANALYSIS_DEFAULT_SAMPLE_RATE;
    g_analysis.sample_rate_hz = ANALYSIS_DEFAULT_SAMPLE_RATE;
    g_analysis.window_samples = ANALYSIS_MODEL_WINDOW_SAMPLES;
    g_analysis.stride_samples = ANALYSIS_MODEL_STRIDE_SAMPLES;
    g_analysis.valid_bits = 12U;
    g_analysis.preprocessing_valid = 0U;
    g_analysis.dwt_epoch = 0U;
    (void)analysis_enable_dwt();
    g_cfft_ready = (arm_cfft_init_1024_q15(&g_cfft) == ARM_MATH_SUCCESS);
    for (uint32_t i = 0U; i < ANALYSIS_FFT_SIZE; ++i)
    {
        const float value = 0.5F - (0.5F * arm_cos_f32((ANALYSIS_TWO_PI * (float)i) /
                                                        (float)ANALYSIS_FFT_SIZE));
        int32_t q = (int32_t)(value * 32767.0F);
        if (q > INT16_MAX) q = INT16_MAX;
        g_window[i] = (q15_t)q;
    }
    rf_v12_preprocess_build_output_map(g_v12_output_map);
    analysis_rebuild_fft_valid_mask();
    g_analysis.configured = g_cfft_ready ? 1U : 0U;
    g_analysis.npu_ready = 0U;
    g_v12_tile_sequence = 0U;
    g_v13_round_index = 0U;
    g_v13_round_center_mask = 0U;
    g_v13_round_last_center = UINT32_MAX;
    g_last_capture_end_time_us = 0U;
    rf_v13_round_builder_init();
    for (uint32_t i = 0U; i < 2U; ++i) analysis_lane_reset(&g_lanes[i]);
}

void analysis_pipeline_configure(uint32_t source_sample_rate_hz,
                                 uint32_t sample_rate_hz,
                                 uint64_t center_frequency_hz,
                                 uint32_t bandwidth_hz,
                                 uint32_t window_samples,
                                 uint32_t valid_bits,
                                 uint32_t flags)
{
    g_analysis.source_sample_rate_hz = (source_sample_rate_hz != 0U) ?
                                       source_sample_rate_hz : ANALYSIS_DEFAULT_SAMPLE_RATE;
    g_analysis.sample_rate_hz = (sample_rate_hz != 0U) ?
                                sample_rate_hz : g_analysis.source_sample_rate_hz;
    g_analysis.center_frequency_hz = center_frequency_hz;
    g_analysis.bandwidth_hz = bandwidth_hz;
    analysis_rebuild_fft_valid_mask();
    /* The model contract is fixed; a caller cannot silently select a 10 ms
     * rate-derived window or a non-overlapping stride. */
    g_analysis.window_samples = ANALYSIS_MODEL_WINDOW_SAMPLES;
    g_analysis.stride_samples = ANALYSIS_MODEL_STRIDE_SAMPLES;
    (void)window_samples;
    g_analysis.valid_bits = (valid_bits == 0U || valid_bits > 16U) ? 16U : valid_bits;
    g_analysis.stream_flags = flags;
    g_analysis.synthetic = (flags & RA8P1_IQ_FLAG_SYNTHETIC) ? 1U : 0U;
    g_analysis.preprocessing_valid =
        (g_analysis.synthetic != 0U) ||
        (g_analysis.center_index < RF_V12_CENTER_COUNT);
    g_analysis.started = 0U;
    g_analysis.sample_index_valid = 0U;
    g_analysis.expected_tile_count = analysis_compute_tile_count(g_analysis.total_samples);
    for (uint32_t i = 0U; i < 2U; ++i) analysis_lane_reset(&g_lanes[i]);
}

void analysis_pipeline_set_session(uint32_t session_id)
{
    g_analysis.session_id = (session_id == 0U) ? 1U : session_id;
    g_analysis.started = 0U;
    g_analysis.sample_index_valid = 0U;
    g_analysis.next_tile_index = 0U;
    g_analysis.windows_completed = 0U;
    g_analysis.stft_frames_total = 0U;
    g_analysis.partial_windows_dropped = 0U;
    g_analysis.discontinuities = 0U;
    g_analysis.discontinuity_pending = 0U;
    g_analysis.log_fallbacks = 0U;
    g_analysis.log_values = 0U;
    g_analysis.capture_result_submitted = 0U;
    g_last_tile_index = 0U;
    for (uint32_t i = 0U; i < 2U; ++i) analysis_lane_reset(&g_lanes[i]);
}

void analysis_pipeline_set_stream_info(uint64_t total_samples, uint32_t center_index)
{
    uint64_t capture_start;
    g_analysis.total_samples = total_samples;
    g_analysis.center_index = center_index;
    g_analysis.expected_tile_count = analysis_compute_tile_count(total_samples);
    g_analysis.capture_result_submitted = 0U;
    if ((g_analysis.synthetic != 0U) ||
        (total_samples < ANALYSIS_MODEL_WINDOW_SAMPLES) ||
        (center_index >= RF_V12_CENTER_COUNT))
    {
        g_analysis.result_round_index = 0U;
        return;
    }
    analysis_begin_result_round(center_index);
    capture_start = analysis_now_us();
    if (capture_start < g_last_capture_end_time_us)
    {
        capture_start = g_last_capture_end_time_us;
    }
    g_analysis.capture_start_time_us = capture_start;
    g_analysis.capture_end_time_us =
        capture_start + analysis_tile_duration_us();
    g_analysis.preprocessing_valid = 1U;
}

void analysis_pipeline_set_queue(uint32_t queue_depth, uint32_t ingress_drops)
{
    g_analysis.queue_depth = queue_depth;
    g_analysis.ingress_drops = ingress_drops;
}

void analysis_pipeline_ingest_s16(const int16_t *iq,
                                  uint32_t complex_samples,
                                  uint64_t sample_index,
                                  uint32_t flags)
{
    /* One conversion block covers the largest ring payload.  A standard
     * 1440-byte IQ datagram therefore enters the scheduler in one pass. */
    q15_t local[ANALYSIS_INGEST_S16_SCALARS];
    uint32_t consumed = 0U;
    if (iq == NULL) return;
    if (g_analysis.valid_bits == 0U) g_analysis.valid_bits = 12U;
    while (consumed < complex_samples)
    {
        uint32_t take = complex_samples - consumed;
        if (take > (sizeof(local) / (2U * sizeof(local[0]))))
            take = sizeof(local) / (2U * sizeof(local[0]));
        const uint32_t scalar_count = take * 2U;
        if (g_analysis.valid_bits == 12U)
        {
            analysis_convert_s12_q15(&iq[consumed * 2U], local, scalar_count);
        }
        else if (g_analysis.valid_bits == 16U)
        {
            memcpy(local, &iq[consumed * 2U], scalar_count * sizeof(local[0]));
        }
        else
        {
            for (uint32_t i = 0U; i < scalar_count; ++i)
                local[i] = analysis_to_q15(iq[(consumed * 2U) + i]);
        }
        analysis_feed_q15(local, take, sample_index + consumed, flags);
        flags &= ~RA8P1_IQ_FLAG_DISCONTINUITY;
        consumed += take;
    }
}

void analysis_pipeline_ingest_q15(const int16_t *iq,
                                  uint32_t complex_samples,
                                  uint64_t sample_index,
                                  uint32_t flags)
{
    if (iq == NULL)
    {
        return;
    }
    analysis_feed_q15((const q15_t *)iq,
                      complex_samples,
                      sample_index,
                      flags);
}

void analysis_pipeline_ingest_s8(const int8_t *iq,
                                 uint32_t complex_samples,
                                 uint64_t sample_index,
                                 uint32_t flags)
{
    q15_t local[512U];
    uint32_t consumed = 0U;
    if (iq == NULL) return;
    g_analysis.valid_bits = 8U;
    while (consumed < complex_samples)
    {
        uint32_t take = complex_samples - consumed;
        if (take > (sizeof(local) / (2U * sizeof(local[0]))))
            take = sizeof(local) / (2U * sizeof(local[0]));
        for (uint32_t i = 0U; i < take * 2U; ++i)
            local[i] = (q15_t)((int16_t)iq[(consumed * 2U) + i] * 256);
        analysis_feed_q15(local, take, sample_index + consumed, flags);
        flags &= ~RA8P1_IQ_FLAG_DISCONTINUITY;
        consumed += take;
    }
}

void analysis_pipeline_synthetic_step(uint32_t complex_samples)
{
    /* Retained as an explicit test hook.  rf_pipeline never calls it unless
     * a future build explicitly enables synthetic mode. */
    q15_t local[512U];
    static uint32_t phase;
    uint32_t produced = 0U;
    while (produced < complex_samples)
    {
        uint32_t count = complex_samples - produced;
        if (count > (sizeof(local) / (2U * sizeof(local[0]))))
            count = sizeof(local) / (2U * sizeof(local[0]));
        for (uint32_t i = 0U; i < count; ++i)
        {
            const uint32_t p = phase++;
            const int32_t tone = (int32_t)((p * 997U) & 0xFFFFU) - 32768;
            local[2U * i] = (q15_t)(tone / 8);
            local[(2U * i) + 1U] = (q15_t)(tone / 12);
        }
        analysis_feed_q15(local, count, g_analysis.next_sample_index,
                          RA8P1_IQ_FLAG_SYNTHETIC);
        produced += count;
    }
}

static void analysis_stft_proof_sort(uint32_t values[ANALYSIS_STFT_PROOF_RUNS])
{
    for (uint32_t i = 1U; i < ANALYSIS_STFT_PROOF_RUNS; ++i)
    {
        const uint32_t value = values[i];
        uint32_t j = i;
        while ((j > 0U) && (values[j - 1U] > value))
        {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = value;
    }
}

static void analysis_stft_proof_summarize(
    volatile analysis_stft_proof_series_t *series)
{
    uint32_t sorted[ANALYSIS_STFT_PROOF_RUNS];
    for (uint32_t i = 0U; i < ANALYSIS_STFT_PROOF_RUNS; ++i)
    {
        sorted[i] = series->samples[i];
    }
    analysis_stft_proof_sort(sorted);
    series->minimum_cycles = sorted[0];
    series->median_cycles = sorted[ANALYSIS_STFT_PROOF_RUNS / 2U];
    series->maximum_cycles = sorted[ANALYSIS_STFT_PROOF_RUNS - 1U];
}

static void analysis_stft_proof_prepare_run(uint32_t session_id)
{
    memset(&g_stft_probe, 0, sizeof(g_stft_probe));
    analysis_pipeline_configure(ANALYSIS_FORMAL_SAMPLE_RATE_HZ,
                                ANALYSIS_FORMAL_SAMPLE_RATE_HZ,
                                2420000000ULL,
                                ANALYSIS_FORMAL_BANDWIDTH_HZ,
                                ANALYSIS_MODEL_WINDOW_SAMPLES,
                                12U,
                                RA8P1_IQ_FLAG_VALID_BITS_12 |
                                RA8P1_IQ_FLAG_SYNTHETIC);
    analysis_pipeline_set_session(session_id);
    analysis_pipeline_set_stream_info(ANALYSIS_MODEL_WINDOW_SAMPLES, 0U);
    g_stft_probe.active = 1U;
}

static bool analysis_stft_proof_run_once(uint32_t session_id,
                                         uint32_t *full_cycles)
{
    uint32_t start_cycles;
    uint32_t sample_index = 0U;

    analysis_stft_proof_prepare_run(session_id);
    start_cycles = analysis_cycle_now();
    while (sample_index < ANALYSIS_MODEL_WINDOW_SAMPLES)
    {
        analysis_pipeline_ingest_s16(g_stft_proof_input,
                                     ANALYSIS_STFT_PROOF_BLOCK_SAMPLES,
                                     sample_index,
                                     RA8P1_IQ_FLAG_SYNTHETIC);
        sample_index += ANALYSIS_STFT_PROOF_BLOCK_SAMPLES;
    }
    g_stft_probe.active = 0U;
    if ((g_stft_probe.completed == 0U) ||
        (g_stft_probe.complete_cycles == 0U) ||
        (g_stft_probe.stft_frames != ANALYSIS_STFT_FRAMES_PER_TILE))
    {
        return false;
    }
    *full_cycles = g_stft_probe.complete_cycles - start_cycles;
    return (*full_cycles >= g_stft_probe.stft_hot_cycles);
}

int analysis_pipeline_run_stft_proof(void)
{
    uint32_t warmup_cycles = 0U;
    uint32_t reference_checksum;
    uint32_t status = ANALYSIS_STFT_PROOF_STATUS_PASS;

    memset((void *)&g_analysis_stft_proof, 0, sizeof(g_analysis_stft_proof));
    g_analysis_stft_proof.magic = ANALYSIS_STFT_PROOF_MAGIC;
    g_analysis_stft_proof.version = ANALYSIS_STFT_PROOF_VERSION;
    g_analysis_stft_proof.status = ANALYSIS_STFT_PROOF_STATUS_RUNNING;
    g_analysis_stft_proof.flags =
        ANALYSIS_STFT_PROOF_FLAG_SYNTHETIC |
        ANALYSIS_STFT_PROOF_FLAG_INPUT_GEN_EXCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_NETWORK_EXCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_CRC_EXCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_NPU_EXCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_IPC_EXCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_PRODUCTION_STFT_PATH |
        ANALYSIS_STFT_PROOF_FLAG_BOOT_ONCE |
        ANALYSIS_STFT_PROOF_FLAG_S16_CONVERT_INCLUDED |
        ANALYSIS_STFT_PROOF_FLAG_STAGE_PROBES_INCLUDED;
    g_analysis_stft_proof.core_clock_hz = SystemCoreClock;
    g_analysis_stft_proof.warmup_runs = 1U;
    g_analysis_stft_proof.measured_runs = ANALYSIS_STFT_PROOF_RUNS;
    g_analysis_stft_proof.complex_samples = ANALYSIS_MODEL_WINDOW_SAMPLES;
    g_analysis_stft_proof.input_format = RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED;
    g_analysis_stft_proof.valid_bits = 12U;
    g_analysis_stft_proof.fft_size = ANALYSIS_FFT_SIZE;
    g_analysis_stft_proof.hop_size = ANALYSIS_HOP_SIZE;
    g_analysis_stft_proof.stft_frames = ANALYSIS_STFT_FRAMES_PER_TILE;
    /* Zero denotes the V12 exact-area 1024->204 reducer; it is not an
     * integer pool width. The explicit 256/51 contract lives in the ELF
     * constants and shared ABI header. */
    g_analysis_stft_proof.frequency_pool = 0U;
    g_analysis_stft_proof.time_pool = ANALYSIS_TIME_POOL;
    g_analysis_stft_proof.input_block_samples = ANALYSIS_STFT_PROOF_BLOCK_SAMPLES;

    for (uint32_t i = 0U; i < ANALYSIS_STFT_PROOF_BLOCK_SAMPLES; ++i)
    {
        g_stft_proof_input[2U * i] =
            (int16_t)(((i * 97U + 31U) & 0x0FFFU) - 2048);
        g_stft_proof_input[(2U * i) + 1U] =
            (int16_t)(((i * 193U + 17U) & 0x0FFFU) - 2048);
    }

    if (!g_cfft_ready)
    {
        status = ANALYSIS_STFT_PROOF_STATUS_INIT_ERROR;
        goto proof_done;
    }
    if (!analysis_enable_dwt())
    {
        status = ANALYSIS_STFT_PROOF_STATUS_DWT_ERROR;
        goto proof_done;
    }
    if (!analysis_stft_proof_run_once(0x53540000U, &warmup_cycles))
    {
        status = ANALYSIS_STFT_PROOF_STATUS_RUN_ERROR;
        goto proof_done;
    }
    reference_checksum = g_stft_probe.checksum;

    for (uint32_t run = 0U; run < ANALYSIS_STFT_PROOF_RUNS; ++run)
    {
        uint32_t full_cycles;
        if (!analysis_stft_proof_run_once(0x53540001U + run, &full_cycles))
        {
            status = ANALYSIS_STFT_PROOF_STATUS_RUN_ERROR;
            break;
        }
        g_analysis_stft_proof.full_window.samples[run] = full_cycles;
        g_analysis_stft_proof.stft_hot.samples[run] =
            g_stft_probe.stft_hot_cycles;
        g_analysis_stft_proof.window_apply.samples[run] =
            g_stft_probe.window_cycles;
        g_analysis_stft_proof.fft.samples[run] = g_stft_probe.fft_cycles;
        g_analysis_stft_proof.power_reduce.samples[run] =
            g_stft_probe.reduce_cycles;
        g_analysis_stft_proof.pool_and_quantize.samples[run] =
            g_stft_probe.pool_cycles;
        g_analysis_stft_proof.ingest_and_schedule.samples[run] =
            full_cycles - g_stft_probe.stft_hot_cycles;
        if (g_stft_probe.checksum != reference_checksum)
        {
            g_analysis_stft_proof.checksum_mismatches++;
        }
        if (g_stft_probe.stft_frames != ANALYSIS_STFT_FRAMES_PER_TILE)
        {
            g_analysis_stft_proof.frame_mismatches++;
        }
        g_analysis_stft_proof.peak_bin = g_stft_probe.peak_bin;
        g_analysis_stft_proof.peak_power = g_stft_probe.peak_power;
    }

    if ((status == ANALYSIS_STFT_PROOF_STATUS_PASS) &&
        ((g_analysis_stft_proof.checksum_mismatches != 0U) ||
         (g_analysis_stft_proof.frame_mismatches != 0U)))
    {
        status = ANALYSIS_STFT_PROOF_STATUS_CHECK_ERROR;
    }
    if (status == ANALYSIS_STFT_PROOF_STATUS_PASS)
    {
        analysis_stft_proof_summarize(&g_analysis_stft_proof.full_window);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.stft_hot);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.window_apply);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.fft);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.power_reduce);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.pool_and_quantize);
        analysis_stft_proof_summarize(&g_analysis_stft_proof.ingest_and_schedule);
        g_analysis_stft_proof.checksum = reference_checksum;
    }

proof_done:
    g_stft_probe.active = 0U;
    g_analysis_stft_proof.cfsr = SCB->CFSR;
    g_analysis_stft_proof.hfsr = SCB->HFSR;
    if ((status == ANALYSIS_STFT_PROOF_STATUS_PASS) &&
        ((g_analysis_stft_proof.cfsr != 0U) ||
         (g_analysis_stft_proof.hfsr != 0U)))
    {
        status = ANALYSIS_STFT_PROOF_STATUS_FAULT;
    }
    g_analysis_stft_proof.status = status;
    __DMB();
    g_analysis_stft_proof.completion_magic = ANALYSIS_STFT_PROOF_DONE_MAGIC;
    return (status == ANALYSIS_STFT_PROOF_STATUS_PASS) ? 0 : -1;
}

static analysis_lane_t *analysis_pending_lane(void)
{
    for (uint32_t index = 0U; index < 2U; ++index)
    {
        if ((g_lanes[index].active != 0U) &&
            (g_lanes[index].complete_pending != 0U))
        {
            return &g_lanes[index];
        }
    }
    return NULL;
}

static void analysis_submit_invalid_capture(uint8_t tile_flags)
{
    rf_v12_detector_input_t input;
    rf_v12_detector_result_t result;
    if ((g_analysis.synthetic != 0U) ||
        (g_analysis.capture_result_submitted != 0U) ||
        (g_analysis.center_index >= RF_V12_CENTER_COUNT) ||
        (g_analysis.result_round_index == 0U))
    {
        return;
    }
    if (tile_flags == 0U)
    {
        tile_flags = RF_V12_TILE_CAPTURE_TIMEOUT;
    }
    memset(&input, 0, sizeof(input));
    input.tile_sequence = analysis_take_v12_tile_sequence();
    input.round_index = g_analysis.result_round_index;
    input.center_index = g_analysis.center_index;
    input.center_frequency_hz = g_analysis.center_frequency_hz;
    input.capture_start_time_us = g_analysis.capture_start_time_us;
    input.capture_end_time_us = g_analysis.capture_end_time_us;
    input.background_generation = 0U;
    input.sdr_gain_db_q8 = 0;
    input.tile_validity = RF_V12_TILE_INVALID;
    input.tile_flags = tile_flags;
    input.c0_db = NULL;
    input.model_input = NULL;
    rf_v12_detector_decode(&input, &result);
    (void)rf_v13_round_builder_submit(&result.tile);
    g_v13_round_center_mask |= 1UL << g_analysis.center_index;
    g_analysis.capture_result_submitted = 1U;
    if (g_analysis.capture_end_time_us > g_last_capture_end_time_us)
    {
        g_last_capture_end_time_us = g_analysis.capture_end_time_us;
    }
}

bool analysis_pipeline_window_ready(void)
{
    return analysis_pending_lane() != NULL;
}

bool analysis_pipeline_commit_stream(void)
{
    analysis_lane_t *lane = analysis_pending_lane();
    if (lane == NULL)
    {
        return false;
    }
    lane->complete_pending = 0U;
    analysis_publish_lane(lane, true, 0U);
    lane->active = 0U;
    return g_analysis.capture_result_submitted != 0U;
}

void analysis_pipeline_reject_stream(uint8_t tile_flags)
{
    analysis_lane_t *lane = analysis_pending_lane();
    if (lane != NULL)
    {
        lane->complete_pending = 0U;
        analysis_publish_lane(lane, false, tile_flags);
        lane->active = 0U;
    }
    else
    {
        analysis_submit_invalid_capture(tile_flags);
    }
}

void analysis_pipeline_finish_stream(void)
{
    if (analysis_pending_lane() != NULL)
    {
        analysis_pipeline_reject_stream(RF_V12_TILE_CAPTURE_TIMEOUT);
    }
    for (uint32_t index = 0U; index < 2U; ++index)
    {
        analysis_lane_reset(&g_lanes[index]);
    }
    g_analysis.started = 0U;
    g_analysis.sample_index_valid = 0U;
}

void analysis_pipeline_abort_stream(void)
{
    if (analysis_pending_lane() != NULL)
    {
        analysis_pipeline_reject_stream(RF_V12_TILE_CAPTURE_TIMEOUT);
    }
    else
    {
        analysis_submit_invalid_capture(RF_V12_TILE_CAPTURE_TIMEOUT);
    }
    for (uint32_t i = 0U; i < 2U; ++i)
    {
        if (g_lanes[i].active != 0U && g_lanes[i].sample_count != 0U)
            g_analysis.partial_windows_dropped++;
        analysis_lane_reset(&g_lanes[i]);
    }
    rf_v13_round_builder_flush();
    g_v13_round_center_mask = 0U;
    g_v13_round_last_center = UINT32_MAX;
    g_analysis.started = 0U;
    g_analysis.sample_index_valid = 0U;
}

void analysis_pipeline_copy_display_tile(uint8_t *destination, uint32_t destination_bytes)
{
    if ((destination != NULL) && (destination_bytes >= sizeof(g_display_tile)))
        memcpy(destination, g_display_tile, sizeof(g_display_tile));
}

void analysis_pipeline_get_stats(analysis_stats_t *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    stats->window_sequence = g_last_tile_index;
    stats->windows_completed = g_analysis.windows_completed;
    stats->stft_frames = g_analysis.stft_frames_total;
    stats->stft_cycles = g_analysis.stft_cycles_last;
    stats->npu_cycles = g_analysis.npu_cycles_last;
    stats->end_to_end_cycles = g_analysis.end_to_end_cycles_last;
    stats->peak_bin = g_last_peak_bin;
    stats->peak_power_q16 = g_last_peak_power;
    stats->sample_rate_hz = g_analysis.sample_rate_hz;
    stats->source_sample_rate_hz = g_analysis.source_sample_rate_hz;
    stats->window_samples = g_analysis.window_samples;
    stats->queue_depth = g_analysis.queue_depth;
    stats->ingress_drops = g_analysis.ingress_drops;
    stats->npu_ready = g_analysis.npu_ready;
    stats->synthetic = g_analysis.synthetic;
    stats->partial_windows_dropped = g_analysis.partial_windows_dropped;
    stats->discontinuities = g_analysis.discontinuities;
    stats->tile_count = g_analysis.expected_tile_count;
    stats->center_index = g_analysis.center_index;
    stats->preprocessing_valid = g_analysis.preprocessing_valid;
    stats->log_fallbacks = g_analysis.log_fallbacks;
    stats->log_values = g_analysis.log_values;
}
