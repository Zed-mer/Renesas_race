#include "rf_v32_width_schedule.h"

#include <string.h>


rf_v31_schedule_status_t rf_v32_run_width_specialist(
    const int8_t roi_nhwc[RF_V32_WIDTH_INPUT_BYTES],
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    int8_t *output_code,
    rf_v31_invoke_fn invoke,
    void *context)
{
    const rf_v31_model_blob_t *model = &g_rf_v32_width_model;
    if (roi_nhwc == NULL || shared_arena == NULL || output_code == NULL ||
        invoke == NULL) {
        return RF_V31_SCHEDULE_BAD_ARGUMENT;
    }
    if (model->input_region != 1u || model->input_bytes != RF_V32_WIDTH_INPUT_BYTES ||
        model->output_count != 1u || model->output_region[0] != 1u ||
        model->output_bytes[0] != RF_V32_WIDTH_OUTPUT_BYTES ||
        (uint64_t)model->input_offset + model->input_bytes > model->scratch_bytes ||
        (uint64_t)model->output_offset[0] + model->output_bytes[0] >
            model->scratch_bytes) {
        return RF_V31_SCHEDULE_MODEL_ABI_ERROR;
    }
    if (shared_arena_bytes < RF_V31_SHARED_ARENA_BYTES ||
        shared_arena_bytes < model->scratch_bytes) {
        return RF_V31_SCHEDULE_ARENA_TOO_SMALL;
    }
    if ((const uint8_t *)roi_nhwc != shared_arena + model->input_offset) {
        memcpy(shared_arena + model->input_offset, roi_nhwc, RF_V32_WIDTH_INPUT_BYTES);
    }
    if (!invoke(model, shared_arena, shared_arena_bytes, context)) {
        return RF_V31_SCHEDULE_INVOKE_FAILED;
    }
    *output_code = (int8_t)shared_arena[model->output_offset[0]];
    return RF_V31_SCHEDULE_OK;
}
