#ifndef RF_V24_T12_RUNTIME_H
#define RF_V24_T12_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "npu_model/rf_v24_t12_model_data.h"
#include "rf_v24_t12_postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V24_T12_INPUT_BYTES 93840u

typedef int (*rf_v24_t12_npu_invoke_fn)(
    const rf_v21_model_blob_t *model,
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    void *context
);

/*
 * Call after V21 has completed and its T12 output has been copied. The
 * specialist output remains at arena offset 0 only until the next NPU call.
 */
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
    void *invoke_context
);

#ifdef __cplusplus
}
#endif

#endif
