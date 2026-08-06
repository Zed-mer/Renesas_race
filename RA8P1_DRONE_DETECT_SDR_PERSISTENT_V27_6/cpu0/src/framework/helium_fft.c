#include "helium_fft.h"

#include <math.h>
#include <string.h>

#if defined(__ARM_FEATURE_MVE)
#include <arm_mve.h>
#endif

#if defined(__has_include)
#if __has_include("arm_math.h")
#include "arm_math.h"
#define RA8P1_HAS_CMSIS_DSP (1)
#endif
#endif

#ifndef RA8P1_HAS_CMSIS_DSP
#define RA8P1_HAS_CMSIS_DSP (0)
#endif

static volatile helium_fft_status_t g_fft_status = HELIUM_FFT_STATUS_NOT_CONFIGURED;
#if RA8P1_HAS_CMSIS_DSP
static arm_cfft_instance_f32 g_fft_instance;
static bool g_fft_ready;
static float32_t g_fft_data[HELIUM_FFT_SIZE * 2U]
    __attribute__((section(".dtcm"), aligned(32)));
static float32_t g_fft_power[HELIUM_FFT_SIZE]
    __attribute__((section(".dtcm"), aligned(32)));
static float32_t g_fft_window[HELIUM_FFT_SIZE]
    __attribute__((section(".dtcm"), aligned(32)));
#endif

void helium_fft_init(void)
{
#if RA8P1_HAS_CMSIS_DSP
    g_fft_ready = (ARM_MATH_SUCCESS == arm_cfft_init_1024_f32(&g_fft_instance));
    if (g_fft_ready)
    {
        arm_hanning_f32(g_fft_window, HELIUM_FFT_SIZE);
    }
#if defined(__ARM_FEATURE_MVE)
    g_fft_status = g_fft_ready ? HELIUM_FFT_STATUS_CMSIS_DSP_MVE : HELIUM_FFT_STATUS_NOT_CONFIGURED;
#else
    g_fft_status = g_fft_ready ? HELIUM_FFT_STATUS_SCALAR_PREPROCESS : HELIUM_FFT_STATUS_NOT_CONFIGURED;
#endif
#elif defined(__ARM_FEATURE_MVE)
    g_fft_status = HELIUM_FFT_STATUS_MVE_PREPROCESS;
#else
    g_fft_status = HELIUM_FFT_STATUS_SCALAR_PREPROCESS;
#endif
}

#if RA8P1_HAS_CMSIS_DSP
static bool helium_fft_finish(helium_fft_result_t *result,
                              int8_t *features,
                              uint32_t feature_bins,
                              uint64_t time_domain_energy)
{
    uint32_t i;
    float32_t maximum;
    float32_t normalized_peak_power;
    uint32_t index;

    arm_cfft_f32(&g_fft_instance, g_fft_data, 0U, 1U);
    arm_cmplx_mag_squared_f32(g_fft_data, g_fft_power, HELIUM_FFT_SIZE);
    g_fft_power[0] = 0.0F;
    arm_max_f32(g_fft_power, HELIUM_FFT_SIZE, &maximum, &index);
    result->peak_bin = (index + (HELIUM_FFT_SIZE / 2U)) % HELIUM_FFT_SIZE;
    normalized_peak_power = maximum * (4.0F / ((float32_t) HELIUM_FFT_SIZE * (float32_t) HELIUM_FFT_SIZE));
    if (normalized_peak_power >= 65535.0F)
    {
        result->peak_power_q16 = UINT32_MAX;
    }
    else
    {
        result->peak_power_q16 = (uint32_t) (normalized_peak_power * 65536.0F);
    }
    result->time_domain_energy = time_domain_energy;
    if (features != NULL)
    {
        const uint32_t spectrum_bins = HELIUM_FFT_SIZE;
        if ((feature_bins == 0U) || ((spectrum_bins % feature_bins) != 0U))
        {
            return false;
        }
        if (maximum <= 0.0F)
        {
            memset(features, 0, feature_bins);
        }
        else
        {
            const uint32_t bins_per_feature = spectrum_bins / feature_bins;
            uint32_t feature;
            for (feature = 0U; feature < feature_bins; feature++)
            {
                float32_t group_maximum = 0.0F;
                uint32_t bin;
                for (bin = 0U; bin < bins_per_feature; bin++)
                {
                    const uint32_t shifted_bin = (feature * bins_per_feature) + bin;
                    const uint32_t source_bin = (shifted_bin + (HELIUM_FFT_SIZE / 2U)) % HELIUM_FFT_SIZE;
                    const float32_t power = g_fft_power[source_bin];
                    if (power > group_maximum)
                    {
                        group_maximum = power;
                    }
                }
                group_maximum = (group_maximum * 127.0F) / maximum;
                if (group_maximum > 127.0F)
                {
                    group_maximum = 127.0F;
                }
                features[feature] = (int8_t) group_maximum;
            }
        }
    }
    return true;
}
#endif

