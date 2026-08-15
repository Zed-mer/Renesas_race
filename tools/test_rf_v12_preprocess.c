#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf_v12_preprocess.h"

#define TEST_POWER_SCALE (1.0F)
#define TEST_TOLERANCE_DB (2.0e-4F)

static int test_fail(const char *message)
{
    fprintf(stderr, "rf_v12_preprocess test failed: %s\n", message);
    return 1;
}

static int feed_tile(rf_v12_preprocess_tile_t *tile,
                     uint32_t retained_power,
                     uint32_t edge_power,
                     int burst_last_frame)
{
    for (uint32_t frame = 0U; frame < RF_V12_RAW_STFT_FRAMES; ++frame)
    {
        uint32_t power = retained_power;
        if ((frame == 0U) || (frame == (RF_V12_RAW_STFT_FRAMES - 1U)))
        {
            power = edge_power;
        }
        else if (burst_last_frame && (((frame - 1U) % 10U) == 9U))
        {
            power = retained_power * 10U;
        }
        if (!rf_v12_preprocess_frame_begin(tile, frame))
        {
            return test_fail("frame_begin rejected a valid ordered frame");
        }
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FFT_POINTS;
             ++frequency)
        {
            rf_v12_preprocess_power_bin(tile, frequency, power);
        }
        if (!rf_v12_preprocess_frame_end(tile, TEST_POWER_SCALE))
        {
            return test_fail("frame_end rejected a valid frame");
        }
    }
    return rf_v12_preprocess_tile_complete(tile) ? 0 :
           test_fail("complete 1152-frame tile was not accepted");
}

static int test_rne(void)
{
    static const struct
    {
        float input;
        int32_t expected;
    } cases[] =
    {
        {0.5F, 0}, {1.5F, 2}, {2.5F, 2}, {3.5F, 4},
        {-0.5F, 0}, {-1.5F, -2}, {-2.5F, -2}, {-3.5F, -4},
        {2.5001F, 3}, {-2.5001F, -3}
    };
    for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); ++i)
    {
        if (rf_v12_preprocess_round_to_nearest_even(cases[i].input) !=
            cases[i].expected)
        {
            return test_fail("round-to-nearest-even mismatch");
        }
    }
    if (rf_v12_preprocess_quantize(RF_V12_C0_MEAN, 0U) !=
        RF_V12_INPUT_ZERO_POINT)
    {
        return test_fail("normalization mean did not map to zero point");
    }
    return 0;
}

static int test_exact_area(void)
{
    rf_v12_preprocess_tile_t *tile = calloc(1U, sizeof(*tile));
    int8_t *features = calloc(RF_V12_FEATURE_BYTES, 1U);
    uint64_t expected_weighted = 0U;
    float expected_db;
    int result = 0;

    if ((tile == NULL) || (features == NULL))
    {
        result = test_fail("allocation failed");
        goto done;
    }
    rf_v12_preprocess_tile_reset(tile, features);
    for (uint32_t frame = 0U; frame < RF_V12_RAW_STFT_FRAMES; ++frame)
    {
        if (!rf_v12_preprocess_frame_begin(tile, frame))
        {
            result = test_fail("exact-area frame_begin failed");
            goto done;
        }
        for (uint32_t frequency = 0U;
             frequency < RF_V12_FFT_POINTS;
             ++frequency)
        {
            rf_v12_preprocess_power_bin(tile, frequency, frequency + 1U);
        }
        if (!rf_v12_preprocess_frame_end(tile, TEST_POWER_SCALE))
        {
            result = test_fail("exact-area frame_end failed");
            goto done;
        }
    }
    for (uint32_t raw = 0U; raw < 6U; ++raw)
    {
        const uint32_t weight = (raw < 5U) ? 51U : 1U;
        expected_weighted += (uint64_t)(raw + 1U) * weight;
    }
    expected_db = 10.0F * log10f((float)expected_weighted / 256.0F);
    if (fabsf(tile->c0_db[0] - expected_db) > TEST_TOLERANCE_DB)
    {
        result = test_fail("1024-to-204 exact-area cell mismatch");
    }

done:
    free(features);
    free(tile);
    return result;
}

static int test_direct_finalize_and_channels(void)
{
    rf_v12_preprocess_tile_t *tile = calloc(1U, sizeof(*tile));
    int8_t *features = calloc(RF_V12_FEATURE_BYTES, 1U);
    rf_v12_preprocess_result_t result_info;
    int result = 0;

    if ((tile == NULL) || (features == NULL))
    {
        result = test_fail("allocation failed");
        goto done;
    }

    rf_v12_preprocess_tile_reset(tile, features);
    if (feed_tile(tile, 600U, UINT32_MAX, 0) != 0)
    {
        result = 1;
        goto done;
    }
    result_info = rf_v12_preprocess_finalize(tile, false);
    if (result_info != RF_V12_PREPROCESS_INVALID)
    {
        result = test_fail("an invalid capture emitted model input");
        goto done;
    }

    memset(features, 0, RF_V12_FEATURE_BYTES);
    rf_v12_preprocess_tile_reset(tile, features);
    if (feed_tile(tile, 600U, UINT32_MAX, 0) != 0)
    {
        result = 1;
        goto done;
    }
    result_info = rf_v12_preprocess_finalize(tile, true);
    if (result_info != RF_V12_PREPROCESS_READY)
    {
        result = test_fail("first valid tile was not inference-ready");
        goto done;
    }
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        const uint32_t offset = cell * RF_V12_FEATURE_CHANNELS;
        const float expected_c0 = 10.0F * log10f(600.0F);
        const float expected_c1 = 0.0F;
        if (fabsf(tile->c0_db[cell] - expected_c0) > TEST_TOLERANCE_DB)
        {
            result = test_fail("uncalibrated C0 mismatch");
            goto done;
        }
        if ((features[offset] !=
             rf_v12_preprocess_quantize(expected_c0, 0U)) ||
            (features[offset + 1U] !=
             rf_v12_preprocess_quantize(expected_c1, 1U)) ||
            (features[offset + 2U] !=
             rf_v12_preprocess_quantize(0.0F, 2U)) ||
            (features[offset + 3U] !=
             rf_v12_preprocess_quantize(0.0F, 3U)))
        {
            result = test_fail("C0/C1/C2/C3 NHWC quantization mismatch");
            goto done;
        }
    }

done:
    free(features);
    free(tile);
    return result;
}

int main(void)
{
    if ((test_rne() != 0) ||
        (test_exact_area() != 0) ||
        (test_direct_finalize_and_channels() != 0))
    {
        return 1;
    }
    puts("rf_v12_preprocess: all host tests passed");
    return 0;
}
