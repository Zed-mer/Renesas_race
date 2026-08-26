#ifndef RF_V32_WIDTH_SCHEDULE_H
#define RF_V32_WIDTH_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>

#include "rf_v31_model_schedule.h"
#include "npu_model/rf_v32_width_model_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Invoke after V31 heatmaps have been copied out of the shared arena. */
rf_v31_schedule_status_t rf_v32_run_width_specialist(
    const int8_t roi_nhwc[RF_V32_WIDTH_INPUT_BYTES],
    uint8_t *shared_arena,
    size_t shared_arena_bytes,
    int8_t *output_code,
    rf_v31_invoke_fn invoke,
    void *context);

#ifdef __cplusplus
}
#endif

#endif
