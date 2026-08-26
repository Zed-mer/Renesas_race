#ifndef RF_MODEL_INPUT_H
#define RF_MODEL_INPUT_H

#include <stdint.h>

#define RF_MODEL_INPUT_MAGIC        (0x52464D49UL) /* RFMI */
#define RF_MODEL_CHANNEL_COUNT      (2U)
#define RF_MODEL_FEATURE_BINS       (128U)

typedef struct st_rf_model_input
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t channel_mask;
    uint32_t fft_size;
    uint32_t sample_rate_hz;
    int8_t spectrum[RF_MODEL_CHANNEL_COUNT][RF_MODEL_FEATURE_BINS];
} rf_model_input_t;

typedef char rf_model_input_size_must_be_276[(sizeof(rf_model_input_t) == 276U) ? 1 : -1];

#endif
