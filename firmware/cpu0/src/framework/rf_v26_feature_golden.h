#ifndef RF_V26_FEATURE_GOLDEN_H
#define RF_V26_FEATURE_GOLDEN_H

#include <stdint.h>

#define RF_V26_FREQUENCY_BINS 204u
#define RF_V26_TIME_BINS 115u
#define RF_V26_RAW_CHANNELS 2u
#define RF_V26_FEATURE_CHANNELS 4u

/*
 * Convert the two absolute STFT channels produced by CPU0 into the exact
 * no-startup-calibration NHWC INT8 tensor consumed by the V26 video model.
 * raw[0] is log-mean power (dBFS), raw[1] is log-max minus log-mean.
 */
void rf_v26_build_input(
    const float raw[RF_V26_RAW_CHANNELS][RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS],
    int8_t output[RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS][RF_V26_FEATURE_CHANNELS]);

#endif
