#ifndef HELIUM_FFT_H
#define HELIUM_FFT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum e_helium_fft_status
{
    HELIUM_FFT_STATUS_NOT_CONFIGURED = 0,
    HELIUM_FFT_STATUS_SCALAR_PREPROCESS = 1,
    HELIUM_FFT_STATUS_MVE_PREPROCESS = 2,
    HELIUM_FFT_STATUS_CMSIS_DSP_MVE = 3
} helium_fft_status_t;

#define HELIUM_FFT_SIZE (1024U)
#define HELIUM_FFT_FEATURE_BINS (128U)

typedef struct st_helium_fft_result
{
    uint32_t frame_sequence;
    uint32_t peak_bin;
    uint32_t peak_power_q16;
    uint64_t time_domain_energy;
} helium_fft_result_t;

void helium_fft_init(void);
helium_fft_status_t helium_fft_status_get(void);
uint64_t helium_iq_energy_s8(const int8_t *iq, uint32_t complex_samples);
uint64_t helium_iq_energy_s16(const int16_t *iq, uint32_t complex_samples);
bool helium_fft_display_spectrum_u8(uint8_t *spectrum, uint32_t spectrum_bins);
bool helium_fft_execute_s8(const int8_t *iq, uint32_t complex_samples, helium_fft_result_t *result);
bool helium_fft_execute_s8_features(const int8_t *iq,
                                    uint32_t complex_samples,
                                    helium_fft_result_t *result,
                                    int8_t *features,
                                    uint32_t feature_bins);
bool helium_fft_execute_s16(const int16_t *iq, uint32_t complex_samples, helium_fft_result_t *result);
bool helium_fft_execute_s16_features(const int16_t *iq,
                                     uint32_t complex_samples,
                                     helium_fft_result_t *result,
                                     int8_t *features,
                                     uint32_t feature_bins);

#endif