bool helium_fft_display_spectrum_u8(uint8_t *spectrum, uint32_t spectrum_bins)
{
#if RA8P1_HAS_CMSIS_DSP
    const float32_t power_scale = 4.0F /
        ((float32_t) HELIUM_FFT_SIZE * (float32_t) HELIUM_FFT_SIZE);
    uint32_t output_bin;

    if ((spectrum == NULL) || (spectrum_bins == 0U) ||
        ((HELIUM_FFT_SIZE % spectrum_bins) != 0U))
    {
        return false;
    }

    for (output_bin = 0U; output_bin < spectrum_bins; output_bin++)
    {
        const uint32_t bins_per_output = HELIUM_FFT_SIZE / spectrum_bins;
        float32_t group_maximum = 0.0F;
        uint32_t bin;

        for (bin = 0U; bin < bins_per_output; bin++)
        {
            const uint32_t shifted_bin = (output_bin * bins_per_output) + bin;
            const uint32_t source_bin = (shifted_bin + (HELIUM_FFT_SIZE / 2U)) % HELIUM_FFT_SIZE;
            if (g_fft_power[source_bin] > group_maximum)
            {
                group_maximum = g_fft_power[source_bin];
            }
        }

        group_maximum *= power_scale;
        if (group_maximum <= 1.0e-12F)
        {
            spectrum[output_bin] = 0U;
        }
        else
        {
            float32_t dbfs = 10.0F * log10f(group_maximum);
            if (dbfs <= -120.0F)
            {
                spectrum[output_bin] = 0U;
            }
            else if (dbfs >= 0.0F)
            {
                spectrum[output_bin] = 255U;
            }
            else
            {
                spectrum[output_bin] = (uint8_t) ((dbfs + 120.0F) * (255.0F / 120.0F));
            }
        }
    }
    return true;
#else
    (void) spectrum;
    (void) spectrum_bins;
    return false;
#endif
}

bool helium_fft_execute_s8_features(const int8_t *iq,
                                    uint32_t complex_samples,
                                    helium_fft_result_t *result,
                                    int8_t *features,
                                    uint32_t feature_bins)
{
#if RA8P1_HAS_CMSIS_DSP
    uint32_t i;

    if ((!g_fft_ready) || (iq == NULL) || (result == NULL) || (complex_samples != HELIUM_FFT_SIZE))
    {
        return false;
    }
    for (i = 0U; i < HELIUM_FFT_SIZE; i++)
    {
        float32_t window = g_fft_window[i] / 128.0F;
        g_fft_data[2U * i] = (float32_t) iq[2U * i] * window;
        g_fft_data[(2U * i) + 1U] = (float32_t) iq[(2U * i) + 1U] * window;
    }
    return helium_fft_finish(result,
                             features,
                             feature_bins,
                             helium_iq_energy_s8(iq, complex_samples));
#else
    (void) iq;
    (void) complex_samples;
    (void) result;
    (void) features;
    (void) feature_bins;
    return false;
#endif
}

bool helium_fft_execute_s16_features(const int16_t *iq,
                                     uint32_t complex_samples,
                                     helium_fft_result_t *result,
                                     int8_t *features,
                                     uint32_t feature_bins)
{
#if RA8P1_HAS_CMSIS_DSP
    uint32_t i;

    if ((!g_fft_ready) || (iq == NULL) || (result == NULL) || (complex_samples != HELIUM_FFT_SIZE))
    {
        return false;
    }
    for (i = 0U; i < HELIUM_FFT_SIZE; i++)
    {
        float32_t window = g_fft_window[i] / 32768.0F;
        g_fft_data[2U * i] = (float32_t) iq[2U * i] * window;
        g_fft_data[(2U * i) + 1U] = (float32_t) iq[(2U * i) + 1U] * window;
    }
    return helium_fft_finish(result,
                             features,
                             feature_bins,
                             helium_iq_energy_s16(iq, complex_samples));
#else
    (void) iq;
    (void) complex_samples;
    (void) result;
    (void) features;
    (void) feature_bins;
    return false;
#endif
}

bool helium_fft_execute_s8(const int8_t *iq, uint32_t complex_samples, helium_fft_result_t *result)
{
    return helium_fft_execute_s8_features(iq, complex_samples, result, NULL, 0U);
}

bool helium_fft_execute_s16(const int16_t *iq, uint32_t complex_samples, helium_fft_result_t *result)
{
    return helium_fft_execute_s16_features(iq, complex_samples, result, NULL, 0U);
}

helium_fft_status_t helium_fft_status_get(void)
{
    return g_fft_status;
}

uint64_t helium_iq_energy_s8(const int8_t *iq, uint32_t complex_samples)
{
    uint32_t i;
    uint64_t energy = 0U;
#if defined(__ARM_FEATURE_MVE)
    uint32_t values = complex_samples * 2U;
    int32_t vector_sum = 0;
    i = 0U;
    while ((values - i) >= 16U)
    {
        int8x16_t samples = vld1q_s8(&iq[i]);
        vector_sum = vmladavaq_s8(vector_sum, samples, samples);
        i += 16U;
        if ((i & 0x0FFFU) == 0U)
        {
            energy += (uint32_t) vector_sum;
            vector_sum = 0;
        }
    }
    energy += (uint32_t) vector_sum;
    for (; i < values; i++)
    {
        int32_t value = iq[i];
        energy += (uint32_t) (value * value);
    }
#else
    for (i = 0U; i < complex_samples; i++)
    {
        int32_t real = iq[(2U * i)];
        int32_t imag = iq[(2U * i) + 1U];
        energy += (uint64_t) ((real * real) + (imag * imag));
    }
#endif
    return energy;
}

uint64_t helium_iq_energy_s16(const int16_t *iq, uint32_t complex_samples)
{
    uint32_t i;
    uint64_t energy = 0U;

    if (iq == NULL)
    {
        return 0U;
    }
    for (i = 0U; i < complex_samples; i++)
    {
        int32_t real = iq[2U * i];
        int32_t imag = iq[(2U * i) + 1U];
        energy += (uint64_t) (((int64_t) real * real) + ((int64_t) imag * imag));
    }
    return energy;
}
