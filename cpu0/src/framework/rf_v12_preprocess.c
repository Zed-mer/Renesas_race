#include "rf_v12_preprocess.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define RF_V12_DB_PER_LOG2_POWER     (3.010299956639812F)
#define RF_V12_POWER_EPSILON         (1.0e-12F)
#define RF_V12_LOG2_INV_LN2          (1.44269504088896340736F)
#define RF_V12_LOG_FALLBACK_MARGIN_DB (2.0e-4F)
#define RF_V12_BACKGROUND_IQR_LIMIT_Q8_8       (2 * 256)
#define RF_V12_VALIDATION_MEDIAN_LIMIT_Q8_8    (256 / 2)
#define RF_V12_VALIDATION_RESIDUAL_LIMIT_Q8_8  (3 * 256)
#define RF_V12_VALIDATION_REQUIRED_NUMERATOR   (9U)
#define RF_V12_VALIDATION_REQUIRED_DENOMINATOR (10U)

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE > 0)
#include "arm_math.h"
#if defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)
#include "arm_vec_math.h"
#define RF_V12_HAS_MVEF (1)
#else
#define RF_V12_HAS_MVEF (0)
#endif
#else
#define RF_V12_HAS_MVEF (0)
#endif

#if defined(__GNUC__) && (defined(__arm__) || defined(__thumb__))
#define RF_V12_HOT_CODE __attribute__((section(".itcm_code_from_flash")))
#else
#define RF_V12_HOT_CODE
#endif

_Static_assert((RF_V12_FFT_POINTS * RF_V12_REBIN_RAW_UNITS) ==
               (RF_V12_FEATURE_FREQUENCY_BINS *
                RF_V12_REBIN_OUTPUT_UNITS),
               "1024-to-204 exact-area ratio changed");
_Static_assert((RF_V12_RAW_STFT_FRAMES -
                (2U * RF_V12_STFT_EDGE_CROP_FRAMES)) ==
               (RF_V12_FEATURE_TIME_BINS * RF_V12_TIME_POOL_FRAMES),
               "V12 crop/time-pool contract changed");
_Static_assert(RF_V12_FEATURE_FREQUENCY_BINS < UINT8_MAX,
               "rebin output index no longer fits the cached map");
_Static_assert(RF_V12_PREPROCESS_BACKGROUND_WINDOWS == 16U,
               "quartile indices require 16 startup windows");
_Static_assert((RF_V12_FEATURE_FREQUENCY_BINS % 4U) == 0U,
               "MVE frequency reducer requires complete four-lane groups");
_Static_assert(RF_V12_REBIN_OUTPUT_UNITS ==
               ((5U * RF_V12_REBIN_RAW_UNITS) + 1U),
               "gathered reducer no longer spans exactly six raw bins");

static const float g_rf_v12_clip_min[RF_V12_FEATURE_CHANNELS] =
{
    RF_V12_C0_CLIP_MIN,
    RF_V12_C1_CLIP_MIN,
    RF_V12_C2_CLIP_MIN,
    RF_V12_C3_CLIP_MIN
};

static const float g_rf_v12_clip_max[RF_V12_FEATURE_CHANNELS] =
{
    RF_V12_C0_CLIP_MAX,
    RF_V12_C1_CLIP_MAX,
    RF_V12_C2_CLIP_MAX,
    RF_V12_C3_CLIP_MAX
};

static const float g_rf_v12_mean[RF_V12_FEATURE_CHANNELS] =
{
    RF_V12_C0_MEAN,
    RF_V12_C1_MEAN,
    RF_V12_C2_MEAN,
    RF_V12_C3_MEAN
};

static const float g_rf_v12_std[RF_V12_FEATURE_CHANNELS] =
{
    RF_V12_C0_STD,
    RF_V12_C1_STD,
    RF_V12_C2_STD,
    RF_V12_C3_STD
};

int32_t rf_v12_preprocess_round_to_nearest_even(float value)
{
    int32_t lower;
    float fraction;

    if (value >= (float)INT32_MAX)
    {
        return INT32_MAX;
    }
    if (value <= (float)INT32_MIN)
    {
        return INT32_MIN;
    }

    lower = (int32_t)value;
    if (value < (float)lower)
    {
        --lower;
    }
    fraction = value - (float)lower;
    if ((fraction > 0.5F) ||
        ((fraction == 0.5F) && ((lower & 1) != 0)))
    {
        ++lower;
    }
    return lower;
}

