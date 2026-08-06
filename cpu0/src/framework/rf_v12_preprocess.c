#include "rf_v12_preprocess.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define RF_V12_REBIN_RAW_UNITS       (51U)
#define RF_V12_REBIN_OUTPUT_UNITS    (256U)
#define RF_V12_DB_PER_LOG2_POWER     (3.010299956639812F)
#define RF_V12_POWER_EPSILON         (1.0e-12F)

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

bool rf_v12_preprocess_frame_begin(rf_v12_preprocess_tile_t *tile,
                                   uint32_t raw_frame_index)
{
    if ((tile == NULL) || (tile->frame_open != 0U) ||
        (raw_frame_index >= RF_V12_RAW_STFT_FRAMES) ||
        (raw_frame_index != tile->raw_frame_index))
    {
        return false;
    }
    memset(tile->frame_weighted_power, 0,
           sizeof(tile->frame_weighted_power));
    tile->frame_open = 1U;
    return true;
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
    uint32_t second_weight;

    /* Boundaries are represented in 1/51 of a raw FFT bin. One output cell
     * is exactly 256 such units, so a raw bin can touch at most two outputs. */
    raw_start = shifted_frequency_bin * RF_V12_REBIN_RAW_UNITS;
    raw_end = raw_start + RF_V12_REBIN_RAW_UNITS;
    first_output = raw_start / RF_V12_REBIN_OUTPUT_UNITS;
    first_end = (first_output + 1U) * RF_V12_REBIN_OUTPUT_UNITS;
    first_weight = ((raw_end < first_end) ? raw_end : first_end) - raw_start;
    tile->frame_weighted_power[first_output] +=
        (uint64_t)linear_power * first_weight;

    second_weight = RF_V12_REBIN_RAW_UNITS - first_weight;
    if ((second_weight != 0U) &&
        ((first_output + 1U) < RF_V12_FEATURE_FREQUENCY_BINS))
    {
        tile->frame_weighted_power[first_output + 1U] +=
            (uint64_t)linear_power * second_weight;
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
        tile->pool_frame_count++;
        if (tile->pool_frame_count == RF_V12_TIME_POOL_FRAMES)
        {
            const uint32_t time = tile->time_bin;
            for (uint32_t frequency = 0U;
                 frequency < RF_V12_FEATURE_FREQUENCY_BINS;
                 ++frequency)
            {
                const uint64_t sum =
                    tile->pool_weighted_power_sum[frequency];
                const uint64_t maximum =
                    tile->pool_weighted_power_max[frequency];
                const uint32_t cell =
                    (frequency * RF_V12_FEATURE_TIME_BINS) + time;
                const uint32_t feature = cell * RF_V12_FEATURE_CHANNELS;
                const float mean_db = rf_v12_power_db(
                    sum,
                    RF_V12_REBIN_OUTPUT_UNITS * RF_V12_TIME_POOL_FRAMES,
                    linear_power_scale);
                const float maximum_db = rf_v12_power_db(
                    maximum,
                    RF_V12_REBIN_OUTPUT_UNITS,
                    linear_power_scale);
                float c1 = maximum_db - mean_db;
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
                tile->pool_weighted_power_sum[frequency] = 0U;
                tile->pool_weighted_power_max[frequency] = 0U;
            }
            tile->pool_frame_count = 0U;
            tile->time_bin++;
        }
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
    background->ready = 0U;
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
        background->calibration_count = 0U;
        background->ready = 0U;
        background->reset_pending = 0U;
        return true;
    }
    if (background->gain_db_q8 == gain_db_q8)
    {
        return false;
    }
    background->gain_db_q8 = gain_db_q8;
    background->calibration_count = 0U;
    background->ready = 0U;
    background->reset_pending = 1U;
    return true;
}

static float rf_v12_median_five(float values[RF_V12_PREPROCESS_BACKGROUND_WINDOWS])
{
    for (uint32_t i = 1U;
         i < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
         ++i)
    {
        const float value = values[i];
        uint32_t position = i;
        while ((position != 0U) && (values[position - 1U] > value))
        {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
    return values[RF_V12_PREPROCESS_BACKGROUND_WINDOWS / 2U];
}

static void rf_v12_freeze_background(
    rf_v12_preprocess_background_t *background)
{
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        float values[RF_V12_PREPROCESS_BACKGROUND_WINDOWS];
        for (uint32_t window = 0U;
             window < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
             ++window)
        {
            values[window] = background->calibration[window][cell];
        }
        background->calibration[0][cell] = rf_v12_median_five(values);
    }
    background->ready = 1U;
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

static void rf_v12_encode_background_relative(
    rf_v12_preprocess_tile_t *tile,
    const float *background,
    bool background_is_cellwise)
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
            memcpy(background->calibration[background->calibration_count],
                   tile->c0_db,
                   sizeof(tile->c0_db));
            background->calibration_count++;
        }
        if (background->calibration_count ==
            RF_V12_PREPROCESS_BACKGROUND_WINDOWS)
        {
            rf_v12_freeze_background(background);
            info.background_became_ready = 1U;
            info.background_generation = background->generation;
        }
        info.result = RF_V12_PREPROCESS_BACKGROUND_NOT_READY;
        return info;
    }

    rf_v12_encode_background_relative(tile,
                                      background->calibration[0],
                                      true);

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
    rf_v12_encode_background_relative(tile, background, false);
    return true;
}

const float *rf_v12_preprocess_background_relative_c0(
    const rf_v12_preprocess_tile_t *tile)
{
    return (tile != NULL) ? tile->c0_db : NULL;
}
