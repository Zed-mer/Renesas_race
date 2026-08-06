#ifndef IQ_NPU_MODEL_H
#define IQ_NPU_MODEL_H

#include <stdint.h>

#include "../rf_v12_sparse_contract.h"

#define IQ_NPU_PROOF_START_MAGIC        (0x4E505501UL)
#define IQ_NPU_PROOF_OPEN_ERROR_MAGIC   (0x4E5055E0UL)
#define IQ_NPU_PROOF_INVOKE_ERROR_MAGIC (0x4E5055E1UL)
#define IQ_NPU_PROOF_PASS_MAGIC         (0x4E5055A5UL)
#define IQ_NPU_BENCHMARK_PASS_MAGIC     (0x4E5042A5UL)
#define IQ_NPU_BENCHMARK_RUNS           (7U)

#define IQ_NPU_INPUT_ELEMENTS           (RF_V12_FEATURE_BYTES)
#define IQ_NPU_INPUT_SCALE              (RF_V12_INPUT_SCALE)
#define IQ_NPU_INPUT_ZERO_POINT         (RF_V12_INPUT_ZERO_POINT)

typedef struct iq_npu_benchmark_proof
{
    uint32_t magic;
    uint32_t runs;
    uint32_t core_clock_hz;
    uint32_t min_cycles;
    uint32_t median_cycles;
    uint32_t max_cycles;
    uint32_t checksum;
    uint32_t samples[IQ_NPU_BENCHMARK_RUNS];
} iq_npu_benchmark_proof_t;

typedef struct iq_npu_stage_cycles
{
    uint32_t v2_input_copy_cycles;
    uint32_t v2_invoke_cycles;
    uint32_t v2_output_copy_cycles;
    uint32_t v3_input_copy_cycles;
    uint32_t v3_invoke_cycles;
    uint32_t v3_output_copy_cycles;
    uint32_t timing_valid;
} iq_npu_stage_cycles_t;

extern volatile uint32_t g_npu_proof[4];
extern volatile iq_npu_benchmark_proof_t g_npu_benchmark;

int iq_npu_model_open(void);
int iq_npu_model_invoke(const int8_t *immutable_features,
                        uint32_t feature_bytes,
                        iq_npu_stage_cycles_t *stage_cycles);
int iq_npu_model_run_proof(void);
int8_t *iq_npu_model_input(void);
const int8_t *iq_npu_model_heatmap(uint32_t class_id);

#endif