int8_t rf_v12_preprocess_quantize(float value, uint32_t channel)
{
    float clipped;
    float normalized;
    float quantized;
    int32_t integer_value;

    if (channel >= RF_V12_FEATURE_CHANNELS)
    {
        return (int8_t)RF_V12_INPUT_ZERO_POINT;
    }
    clipped = value;
    if (!(clipped >= g_rf_v12_clip_min[channel]))
    {
        clipped = g_rf_v12_clip_min[channel];
    }
    else if (clipped > g_rf_v12_clip_max[channel])
    {
        clipped = g_rf_v12_clip_max[channel];
    }
    normalized = (clipped - g_rf_v12_mean[channel]) /
                 g_rf_v12_std[channel];
    quantized = (normalized / RF_V12_INPUT_SCALE) +
                (float)RF_V12_INPUT_ZERO_POINT;
    integer_value = rf_v12_preprocess_round_to_nearest_even(quantized);
    if (integer_value > INT8_MAX)
    {
        integer_value = INT8_MAX;
    }
    else if (integer_value < INT8_MIN)
    {
        integer_value = INT8_MIN;
    }
    return (int8_t)integer_value;
}

static float rf_v12_power_db(uint64_t weighted_power,
                             uint32_t divisor,
                             float linear_power_scale)
{
    float power;
    if ((weighted_power == 0U) || (divisor == 0U) ||
        !(linear_power_scale > 0.0F))
    {
        return 10.0F * log10f(RF_V12_POWER_EPSILON);
    }
    power = ((float)weighted_power / (float)divisor) *
            linear_power_scale;
    return log2f(power + RF_V12_POWER_EPSILON) *
           RF_V12_DB_PER_LOG2_POWER;
}

static bool rf_v12_power_near_quantization_boundary(float value,
                                                    uint32_t channel)
{
    float clipped;
    float scaled;
    float lower;
    float margin;

    if (channel >= RF_V12_FEATURE_CHANNELS ||
        !(value == value))
    {
        return true;
    }
    margin = RF_V12_LOG_FALLBACK_MARGIN_DB /
             (g_rf_v12_std[channel] * RF_V12_INPUT_SCALE);
    clipped = value;
    if (clipped < g_rf_v12_clip_min[channel])
    {
        return (g_rf_v12_clip_min[channel] - clipped) <=
               RF_V12_LOG_FALLBACK_MARGIN_DB;
    }
    if (clipped > g_rf_v12_clip_max[channel])
    {
        return (clipped - g_rf_v12_clip_max[channel]) <=
               RF_V12_LOG_FALLBACK_MARGIN_DB;
    }
    scaled = (((clipped - g_rf_v12_mean[channel]) /
               g_rf_v12_std[channel]) /
              RF_V12_INPUT_SCALE) +
             (float)RF_V12_INPUT_ZERO_POINT;
    lower = floorf(scaled);
    return fabsf((scaled - lower) - 0.5F) <= margin;
}

#if RF_V12_HAS_MVEF
static RF_V12_HOT_CODE void rf_v12_power_db_max_mve4(
    const uint64_t *weighted_power,
    uint32_t divisor,
    float linear_power_scale,
    float destination[4])
{
    float input[4] __attribute__((aligned(16)));
    uint32_t invalid = 0U;

    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if ((weighted_power[i] == 0U) ||
            (divisor == 0U) ||
            !(linear_power_scale > 0.0F))
        {
            input[i] = RF_V12_POWER_EPSILON;
            invalid |= 1UL << i;
        }
        else
        {
            input[i] = ((float)weighted_power[i] / (float)divisor) *
                       linear_power_scale + RF_V12_POWER_EPSILON;
        }
    }

    {
        f32x4_t logs = vlogq_f32(vld1q(input));
        logs = vmulq_n_f32(
            logs,
            RF_V12_LOG2_INV_LN2 * RF_V12_DB_PER_LOG2_POWER);
        vst1q(destination, logs);
    }
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        if ((invalid & (1UL << i)) != 0U)
        {
            destination[i] = rf_v12_power_db(
                weighted_power[i], divisor, linear_power_scale);
        }
    }
}
#endif

