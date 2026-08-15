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

static void prepare_complete_tile(rf_v12_preprocess_tile_t *tile,
                                  int8_t *features,
                                  float c0_db)
{
    rf_v12_preprocess_tile_reset(tile, features);
    tile->raw_frame_index = RF_V12_RAW_STFT_FRAMES;
    tile->time_bin = RF_V12_FEATURE_TIME_BINS;
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        tile->c0_db[cell] = c0_db;
        features[(cell * RF_V12_FEATURE_CHANNELS) + 1U] =
            rf_v12_preprocess_quantize(0.0F, 1U);
    }
}

static int test_robust_background_and_channels(void)
{
    rf_v12_preprocess_tile_t *tile = calloc(1U, sizeof(*tile));
    int8_t *features = calloc(RF_V12_FEATURE_BYTES, 1U);
    rf_v12_preprocess_background_t *background =
        calloc(1U, sizeof(*background));
    rf_v12_preprocess_finalize_info_t info;
    int result = 0;

    if ((tile == NULL) || (features == NULL) || (background == NULL))
    {
        result = test_fail("allocation failed");
        goto done;
    }

    rf_v12_preprocess_background_init(background);
    if (!rf_v12_preprocess_background_set_gain(background, 0))
    {
        result = test_fail("initial gain was not accepted");
        goto done;
    }
    prepare_complete_tile(tile, features, 30.0F);
    info = rf_v12_preprocess_finalize(tile, background, false);
    if ((info.result != RF_V12_PREPROCESS_INVALID) ||
        (background->valid_window_count != 0U))
    {
        result = test_fail("an invalid capture trained the background");
        goto done;
    }

    for (uint32_t window = 0U;
         window < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
         ++window)
    {
        prepare_complete_tile(tile, features, 30.0F);
        tile->c0_db[0] = (float)window;
        info = rf_v12_preprocess_finalize(tile, background, true);
        if ((info.result != RF_V12_PREPROCESS_BACKGROUND_NOT_READY) ||
            (background->ready != 0U))
        {
            result = test_fail("training window escaped startup suppression");
            goto done;
        }
    }
    if ((background->unstable_mask[0] & 1U) == 0U)
    {
        result = test_fail("intermittent cell was not marked unstable");
        goto done;
    }
    if ((background->baseline_q8_8[0] != (int16_t)(3.5F * 256.0F)) ||
        (background->baseline_q8_8[1] != (int16_t)(30.0F * 256.0F)))
    {
        result = test_fail("Q25/Q50 candidate selection mismatch");
        goto done;
    }

    for (uint32_t validation = 0U;
         validation < RF_V12_PREPROCESS_VALIDATION_WINDOWS;
         ++validation)
    {
        prepare_complete_tile(tile, features, 30.0F);
        tile->c0_db[0] = 3.5F;
        info = rf_v12_preprocess_finalize(tile, background, true);
        if (info.result != RF_V12_PREPROCESS_BACKGROUND_NOT_READY)
        {
            result = test_fail("validation window emitted model input");
            goto done;
        }
    }
    if ((background->ready == 0U) || (background->forced_ready != 0U) ||
        (background->generation != 1U) ||
        (info.background_became_ready == 0U))
    {
        result = test_fail("two-window validation did not freeze background");
        goto done;
    }

    memset(features, 0, RF_V12_FEATURE_BYTES);
    prepare_complete_tile(tile, features, 31.0F);
    tile->c0_db[0] = 4.5F;
    info = rf_v12_preprocess_finalize(tile, background, true);
    if (info.result != RF_V12_PREPROCESS_READY)
    {
        result = test_fail("post-calibration tile was not inference-ready");
        goto done;
    }
    for (uint32_t cell = 0U;
         cell < RF_V12_PREPROCESS_FEATURE_CELLS;
         ++cell)
    {
        const uint32_t offset = cell * RF_V12_FEATURE_CHANNELS;
        const float expected_c0 = 1.0F;
        const float expected_c1 = 0.0F;
        if (fabsf(tile->c0_db[cell] - expected_c0) > TEST_TOLERANCE_DB)
        {
            result = test_fail("background-relative C0 mismatch");
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

    rf_v12_preprocess_background_init(background);
    (void)rf_v12_preprocess_background_set_gain(background, 0);
    for (uint32_t window = 0U;
         window < RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
         ++window)
    {
        prepare_complete_tile(tile, features, 0.0F);
        (void)rf_v12_preprocess_finalize(tile, background, true);
    }
    for (uint32_t extension = RF_V12_PREPROCESS_BACKGROUND_WINDOWS;
         extension < RF_V12_PREPROCESS_MAX_CALIBRATION_WINDOWS;
         ++extension)
    {
        prepare_complete_tile(tile, features, 10.0F);
        info = rf_v12_preprocess_finalize(tile, background, true);
    }
    if ((background->ready == 0U) || (background->forced_ready == 0U) ||
        (background->valid_window_count !=
         RF_V12_PREPROCESS_MAX_CALIBRATION_WINDOWS) ||
        (info.background_became_ready == 0U))
    {
        result = test_fail("24-window validation bound was not enforced");
    }

done:
    free(background);
    free(features);
    free(tile);
    return result;
}

int main(void)
{
    if ((test_rne() != 0) ||
        (test_exact_area() != 0) ||
        (test_robust_background_and_channels() != 0))
    {
        return 1;
    }
    puts("rf_v12_preprocess: all host tests passed");
    return 0;
}
