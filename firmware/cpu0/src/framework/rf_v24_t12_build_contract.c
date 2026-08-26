#include "rf_v24_t12_build_contract.h"

#include "npu_model/rf_v24_t12_model_data.h"
#include "rf_v24_t12_postprocess.h"

/* CPU1 keeps the active V27 state machine.  The V24 model contract still
 * records the tested T12 hold value for offline/ELF contract inspection. */
#ifndef RF_V24_T12_WORKING_EXIT_MISS_ROUNDS
#define RF_V24_T12_WORKING_EXIT_MISS_ROUNDS UINT32_C(6)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RF_V24_T12_CONTRACT_ATTRIBUTES \
    __attribute__((used, aligned(4)))
#else
#define RF_V24_T12_CONTRACT_ATTRIBUTES
#endif

const rf_v24_t12_build_contract_t g_rf_v24_t12_build_contract
    RF_V24_T12_CONTRACT_ATTRIBUTES = {
        RF_V24_T12_BUILD_CONTRACT_MAGIC,
        RF_V24_T12_BUILD_CONTRACT_WORDS,
        UINT32_C(24),
        UINT32_C(12),
        UINT32_C(204),
        UINT32_C(115),
        UINT32_C(4),
        RF_V24_T12_HEATMAP_FREQUENCY_BINS,
        RF_V24_T12_HEATMAP_TIME_BINS,
        UINT32_C(1),
        UINT32_C(93840),
        RF_V24_T12_SPECIALIST_OUTPUT_BYTES,
        RF_V24_T12_SHARED_ARENA_BYTES,
        RF_V24_T12_SPECIALIST_SCRATCH_BYTES,
        RF_V24_T12_SPECIALIST_INPUT_OFFSET,
        RF_V24_T12_SPECIALIST_OUTPUT_OFFSET,
        RF_V24_T12_SPECIALIST_COMMAND_BYTES,
        RF_V24_T12_SPECIALIST_WEIGHT_BYTES,
        UINT32_C(139921665),
        INT32_C(-1),
        UINT32_C(157624096),
        INT32_C(89),
        UINT32_C(3),
        RF_V24_T12_EVENT_THRESHOLD_Q15,
        (uint32_t)RF_V24_T12_BANDWIDTH_HZ,
        (uint32_t)RF_V24_T12_DURATION_SAMPLES,
        RF_V24_T12_WORKING_EXIT_MISS_ROUNDS,
};