void rf_v12_preprocess_tile_reset(rf_v12_preprocess_tile_t *tile,
                                  int8_t *feature_staging)
{
    if (tile == NULL)
    {
        return;
    }
    memset(tile->frame_weighted_power, 0,
           sizeof(tile->frame_weighted_power));
    memset(tile->pool_weighted_power_sum, 0,
           sizeof(tile->pool_weighted_power_sum));
    memset(tile->pool_weighted_power_max, 0,
           sizeof(tile->pool_weighted_power_max));
    tile->feature_staging = feature_staging;
    tile->raw_frame_index = 0U;
    tile->time_bin = 0U;
    tile->pool_frame_count = 0U;
    tile->frame_open = 0U;
}

void rf_v12_preprocess_build_rebin_map(
    rf_v12_rebin_map_t map[RF_V12_FFT_POINTS])
{
    if (map == NULL)
    {
        return;
    }
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FFT_POINTS;
         ++frequency)
    {
        const uint32_t raw_start = frequency * RF_V12_REBIN_RAW_UNITS;
        const uint32_t raw_end = raw_start + RF_V12_REBIN_RAW_UNITS;
        const uint32_t first_output = raw_start / RF_V12_REBIN_OUTPUT_UNITS;
        const uint32_t first_end =
            (first_output + 1U) * RF_V12_REBIN_OUTPUT_UNITS;
        const uint32_t first_weight =
            ((raw_end < first_end) ? raw_end : first_end) - raw_start;

        map[frequency].first_output = (uint8_t)first_output;
        map[frequency].first_weight = (uint8_t)first_weight;
    }
}

void rf_v12_preprocess_build_output_map(
    rf_v12_rebin_output_map_t map[RF_V12_FEATURE_FREQUENCY_BINS])
{
    if (map == NULL)
    {
        return;
    }
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        const uint32_t output_start =
            frequency * RF_V12_REBIN_OUTPUT_UNITS;
        const uint32_t first_input =
            output_start / RF_V12_REBIN_RAW_UNITS;
        const uint32_t first_offset =
            output_start % RF_V12_REBIN_RAW_UNITS;
        const uint32_t first_weight =
            RF_V12_REBIN_RAW_UNITS - first_offset;
        const uint32_t last_weight =
            RF_V12_REBIN_OUTPUT_UNITS - first_weight -
            (4U * RF_V12_REBIN_RAW_UNITS);

        map[frequency].first_input = (uint16_t)first_input;
        map[frequency].first_weight = (uint8_t)first_weight;
        map[frequency].last_weight = (uint8_t)last_weight;
    }
}

static bool rf_v12_preprocess_frame_open(
    rf_v12_preprocess_tile_t *tile,
    uint32_t raw_frame_index,
    bool clear_scatter_buffer)
{
    if ((tile == NULL) || (tile->frame_open != 0U) ||
        (raw_frame_index >= RF_V12_RAW_STFT_FRAMES) ||
        (raw_frame_index != tile->raw_frame_index))
    {
        return false;
    }
    if (clear_scatter_buffer)
    {
        memset(tile->frame_weighted_power, 0,
               sizeof(tile->frame_weighted_power));
    }
    tile->frame_open = 1U;
    return true;
}

bool rf_v12_preprocess_frame_begin(rf_v12_preprocess_tile_t *tile,
                                   uint32_t raw_frame_index)
{
    return rf_v12_preprocess_frame_open(tile, raw_frame_index, true);
}

static inline __attribute__((always_inline)) void
rf_v12_accumulate_power_bin_unchecked(
    rf_v12_preprocess_tile_t *tile,
    uint32_t shifted_frequency_bin,
    uint32_t linear_power)
{
    uint32_t raw_start;
    uint32_t raw_end;
    uint32_t first_output;
    uint32_t first_end;
    uint32_t first_weight;

    /* Boundaries are represented in 1/51 of a raw FFT bin. One output cell
     * is exactly 256 such units, so a raw bin can touch at most two outputs. */
    raw_start = shifted_frequency_bin * RF_V12_REBIN_RAW_UNITS;
    raw_end = raw_start + RF_V12_REBIN_RAW_UNITS;
    first_output = raw_start / RF_V12_REBIN_OUTPUT_UNITS;
    first_end = (first_output + 1U) * RF_V12_REBIN_OUTPUT_UNITS;
    first_weight = ((raw_end < first_end) ? raw_end : first_end) - raw_start;
    {
        const rf_v12_rebin_map_t map =
        {
            (uint8_t)first_output,
            (uint8_t)first_weight
        };
        rf_v12_preprocess_power_bin_mapped(tile, &map, linear_power);
    }
}

