#ifndef RF_V24_T12_BUILD_CONTRACT_H
#define RF_V24_T12_BUILD_CONTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_V24_T12_BUILD_CONTRACT_MAGIC UINT32_C(0x56325431)
#define RF_V24_T12_BUILD_CONTRACT_WORDS 27u

typedef struct rf_v24_t12_build_contract {
    uint32_t magic;
    uint32_t word_count;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t input_frequency_bins;
    uint32_t input_time_bins;
    uint32_t input_channels;
    uint32_t output_frequency_bins;
    uint32_t output_time_bins;
    uint32_t output_channels;
    uint32_t input_bytes;
    uint32_t output_bytes;
    uint32_t shared_arena_bytes;
    uint32_t specialist_scratch_bytes;
    uint32_t specialist_input_offset;
    uint32_t specialist_output_offset;
    uint32_t specialist_command_bytes;
    uint32_t specialist_weight_bytes;
    uint32_t input_scale_x1e9;
    int32_t input_zero_point;
    uint32_t output_scale_x1e9;
    int32_t output_zero_point;
    uint32_t t12_logical_class_index;
    uint32_t event_threshold_q15;
    uint32_t fixed_bandwidth_hz;
    uint32_t fixed_duration_samples;
    uint32_t t12_exit_miss_rounds;
} rf_v24_t12_build_contract_t;

extern const rf_v24_t12_build_contract_t g_rf_v24_t12_build_contract;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(
    sizeof(rf_v24_t12_build_contract_t) ==
        RF_V24_T12_BUILD_CONTRACT_WORDS * sizeof(uint32_t),
    "V24 T12 build contract layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
