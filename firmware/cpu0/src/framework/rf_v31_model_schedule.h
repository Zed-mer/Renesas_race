#ifndef RF_V31_MODEL_SCHEDULE_H
#define RF_V31_MODEL_SCHEDULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "npu_model/rf_v31_model_data.h"
#include "rf_v31_detection_contract.h"

#define RF_V31_CLASS_COUNT RF_V31_DETECTION_CLASS_COUNT

typedef enum rf_v31_class_id {
    RF_V31_DJI_CONTROL = 0,
    RF_V31_DJI_VIDEO = 1,
    RF_V31_AT9S = 2,
    RF_V31_T12 = 3,
    RF_V31_XIAOBAWANG = 4
} rf_v31_class_id_t;

typedef enum rf_v31_schedule_status {
    RF_V31_SCHEDULE_OK = 0,
    RF_V31_SCHEDULE_BAD_ARGUMENT = 1,
    RF_V31_SCHEDULE_ARENA_TOO_SMALL = 2,
    RF_V31_SCHEDULE_MODEL_ABI_ERROR = 3,
    RF_V31_SCHEDULE_INVOKE_FAILED = 4
} rf_v31_schedule_status_t;

typedef bool (*rf_v31_invoke_fn)(
    const rf_v31_model_blob_t *model,
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    void *context);

/*
 * q10_input must remain outside shared_arena. Each invocation overwrites the
 * arena, so selected heatmaps are copied into caller-owned storage before the
 * next model runs. heatmaps use logical class order 0..4.
 */
rf_v31_schedule_status_t rf_v31_run_selected_models(
    const int8_t q10_input[RF_V31_INPUT_BYTES],
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    int8_t heatmaps[RF_V31_CLASS_COUNT][RF_V31_HEATMAP_BYTES],
    rf_v31_invoke_fn invoke,
    void *context);

#endif