RF_V12_HOT_CODE void rf_v12_preprocess_power_bin(
    rf_v12_preprocess_tile_t *tile,
    uint32_t shifted_frequency_bin,
    uint32_t linear_power)
{
    if ((tile == NULL) || (tile->frame_open == 0U) ||
        (shifted_frequency_bin >= RF_V12_FFT_POINTS) ||
        (tile->raw_frame_index < RF_V12_STFT_EDGE_CROP_FRAMES) ||
        (tile->raw_frame_index >=
         (RF_V12_RAW_STFT_FRAMES - RF_V12_STFT_EDGE_CROP_FRAMES)))
    {
        return;
    }
    rf_v12_accumulate_power_bin_unchecked(tile,
                                          shifted_frequency_bin,
                                           linear_power);
}

static RF_V12_HOT_CODE inline void rf_v12_finalize_pool_frequency(
    rf_v12_preprocess_tile_t *tile,
    uint32_t frequency,
    uint64_t sum,
    uint64_t maximum,
    float linear_power_scale,
    float maximum_db,
    bool maximum_is_approximate)
{
    const uint32_t time = tile->time_bin;
    const uint32_t cell =
        (frequency * RF_V12_FEATURE_TIME_BINS) + time;
    const uint32_t feature = cell * RF_V12_FEATURE_CHANNELS;
    const float mean_db = rf_v12_power_db(
        sum,
        RF_V12_REBIN_OUTPUT_UNITS * RF_V12_TIME_POOL_FRAMES,
        linear_power_scale);
    float c1;

    /* Only C1 uses the vector approximation. C0 remains scalar so background
     * calibration and all spatial/temporal derivatives retain their exact
     * scalar values. Near a quantization boundary, recompute the one scalar
     * logarithm and keep the model input byte-identical. */
    c1 = maximum_db - mean_db;
    if (maximum_is_approximate &&
        rf_v12_power_near_quantization_boundary(c1, 1U))
    {
        maximum_db = rf_v12_power_db(
            maximum, RF_V12_REBIN_OUTPUT_UNITS, linear_power_scale);
        c1 = maximum_db - mean_db;
    }
    if (!(c1 > 0.0F))
    {
        c1 = 0.0F;
    }
    tile->c0_db[cell] = mean_db;
    if (tile->feature_staging != NULL)
    {
        tile->feature_staging[feature + 1U] =
            rf_v12_preprocess_quantize(c1, 1U);
    }
}

static RF_V12_HOT_CODE void rf_v12_finish_pool_frame(
    rf_v12_preprocess_tile_t *tile,
    float linear_power_scale)
{
    tile->pool_frame_count++;
    if (tile->pool_frame_count != RF_V12_TIME_POOL_FRAMES)
    {
        return;
    }

#if RF_V12_HAS_MVEF
    for (uint32_t base = 0U;
         base < RF_V12_FEATURE_FREQUENCY_BINS;
         base += 4U)
    {
        uint64_t sums[4] __attribute__((aligned(16)));
        uint64_t maxima[4] __attribute__((aligned(16)));
        float maximum_db[4] __attribute__((aligned(16)));
        for (uint32_t index = 0U; index < 4U; ++index)
        {
            sums[index] =
                tile->pool_weighted_power_sum[base + index];
            maxima[index] =
                tile->pool_weighted_power_max[base + index];
        }
        rf_v12_power_db_max_mve4(
            maxima,
            RF_V12_REBIN_OUTPUT_UNITS,
            linear_power_scale,
            maximum_db);
        for (uint32_t index = 0U; index < 4U; ++index)
        {
            rf_v12_finalize_pool_frequency(
                tile,
                base + index,
                sums[index],
                maxima[index],
                linear_power_scale,
                maximum_db[index],
                true);
        }
    }
#else
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        rf_v12_finalize_pool_frequency(
            tile,
            frequency,
            tile->pool_weighted_power_sum[frequency],
            tile->pool_weighted_power_max[frequency],
            linear_power_scale,
            rf_v12_power_db(
                tile->pool_weighted_power_max[frequency],
                RF_V12_REBIN_OUTPUT_UNITS,
                linear_power_scale),
            false);
    }
