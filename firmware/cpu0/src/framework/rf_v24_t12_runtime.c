#include "rf_v24_t12_runtime.h"

#include <string.h>

size_t rf_v24_t12_run_specialist(
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    const int8_t *immutable_input,
    size_t immutable_input_bytes,
    const int8_t *v21_t12_heatmap,
    size_t v21_t12_heatmap_bytes,
    uint64_t capture_center_frequency_hz,
    rf_v24_t12_event_t *events,
    size_t event_capacity,
    rf_v24_t12_npu_invoke_fn invoke,
    void *invoke_context)
{
    const int8_t *specialist_output;
    if (shared_arena == NULL || immutable_input == NULL ||
        v21_t12_heatmap == NULL || events == NULL || invoke == NULL ||
        (capture_center_frequency_hz != UINT64_C(2420000000) &&
         capture_center_frequency_hz != UINT64_C(2464000000)) ||
        shared_arena_bytes < RF_V24_T12_SHARED_ARENA_BYTES ||
        immutable_input_bytes != RF_V24_T12_INPUT_BYTES ||
        v21_t12_heatmap_bytes != RF_V24_T12_HEATMAP_BYTES ||
        RF_V24_T12_SPECIALIST_INPUT_OFFSET + RF_V24_T12_INPUT_BYTES >
            shared_arena_bytes ||
        RF_V24_T12_SPECIALIST_OUTPUT_OFFSET + RF_V24_T12_HEATMAP_BYTES >
            shared_arena_bytes) {
        return 0u;
    }
    memcpy(
        shared_arena + RF_V24_T12_SPECIALIST_INPUT_OFFSET,
        immutable_input,
        RF_V24_T12_INPUT_BYTES);
    if (!invoke(
            &g_rf_v24_t12_specialist_model,
            shared_arena,
            shared_arena_bytes,
            invoke_context)) {
        return 0u;
    }
    specialist_output = (const int8_t *)(
        shared_arena + RF_V24_T12_SPECIALIST_OUTPUT_OFFSET);
    return rf_v24_t12_postprocess(
        v21_t12_heatmap,
        v21_t12_heatmap_bytes,
        specialist_output,
        RF_V24_T12_HEATMAP_BYTES,
        capture_center_frequency_hz,
        events,
        event_capacity);
}
