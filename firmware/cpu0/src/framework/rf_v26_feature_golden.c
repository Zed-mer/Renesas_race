#include "rf_v26_feature_golden.h"

#include <math.h>

#define RF_V26_FLOOR_QUANTILE 0.10f
#define RF_V26_RELIABLE_EDGE_BINS 7u
#define RF_V26_INPUT_SCALE 0.14275820553302765f
#define RF_V26_INPUT_ZERO_POINT 0

static const float k_clip_min[RF_V26_FEATURE_CHANNELS] = {
    -8.0f, 0.0f, -12.0f, -22.0f};
static const float k_clip_max[RF_V26_FEATURE_CHANNELS] = {
    32.0f, 12.0f, 12.0f, 22.0f};
static const float k_mean[RF_V26_FEATURE_CHANNELS] = {
    3.624701738357544f, 3.079335927963257f,
    0.000020333360225777142f, 0.000061713853845931f};
static const float k_std[RF_V26_FEATURE_CHANNELS] = {
    4.285704135894775f, 0.936720073223114f,
    0.9811630249023438f, 1.2086801528930664f};

static float quantile_floor(
    const float raw[RF_V26_RAW_CHANNELS][RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS],
    unsigned time_index)
{
    float sorted[RF_V26_FREQUENCY_BINS - 2u * RF_V26_RELIABLE_EDGE_BINS];
    unsigned count = 0u;
    unsigned frequency;
    for (frequency = RF_V26_RELIABLE_EDGE_BINS;
         frequency < RF_V26_FREQUENCY_BINS - RF_V26_RELIABLE_EDGE_BINS;
         ++frequency) {
        float value = raw[0][frequency][time_index];
        unsigned position = count;
        while (position > 0u && sorted[position - 1u] > value) {
            sorted[position] = sorted[position - 1u];
            --position;
        }
        sorted[position] = value;
        ++count;
    }
    /* NumPy's linear quantile: q*(N-1) = 18.9 for N=190 and q=.10.
     * Keep the lower + fraction * delta evaluation order: it is required for
     * byte-exact float32 parity at half-way INT8 rounding boundaries. */
    return sorted[18u] + 0.90f * (sorted[19u] - sorted[18u]);
}

static float relative_value(
    const float raw[RF_V26_RAW_CHANNELS][RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS],
    const float floors[RF_V26_TIME_BINS], unsigned frequency, unsigned time)
{
    return raw[0][frequency][time] - floors[time];
}

static int8_t quantize_feature(float value, unsigned channel)
{
    float clipped = fminf(k_clip_max[channel], fmaxf(k_clip_min[channel], value));
    float normalized = (clipped - k_mean[channel]) / k_std[channel];
    long rounded = (long)nearbyintf(normalized / RF_V26_INPUT_SCALE +
                                    (float)RF_V26_INPUT_ZERO_POINT);
    if (rounded < -128L) {
        rounded = -128L;
    } else if (rounded > 127L) {
        rounded = 127L;
    }
    return (int8_t)rounded;
}

void rf_v26_build_input(
    const float raw[RF_V26_RAW_CHANNELS][RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS],
    int8_t output[RF_V26_FREQUENCY_BINS][RF_V26_TIME_BINS][RF_V26_FEATURE_CHANNELS])
{
    float floors[RF_V26_TIME_BINS];
    unsigned frequency;
    unsigned time;
    for (time = 0u; time < RF_V26_TIME_BINS; ++time) {
        floors[time] = quantile_floor(raw, time);
    }
    for (frequency = 0u; frequency < RF_V26_FREQUENCY_BINS; ++frequency) {
        for (time = 0u; time < RF_V26_TIME_BINS; ++time) {
            unsigned previous_frequency = frequency == 0u ? 0u : frequency - 1u;
            unsigned next_frequency = frequency + 1u >= RF_V26_FREQUENCY_BINS
                                          ? RF_V26_FREQUENCY_BINS - 1u
                                          : frequency + 1u;
            unsigned previous_time = time == 0u ? 0u : time - 1u;
            unsigned next_time = time + 1u >= RF_V26_TIME_BINS
                                     ? RF_V26_TIME_BINS - 1u
                                     : time + 1u;
            float center = relative_value(raw, floors, frequency, time);
            float frequency_gradient = frequency == 0u ||
                                               frequency + 1u >= RF_V26_FREQUENCY_BINS
                                           ? relative_value(raw, floors, next_frequency, time) -
                                                 relative_value(raw, floors, previous_frequency, time)
                                           : 0.5f * (relative_value(raw, floors, next_frequency, time) -
                                                     relative_value(raw, floors, previous_frequency, time));
            float time_gradient = time == 0u || time + 1u >= RF_V26_TIME_BINS
                                      ? relative_value(raw, floors, frequency, next_time) -
                                            relative_value(raw, floors, frequency, previous_time)
                                      : 0.5f * (relative_value(raw, floors, frequency, next_time) -
                                                relative_value(raw, floors, frequency, previous_time));
            output[frequency][time][0] = quantize_feature(center, 0u);
            output[frequency][time][1] = quantize_feature(raw[1][frequency][time], 1u);
            output[frequency][time][2] = quantize_feature(frequency_gradient, 2u);
            output[frequency][time][3] = quantize_feature(time_gradient, 3u);
        }
    }
}