#endif
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        tile->pool_weighted_power_sum[frequency] = 0U;
        tile->pool_weighted_power_max[frequency] = 0U;
    }
    tile->pool_frame_count = 0U;
    tile->time_bin++;
}

RF_V12_HOT_CODE bool rf_v12_preprocess_frame_end(
    rf_v12_preprocess_tile_t *tile,
    float linear_power_scale)
{
    const bool retained =
        (tile != NULL) &&
        (tile->raw_frame_index >= RF_V12_STFT_EDGE_CROP_FRAMES) &&
        (tile->raw_frame_index <
         (RF_V12_RAW_STFT_FRAMES - RF_V12_STFT_EDGE_CROP_FRAMES));

    if ((tile == NULL) || (tile->frame_open == 0U))
    {
        return false;
    }
    if (retained)
    {
        if ((tile->time_bin >= RF_V12_FEATURE_TIME_BINS) ||
            (tile->pool_frame_count >= RF_V12_TIME_POOL_FRAMES))
        {
            tile->frame_open = 0U;
            return false;
        }
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FEATURE_FREQUENCY_BINS;
             ++frequency)
        {
            const uint64_t weighted =
                tile->frame_weighted_power[frequency];
            tile->pool_weighted_power_sum[frequency] += weighted;
            if (weighted > tile->pool_weighted_power_max[frequency])
            {
                tile->pool_weighted_power_max[frequency] = weighted;
            }
        }
        rf_v12_finish_pool_frame(tile, linear_power_scale);
    }
    tile->frame_open = 0U;
    tile->raw_frame_index++;
    return true;
}

RF_V12_HOT_CODE bool rf_v12_preprocess_frame(
    rf_v12_preprocess_tile_t *tile,
    const uint32_t fftshift_power[RF_V12_FFT_POINTS],
    uint32_t raw_frame_index,
    float linear_power_scale)
{
    if ((fftshift_power == NULL) ||
        !rf_v12_preprocess_frame_begin(tile, raw_frame_index))
    {
        return false;
    }
    if ((raw_frame_index >= RF_V12_STFT_EDGE_CROP_FRAMES) &&
        (raw_frame_index <
         (RF_V12_RAW_STFT_FRAMES - RF_V12_STFT_EDGE_CROP_FRAMES)))
    {
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FFT_POINTS;
             ++frequency)
        {
            rf_v12_accumulate_power_bin_unchecked(
                tile, frequency, fftshift_power[frequency]);
        }
    }
    return rf_v12_preprocess_frame_end(tile, linear_power_scale);
}

RF_V12_HOT_CODE bool rf_v12_preprocess_frame_gathered(
    rf_v12_preprocess_tile_t *tile,
    const rf_v12_rebin_output_map_t
        map[RF_V12_FEATURE_FREQUENCY_BINS],
    const uint32_t fftshift_power[RF_V12_FFT_POINTS],
    uint32_t raw_frame_index,
    float linear_power_scale)
{
    const bool retained =
        (raw_frame_index >= RF_V12_STFT_EDGE_CROP_FRAMES) &&
        (raw_frame_index <
         (RF_V12_RAW_STFT_FRAMES - RF_V12_STFT_EDGE_CROP_FRAMES));

    if ((map == NULL) || (fftshift_power == NULL) ||
        !rf_v12_preprocess_frame_open(tile, raw_frame_index, false))
    {
        return false;
    }
    if (retained)
    {
        if ((tile->time_bin >= RF_V12_FEATURE_TIME_BINS) ||
            (tile->pool_frame_count >= RF_V12_TIME_POOL_FRAMES))
        {
            tile->frame_open = 0U;
            return false;
        }
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FEATURE_FREQUENCY_BINS;
             ++frequency)
        {
            const uint32_t first = map[frequency].first_input;
            const uint64_t middle =
                (uint64_t)fftshift_power[first + 1U] +
                fftshift_power[first + 2U] +
                fftshift_power[first + 3U] +
                fftshift_power[first + 4U];
            const uint64_t weighted =
                ((uint64_t)fftshift_power[first] *
                 map[frequency].first_weight) +
                (middle * RF_V12_REBIN_RAW_UNITS) +
                ((uint64_t)fftshift_power[first + 5U] *
                 map[frequency].last_weight);

            tile->pool_weighted_power_sum[frequency] += weighted;
            if (weighted > tile->pool_weighted_power_max[frequency])
            {
                tile->pool_weighted_power_max[frequency] = weighted;
            }
        }
        rf_v12_finish_pool_frame(tile, linear_power_scale);
    }
    tile->frame_open = 0U;
    tile->raw_frame_index++;
    return true;
}

