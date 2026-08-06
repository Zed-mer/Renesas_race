#ifndef NPU_RUNNER_H
#define NPU_RUNNER_H

#include <stdbool.h>
#include <stdint.h>

#include "rf_v12_sparse_contract.h"

#define NPU_RUNNER_INPUT_BYTES (RF_V12_FEATURE_BYTES)
#define NPU_RUNNER_CLASS_COUNT (RF_V12_CLASS_COUNT)
#define NPU_RUNNER_HEATMAP_BYTES (RF_V12_HEATMAP_BYTES)

typedef struct st_npu_runner_stats
{
    uint32_t ready;
    uint32_t inference_count;
    uint32_t last_cycles;
    uint32_t last_v2_input_copy_cycles;
    uint32_t last_v2_invoke_cycles;
    uint32_t last_v2_output_copy_cycles;
    uint32_t last_v3_input_copy_cycles;
    uint32_t last_v3_invoke_cycles;
    uint32_t last_v3_output_copy_cycles;
    uint32_t last_stage_timing_valid;
    uint32_t last_class;
    int32_t last_score_q15;
    uint32_t mask_valid;
    int32_t last_error;
    uint32_t last_timing_valid;
    uint32_t last_dwt_recovered;
    uint32_t dwt_recovery_count;
} npu_runner_stats_t;

void npu_runner_init(void);
bool npu_runner_infer(const void *features, uint32_t feature_bytes);
void npu_runner_result_set(uint32_t class_id, uint16_t score_q15);
void npu_runner_stats_get(npu_runner_stats_t *stats);
const int8_t *npu_runner_heatmap(uint32_t class_id);

#endif
