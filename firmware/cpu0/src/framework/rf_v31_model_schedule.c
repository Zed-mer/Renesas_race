#include "rf_v31_model_schedule.h"

#include <string.h>


static bool model_abi_is_valid(const rf_v31_model_blob_t *model)
{
    uint32_t index;
    if (model == NULL || model->input_region != 1u ||
        model->input_bytes != RF_V31_INPUT_BYTES || model->output_count == 0u ||
        model->output_count > RF_V31_MODEL_MAX_OUTPUTS) {
        return false;
    }
    if ((uint64_t)model->input_offset + model->input_bytes > model->scratch_bytes) {
        return false;
    }
    for (index = 0u; index < model->output_count; ++index) {
        if (model->output_region[index] != 1u ||
            model->output_bytes[index] != RF_V31_HEATMAP_BYTES ||
            (uint64_t)model->output_offset[index] + model->output_bytes[index] >
                model->scratch_bytes) {
            return false;
        }
    }
    return true;
}


static rf_v31_schedule_status_t run_one(
    const rf_v31_model_blob_t *model,
    const int8_t input[RF_V31_INPUT_BYTES],
    uint8_t *arena,
    size_t arena_bytes,
    rf_v31_invoke_fn invoke,
    void *context)
{
    if (!model_abi_is_valid(model)) {
        return RF_V31_SCHEDULE_MODEL_ABI_ERROR;
    }
    if (arena_bytes < model->scratch_bytes) {
        return RF_V31_SCHEDULE_ARENA_TOO_SMALL;
    }
    memcpy(arena + model->input_offset, input, RF_V31_INPUT_BYTES);
    if (!invoke(model, arena, arena_bytes, context)) {
        return RF_V31_SCHEDULE_INVOKE_FAILED;
    }
    return RF_V31_SCHEDULE_OK;
}


static void copy_output(
    const rf_v31_model_blob_t *model,
    uint32_t physical_index,
    const uint8_t *arena,
    int8_t destination[RF_V31_HEATMAP_BYTES])
{
    memcpy(
        destination,
        arena + model->output_offset[physical_index],
        RF_V31_HEATMAP_BYTES);
}


rf_v31_schedule_status_t rf_v31_run_selected_models(
    const int8_t q10_input[RF_V31_INPUT_BYTES],
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    int8_t heatmaps[RF_V31_CLASS_COUNT][RF_V31_HEATMAP_BYTES],
    rf_v31_invoke_fn invoke,
    void *context)
{
    rf_v31_schedule_status_t status;
    if (q10_input == NULL || shared_arena == NULL || heatmaps == NULL ||
        invoke == NULL) {
        return RF_V31_SCHEDULE_BAD_ARGUMENT;
    }
    if (shared_arena_bytes < RF_V31_SHARED_ARENA_BYTES) {
        return RF_V31_SCHEDULE_ARENA_TOO_SMALL;
    }

    status = run_one(
        &g_rf_v31_main_model,
        q10_input,
        shared_arena,
        shared_arena_bytes,
        invoke,
        context);
    if (status != RF_V31_SCHEDULE_OK) {
        return status;
    }
    /* Vela physical order is [4, 1, 3, 0, 2]. Only 4 and 2 are active. */
    copy_output(
        &g_rf_v31_main_model,
        0u,
        shared_arena,
        heatmaps[RF_V31_XIAOBAWANG]);
    copy_output(
        &g_rf_v31_main_model,
        4u,
        shared_arena,
        heatmaps[RF_V31_AT9S]);

    status = run_one(
        &g_rf_v31_dji_video_model,
        q10_input,
        shared_arena,
        shared_arena_bytes,
        invoke,
        context);
    if (status != RF_V31_SCHEDULE_OK) {
        return status;
    }
    copy_output(
        &g_rf_v31_dji_video_model,
        0u,
        shared_arena,
        heatmaps[RF_V31_DJI_VIDEO]);

    status = run_one(
        &g_rf_v31_dji_control_model,
        q10_input,
        shared_arena,
        shared_arena_bytes,
        invoke,
        context);
    if (status != RF_V31_SCHEDULE_OK) {
        return status;
    }
    copy_output(
        &g_rf_v31_dji_control_model,
        0u,
        shared_arena,
        heatmaps[RF_V31_DJI_CONTROL]);

    status = run_one(
        &g_rf_v31_t12_model,
        q10_input,
        shared_arena,
        shared_arena_bytes,
        invoke,
        context);
    if (status != RF_V31_SCHEDULE_OK) {
        return status;
    }
    copy_output(
        &g_rf_v31_t12_model,
        0u,
        shared_arena,
        heatmaps[RF_V31_T12]);
    return RF_V31_SCHEDULE_OK;
}