void rf_v12_preprocess_background_init(
    rf_v12_preprocess_background_t *background)
{
    if (background == NULL)
    {
        return;
    }
    background->gain_db_q8 = 0;
    background->generation = 0U;
    background->gain_valid = 0U;
    background->calibration_count = 0U;
    background->calibration_write_index = 0U;
    background->validation_pass_count = 0U;
    background->valid_window_count = 0U;
    background->ready = 0U;
    background->forced_ready = 0U;
    background->reset_pending = 0U;
}

bool rf_v12_preprocess_background_set_gain(
    rf_v12_preprocess_background_t *background,
    int16_t gain_db_q8)
{
    if (background == NULL)
    {
        return false;
    }
    if (background->gain_valid == 0U)
    {
        background->gain_db_q8 = gain_db_q8;
        background->gain_valid = 1U;
        return true;
    }
    if ((background->gain_db_q8 == gain_db_q8) ||
        (background->ready != 0U))
    {
        return false;
    }
    background->gain_db_q8 = gain_db_q8;
    background->calibration_count = 0U;
    background->calibration_write_index = 0U;
    background->validation_pass_count = 0U;
    background->valid_window_count = 0U;
    background->forced_ready = 0U;
    background->reset_pending = 1U;
    return true;
}

static int16_t rf_v12_background_q8_8(float value)
{
    int32_t quantized = rf_v12_preprocess_round_to_nearest_even(
        value * (float)(1U << RF_V12_PREPROCESS_BACKGROUND_Q_SHIFT));
    if (quantized > INT16_MAX)
    {
        quantized = INT16_MAX;
    }
    else if (quantized < INT16_MIN)
    {
        quantized = INT16_MIN;
    }
    return (int16_t)quantized;
}

static int16_t rf_v12_q8_8_midpoint(int16_t low, int16_t high)
{
    return (int16_t)(((int32_t)low + (int32_t)high) / 2);
}

static void rf_v12_background_store(
    rf_v12_preprocess_background_t *background,
    const rf_v12_preprocess_tile_t *tile)
{
    int16_t *destination =
        background->calibration_q8_8[background->calibration_write_index];
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        destination[cell] = rf_v12_background_q8_8(tile->c0_db[cell]);
    }
    background->calibration_write_index++;
    if (background->calibration_write_index ==
        RF_V12_PREPROCESS_BACKGROUND_WINDOWS)
    {
        background->calibration_write_index = 0U;
    }
    if (background->calibration_count <
        RF_V12_PREPROCESS_BACKGROUND_WINDOWS)
    {
        background->calibration_count++;
    }
}

static void rf_v12_background_build_candidate(
    rf_v12_preprocess_background_t *background)
{
    memset(background->unstable_mask, 0,
           sizeof(background->unstable_mask));
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        int16_t values[RF_V12_PREPROCESS_BACKGROUND_WINDOWS];
        int16_t q25;
        int16_t q50;
        int16_t q75;
        bool unstable;

        for (uint32_t window = 0U;
             window < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
             ++window)
        {
            values[window] = background->calibration_q8_8[window][cell];
        }
        for (uint32_t index = 1U;
             index < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
             ++index)
        {
            const int16_t value = values[index];
            uint32_t position = index;
            while ((position != 0U) &&
                   (values[position - 1U] > value))
            {
                values[position] = values[position - 1U];
                --position;
            }
            values[position] = value;
        }
        q25 = rf_v12_q8_8_midpoint(values[3], values[4]);
        q50 = rf_v12_q8_8_midpoint(values[7], values[8]);
        q75 = rf_v12_q8_8_midpoint(values[11], values[12]);
        unstable = ((int32_t)q75 - (int32_t)q25) >
                   RF_V12_BACKGROUND_IQR_LIMIT_Q8_8;
        background->baseline_q8_8[cell] = unstable ? q25 : q50;
        if (unstable)
        {
            background->unstable_mask[cell >> 3U] |=
                (uint8_t)(1U << (cell & 7U));
        }
    }
}

