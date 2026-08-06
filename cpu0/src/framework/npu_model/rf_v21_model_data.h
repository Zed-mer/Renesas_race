#ifndef RF_V21_MODEL_DATA_H
#define RF_V21_MODEL_DATA_H

#include <stdint.h>

#define RF_V21_MODEL_MAX_OUTPUTS 5u
#define RF_V21_SHARED_ARENA_BYTES 192176u
#define RF_V21_MODEL_COMMAND_WEIGHT_BYTES 67552u

#define RF_V21_NONVIDEO_COMMAND_BYTES 18164u
#define RF_V21_NONVIDEO_WEIGHT_BYTES 11776u
#define RF_V21_NONVIDEO_SCRATCH_BYTES 192176u
#define RF_V21_NONVIDEO_INPUT_BYTES 93840u
#define RF_V21_NONVIDEO_OUTPUT_COUNT 5u
#define RF_V21_V20_VIDEO_COMMAND_BYTES 25996u
#define RF_V21_V20_VIDEO_WEIGHT_BYTES 11616u
#define RF_V21_V20_VIDEO_SCRATCH_BYTES 161488u
#define RF_V21_V20_VIDEO_INPUT_BYTES 93840u
#define RF_V21_V20_VIDEO_OUTPUT_COUNT 1u

typedef struct rf_v21_model_blob {
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
    uint32_t output_region[RF_V21_MODEL_MAX_OUTPUTS];
    uint32_t output_offset[RF_V21_MODEL_MAX_OUTPUTS];
    uint32_t output_bytes[RF_V21_MODEL_MAX_OUTPUTS];
} rf_v21_model_blob_t;

extern const rf_v21_model_blob_t g_rf_v21_nonvideo_model;
extern const rf_v21_model_blob_t g_rf_v21_v20_video_model;

#endif
