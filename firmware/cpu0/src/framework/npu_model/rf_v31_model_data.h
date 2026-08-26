#ifndef RF_V31_MODEL_DATA_H
#define RF_V31_MODEL_DATA_H

#include <stdint.h>

#define RF_V31_MODEL_MAX_OUTPUTS 5u
#ifndef RF_V31_SHARED_ARENA_BYTES
#define RF_V31_SHARED_ARENA_BYTES 192176u
#endif
#define RF_V31_MODEL_COMMAND_WEIGHT_BYTES 278196u

#define RF_V31_MAIN_COMMAND_BYTES 17772u
#define RF_V31_MAIN_WEIGHT_BYTES 11792u
#define RF_V31_MAIN_SCRATCH_BYTES 192176u
#define RF_V31_MAIN_INPUT_OFFSET 0u
#define RF_V31_MAIN_INPUT_BYTES 93840u
#define RF_V31_MAIN_OUTPUT_COUNT 5u
#define RF_V31_DJI_VIDEO_COMMAND_BYTES 25996u
#define RF_V31_DJI_VIDEO_WEIGHT_BYTES 11600u
#define RF_V31_DJI_VIDEO_SCRATCH_BYTES 161488u
#define RF_V31_DJI_VIDEO_INPUT_OFFSET 47328u
#define RF_V31_DJI_VIDEO_INPUT_BYTES 93840u
#define RF_V31_DJI_VIDEO_OUTPUT_COUNT 1u
#define RF_V31_DJI_CONTROL_COMMAND_BYTES 108488u
#define RF_V31_DJI_CONTROL_WEIGHT_BYTES 6144u
#define RF_V31_DJI_CONTROL_SCRATCH_BYTES 179536u
#define RF_V31_DJI_CONTROL_INPUT_OFFSET 5920u
#define RF_V31_DJI_CONTROL_INPUT_BYTES 93840u
#define RF_V31_DJI_CONTROL_OUTPUT_COUNT 1u
#define RF_V31_T12_COMMAND_BYTES 91092u
#define RF_V31_T12_WEIGHT_BYTES 5312u
#define RF_V31_T12_SCRATCH_BYTES 157264u
#define RF_V31_T12_INPUT_OFFSET 5920u
#define RF_V31_T12_INPUT_BYTES 93840u
#define RF_V31_T12_OUTPUT_COUNT 1u

typedef struct rf_v31_model_blob {
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
    uint32_t output_region[RF_V31_MODEL_MAX_OUTPUTS];
    uint32_t output_offset[RF_V31_MODEL_MAX_OUTPUTS];
    uint32_t output_bytes[RF_V31_MODEL_MAX_OUTPUTS];
} rf_v31_model_blob_t;

extern const rf_v31_model_blob_t g_rf_v31_main_model;
extern const rf_v31_model_blob_t g_rf_v31_dji_video_model;
extern const rf_v31_model_blob_t g_rf_v31_dji_control_model;
extern const rf_v31_model_blob_t g_rf_v31_t12_model;

#endif