static bool rf_v12_background_validation_passes(
    const rf_v12_preprocess_background_t *background,
    const rf_v12_preprocess_tile_t *tile)
{
    uint32_t residual_below_median_band = 0U;
    uint32_t residual_above_median_band = 0U;
    uint32_t residual_below_limit = 0U;
    const uint32_t median_count_limit =
        RF_V12_PREPROCESS_FEATURE_CELLS / 2U;

    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        const int32_t residual =
            (int32_t)rf_v12_background_q8_8(tile->c0_db[cell]) -
            (int32_t)background->baseline_q8_8[cell];
        if (residual < -RF_V12_VALIDATION_MEDIAN_LIMIT_Q8_8)
        {
            residual_below_median_band++;
        }
        if (residual > RF_V12_VALIDATION_MEDIAN_LIMIT_Q8_8)
        {
            residual_above_median_band++;
        }
        if (residual <= RF_V12_VALIDATION_RESIDUAL_LIMIT_Q8_8)
        {
            residual_below_limit++;
        }
    }
    return (residual_below_median_band <= median_count_limit) &&
           (residual_above_median_band <= median_count_limit) &&
           ((residual_below_limit *
             RF_V12_VALIDATION_REQUIRED_DENOMINATOR) >=
            (RF_V12_PREPROCESS_FEATURE_CELLS *
             RF_V12_VALIDATION_REQUIRED_NUMERATOR));
}

static void rf_v12_background_mark_ready(
    rf_v12_preprocess_background_t *background,
    bool forced)
{
    background->ready = 1U;
    background->forced_ready = forced ? 1U : 0U;
    background->generation++;
    if (background->generation == 0U)
    {
        background->generation = 1U;
    }
}

bool rf_v12_preprocess_tile_complete(const rf_v12_preprocess_tile_t *tile)
{
    return (tile != NULL) &&
           (tile->frame_open == 0U) &&
           (tile->raw_frame_index == RF_V12_RAW_STFT_FRAMES) &&
           (tile->time_bin == RF_V12_FEATURE_TIME_BINS) &&
           (tile->pool_frame_count == 0U);
}

static void rf_v12_encode_features(
    rf_v12_preprocess_tile_t *tile,
    const float *background,
    bool background_is_cellwise)
{
    if (background != NULL)
    {
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FEATURE_FREQUENCY_BINS;
             ++frequency)
        {
            for (uint32_t time = 0U;
                 time < RF_V12_FEATURE_TIME_BINS;
                 ++time)
            {
                const uint32_t cell =
                    (frequency * RF_V12_FEATURE_TIME_BINS) + time;
                tile->c0_db[cell] -= background_is_cellwise ?
                                     background[cell] : background[frequency];
            }
        }
    }

    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        for (uint32_t time = 0U;
             time < RF_V12_FEATURE_TIME_BINS;
             ++time)
        {
            const uint32_t cell =
                (frequency * RF_V12_FEATURE_TIME_BINS) + time;
            const uint32_t feature = cell * RF_V12_FEATURE_CHANNELS;
            float c2;
            float c3;

            if (frequency == 0U)
            {
                c2 = tile->c0_db[cell + RF_V12_FEATURE_TIME_BINS] -
                     tile->c0_db[cell];
            }
            else if (frequency == (RF_V12_FEATURE_FREQUENCY_BINS - 1U))
            {
                c2 = tile->c0_db[cell] -
                     tile->c0_db[cell - RF_V12_FEATURE_TIME_BINS];
            }
            else
            {
                c2 = 0.5F *
                     (tile->c0_db[cell + RF_V12_FEATURE_TIME_BINS] -
                      tile->c0_db[cell - RF_V12_FEATURE_TIME_BINS]);
            }

            if (time == 0U)
            {
                c3 = tile->c0_db[cell + 1U] - tile->c0_db[cell];
            }
            else if (time == (RF_V12_FEATURE_TIME_BINS - 1U))
            {
                c3 = tile->c0_db[cell] - tile->c0_db[cell - 1U];
            }
            else
            {
                c3 = 0.5F *
                     (tile->c0_db[cell + 1U] - tile->c0_db[cell - 1U]);
            }

            tile->feature_staging[feature] =
                rf_v12_preprocess_quantize(tile->c0_db[cell], 0U);
            tile->feature_staging[feature + 2U] =
                rf_v12_preprocess_quantize(c2, 2U);
            tile->feature_staging[feature + 3U] =
                rf_v12_preprocess_quantize(c3, 3U);
        }
    }
}

