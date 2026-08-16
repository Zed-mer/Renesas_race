#ifndef RF_V27_MODEL_DATA_H
#define RF_V27_MODEL_DATA_H

#include <stdint.h>

#define RF_V27_ABSOLUTE_SHARED_ARENA_BYTES 192176u
#define RF_V27_ABSOLUTE_SCRATCH_BYTES 192176u
#define RF_V27_ABSOLUTE_COMMAND_BYTES 19768u
#define RF_V27_ABSOLUTE_WEIGHT_BYTES 16960u
#define RF_V27_ABSOLUTE_INPUT_BYTES 93840u
#define RF_V27_ABSOLUTE_INPUT_OFFSET 0u
#define RF_V27_ABSOLUTE_OUTPUT_COUNT 5u
#define RF_V27_ABSOLUTE_OUTPUT_BYTES 5916u
#define RF_V27_ABSOLUTE_OUTPUT_DJI_CONTROL 3u
#define RF_V27_ABSOLUTE_OUTPUT_OFFSET_DJI_CONTROL 5920u

typedef struct rf_v27_model_blob
{
    const char *name;
    const uint8_t *command;
    const uint8_t *weights;
    uint32_t command_bytes;
    uint32_t weight_bytes;
    uint32_t weight_region;
    uint32_t scratch_region;
    uint32_t scratch_bytes;
    uint32_t input_region;
    uint32_t input_offset;
    uint32_t input_bytes;
    uint32_t output_count;
    uint32_t output_region[RF_V27_ABSOLUTE_OUTPUT_COUNT];
    uint32_t output_offset[RF_V27_ABSOLUTE_OUTPUT_COUNT];
    uint32_t output_bytes[RF_V27_ABSOLUTE_OUTPUT_COUNT];
} rf_v27_model_blob_t;

extern const rf_v27_model_blob_t g_rf_v27_absolute_model;

#endif