rf_v12_preprocess_finalize_info_t rf_v12_preprocess_finalize(
    rf_v12_preprocess_tile_t *tile,
    rf_v12_preprocess_background_t *background,
    bool capture_valid)
{
    rf_v12_preprocess_finalize_info_t info =
    {
        RF_V12_PREPROCESS_INVALID, 0U, 0U, 0U
    };

    if (background != NULL)
    {
        info.background_generation = background->generation;
        info.background_reset = background->reset_pending;
    }
    if ((tile == NULL) || (background == NULL) ||
        (tile->feature_staging == NULL) || !capture_valid ||
        (background->gain_valid == 0U) ||
        !rf_v12_preprocess_tile_complete(tile))
    {
        return info;
    }

    if (background->ready == 0U)
    {
        if (background->calibration_count <
            RF_V12_PREPROCESS_BACKGROUND_WINDOWS)
        {
            rf_v12_background_store(background, tile);
            background->valid_window_count++;
            if (background->calibration_count ==
                RF_V12_PREPROCESS_BACKGROUND_WINDOWS)
            {
                rf_v12_background_build_candidate(background);
            }
        }
        else
        {
            const bool validation_passed =
                rf_v12_background_validation_passes(background, tile);
            bool forced = false;

            background->valid_window_count++;
            if (validation_passed)
            {
                background->validation_pass_count++;
            }
            else
            {
                background->validation_pass_count = 0U;
                rf_v12_background_store(background, tile);
                rf_v12_background_build_candidate(background);
            }
            if (background->validation_pass_count >=
                RF_V12_PREPROCESS_VALIDATION_WINDOWS)
            {
                rf_v12_background_mark_ready(background, false);
            }
            else if (background->valid_window_count >=
                     RF_V12_PREPROCESS_MAX_CALIBRATION_WINDOWS)
            {
                forced = true;
                rf_v12_background_mark_ready(background, forced);
            }
            if (background->ready != 0U)
            {
                info.background_became_ready = 1U;
                info.background_generation = background->generation;
            }
        }
        info.result = RF_V12_PREPROCESS_BACKGROUND_NOT_READY;
        return info;
    }

    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        tile->c0_db[cell] -=
            (float)background->baseline_q8_8[cell] /
            (float)(1U << RF_V12_PREPROCESS_BACKGROUND_Q_SHIFT);
    }

    rf_v12_encode_features(tile, NULL, false);
    info.result = RF_V12_PREPROCESS_READY;
    info.background_generation = background->generation;
    info.background_reset = 0U;
    background->reset_pending = 0U;
    return info;
}

bool rf_v12_preprocess_finalize_synthetic(
    rf_v12_preprocess_tile_t *tile)
{
    float background[RF_V12_FEATURE_FREQUENCY_BINS];
    float values[RF_V12_FEATURE_TIME_BINS];

    if ((tile == NULL) || (tile->feature_staging == NULL) ||
        !rf_v12_preprocess_tile_complete(tile))
    {
        return false;
    }
    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        for (uint32_t time = 0U;
             time < RF_V12_FEATURE_TIME_BINS;
             ++time)
        {
            values[time] = tile->c0_db[
                (frequency * RF_V12_FEATURE_TIME_BINS) + time];
        }
        for (uint32_t index = 1U;
             index < RF_V12_FEATURE_TIME_BINS;
             ++index)
        {
            const float value = values[index];
            uint32_t position = index;
            while ((position != 0U) &&
                   (values[position - 1U] > value))
            {
                values[position] = values[position - 1U];
                --position;
            }
            values[position] = value;
        }
        background[frequency] =
            values[RF_V12_FEATURE_TIME_BINS / 2U];
    }
    rf_v12_encode_features(tile, background, false);
    return true;
}

const float *rf_v12_preprocess_c0(
    const rf_v12_preprocess_tile_t *tile)
{
    return (tile != NULL) ? tile->c0_db : NULL;
}
