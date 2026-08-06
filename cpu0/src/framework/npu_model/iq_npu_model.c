#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "ethosu_driver.h"
#include "hal_data.h"
#include "iq_npu_model.h"
#include "rf_v21_model_data.h"
#include "../rf_v12_sparse_contract.h"

#define IQ_NPU_BASE_ADDRESS_COUNT (2U)
#define IQ_NPU_DTCM __attribute__((section(".dtcm"), aligned(32), used))
#define IQ_NPU_SDRAM __attribute__((section(".sdram_noinit"), aligned(32), used))

_Static_assert(RF_V21_SHARED_ARENA_BYTES <=
               RF_V12_SHARED_ARENA_HARD_LIMIT_BYTES,
               "shared arena exceeds the RA8P1 hard limit");
_Static_assert(RF_V21_NONVIDEO_SCRATCH_BYTES == RF_V21_SHARED_ARENA_BYTES,
               "V21 non-video arena contract changed");
_Static_assert(RF_V21_V20_VIDEO_SCRATCH_BYTES <= RF_V21_SHARED_ARENA_BYTES,
               "V20 V3 exceeds the V21 shared arena");
_Static_assert(RF_V21_NONVIDEO_ARENA_INPUT_OFFSET + RF_V12_FEATURE_BYTES <=
               RF_V21_SHARED_ARENA_BYTES,
               "V21 non-video input exceeds the shared arena");
_Static_assert(RF_V21_V20_VIDEO_ARENA_INPUT_OFFSET + RF_V12_FEATURE_BYTES <=
               RF_V21_SHARED_ARENA_BYTES,
               "V20 V3 input exceeds the V21 shared arena");
_Static_assert(RF_V20_V3_VIDEO_OFFSET + RF_V12_HEATMAP_BYTES <=
               RF_V21_SHARED_ARENA_BYTES,
               "V20 V3 output exceeds the shared arena");
_Static_assert(RF_V21_NONVIDEO_AT9S_OFFSET + RF_V12_HEATMAP_BYTES <=
               RF_V21_SHARED_ARENA_BYTES,
               "V21 non-video output exceeds the shared arena");

static uint8_t s_iq_npu_arena[RF_V21_SHARED_ARENA_BYTES]
    __attribute__((section(".bss.iq_npu_arena"), aligned(32), used));
static int8_t s_iq_npu_heatmaps[RF_V12_CLASS_COUNT][RF_V12_HEATMAP_BYTES]
    IQ_NPU_DTCM;
static int8_t s_iq_npu_proof_input[RF_V12_FEATURE_BYTES] IQ_NPU_SDRAM;
static struct rt_mutex s_iq_npu_mutex;
static rt_bool_t s_iq_npu_mutex_ready;
static rt_bool_t s_iq_npu_opened;

volatile uint32_t g_npu_proof[4] __attribute__((used));
volatile iq_npu_benchmark_proof_t g_npu_benchmark __attribute__((used));

static int iq_npu_lock_init(void)
{
    if (s_iq_npu_mutex_ready)
    {
        return RT_EOK;
    }
    if (RT_EOK != rt_mutex_init(&s_iq_npu_mutex, "iqnpu", RT_IPC_FLAG_PRIO))
    {
        return -RT_ERROR;
    }
    s_iq_npu_mutex_ready = RT_TRUE;
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(iq_npu_lock_init);

int8_t *iq_npu_model_input(void)
{
    return (int8_t *)(s_iq_npu_arena + RF_V21_NONVIDEO_ARENA_INPUT_OFFSET);
}

const int8_t *iq_npu_model_heatmap(uint32_t class_id)
{
    return (class_id < RF_V12_CLASS_COUNT) ?
           s_iq_npu_heatmaps[class_id] : NULL;
}

int iq_npu_model_open(void)
{
    fsp_err_t error;

    if ((RT_EOK != iq_npu_lock_init()) ||
        (RT_EOK != rt_mutex_take(&s_iq_npu_mutex, RT_WAITING_FOREVER)))
    {
        return (int)FSP_ERR_INTERNAL;
    }
    if (s_iq_npu_opened)
    {
        rt_mutex_release(&s_iq_npu_mutex);
        return (int)FSP_SUCCESS;
    }
    error = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl, &g_rm_ethosu0_cfg);
    if (FSP_SUCCESS == error)
    {
        s_iq_npu_opened = RT_TRUE;
    }
    rt_mutex_release(&s_iq_npu_mutex);
    return (int)error;
}

static int iq_npu_invoke_blob(const uint8_t *command,
                              uint32_t command_bytes,
                              const uint8_t *weights,
                              uint32_t weight_bytes)
{
    uint64_t base_addresses[IQ_NPU_BASE_ADDRESS_COUNT] = {0U};
    size_t base_address_sizes[IQ_NPU_BASE_ADDRESS_COUNT] = {0U};

    base_addresses[0] = (uint64_t)(uintptr_t)weights;
    base_address_sizes[0] = weight_bytes;
    base_addresses[1] = (uint64_t)(uintptr_t)s_iq_npu_arena;
    base_address_sizes[1] = sizeof(s_iq_npu_arena);

    return ethosu_invoke_v3(&g_ethosu0,
                            command,
                            (int)command_bytes,
                            base_addresses,
                            base_address_sizes,
                            (int)IQ_NPU_BASE_ADDRESS_COUNT,
                             NULL);
}

static rt_bool_t iq_npu_cycle_counter_is_enabled(void)
{
    return (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
            ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
            ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U)) ?
           RT_TRUE : RT_FALSE;
}

static uint32_t iq_npu_cycle_now_fast(void)
{
    __asm volatile ("" ::: "memory");
    return DWT->CYCCNT;
}

int iq_npu_model_invoke(const int8_t *immutable_features,
                        uint32_t feature_bytes,
                        iq_npu_stage_cycles_t *stage_cycles)
{
    int result;
    uint32_t stage_start = 0U;
    rt_bool_t timing_enabled;
    uintptr_t input_address = (uintptr_t)immutable_features;
    uintptr_t arena_start = (uintptr_t)s_iq_npu_arena;
    uintptr_t arena_end = arena_start + sizeof(s_iq_npu_arena);

    if ((immutable_features == NULL) ||
        (feature_bytes != RF_V12_FEATURE_BYTES) ||
        ((input_address >= arena_start) && (input_address < arena_end)))
    {
        return -1;
    }
    if (stage_cycles != NULL)
    {
        memset(stage_cycles, 0, sizeof(*stage_cycles));
    }
    result = iq_npu_model_open();
    if (result != 0)
    {
        return -2;
    }
    if (RT_EOK != rt_mutex_take(&s_iq_npu_mutex, RT_WAITING_FOREVER))
    {
        return -3;
    }

    timing_enabled = ((stage_cycles != NULL) &&
                      iq_npu_cycle_counter_is_enabled()) ?
                     RT_TRUE : RT_FALSE;
    if (timing_enabled)
    {
        stage_start = iq_npu_cycle_now_fast();
    }
    memcpy(s_iq_npu_arena + g_rf_v21_nonvideo_model.input_offset,
           immutable_features,
           feature_bytes);
    if (timing_enabled)
    {
        stage_cycles->v2_input_copy_cycles =
            iq_npu_cycle_now_fast() - stage_start;
    }
    __DMB();
    if (timing_enabled)
    {
        stage_start = iq_npu_cycle_now_fast();
    }
    result = iq_npu_invoke_blob(g_rf_v21_nonvideo_model.command,
                                g_rf_v21_nonvideo_model.command_bytes,
                                g_rf_v21_nonvideo_model.weights,
                                g_rf_v21_nonvideo_model.weight_bytes);
    __DMB();
    if (timing_enabled)
    {
        stage_cycles->v2_invoke_cycles =
            iq_npu_cycle_now_fast() - stage_start;
    }
    if (result == 0)
    {
        if (timing_enabled)
        {
            stage_start = iq_npu_cycle_now_fast();
        }
        memcpy(s_iq_npu_heatmaps[RF_V12_CLASS_XIAOBAWANG],
               s_iq_npu_arena +
                   g_rf_v21_nonvideo_model.output_offset[
                       RF_V21_NONVIDEO_OUTPUT_XIAOBAWANG],
               RF_V12_HEATMAP_BYTES);
        memcpy(s_iq_npu_heatmaps[RF_V12_CLASS_T12],
               s_iq_npu_arena +
                   g_rf_v21_nonvideo_model.output_offset[
                       RF_V21_NONVIDEO_OUTPUT_T12],
               RF_V12_HEATMAP_BYTES);
        memcpy(s_iq_npu_heatmaps[RF_V12_CLASS_DJI_CONTROL],
               s_iq_npu_arena +
                   g_rf_v21_nonvideo_model.output_offset[
                       RF_V21_NONVIDEO_OUTPUT_DJI_CONTROL],
               RF_V12_HEATMAP_BYTES);
        memcpy(s_iq_npu_heatmaps[RF_V12_CLASS_AT9S],
               s_iq_npu_arena +
                   g_rf_v21_nonvideo_model.output_offset[
                       RF_V21_NONVIDEO_OUTPUT_AT9S],
               RF_V12_HEATMAP_BYTES);
        if (timing_enabled)
        {
            stage_cycles->v2_output_copy_cycles =
                iq_npu_cycle_now_fast() - stage_start;
            stage_start = iq_npu_cycle_now_fast();
        }

        memcpy(s_iq_npu_arena + g_rf_v21_v20_video_model.input_offset,
               immutable_features,
               feature_bytes);
        if (timing_enabled)
        {
            stage_cycles->v3_input_copy_cycles =
                iq_npu_cycle_now_fast() - stage_start;
        }
        __DMB();
        if (timing_enabled)
        {
            stage_start = iq_npu_cycle_now_fast();
        }
        result = iq_npu_invoke_blob(g_rf_v21_v20_video_model.command,
                                    g_rf_v21_v20_video_model.command_bytes,
                                    g_rf_v21_v20_video_model.weights,
                                    g_rf_v21_v20_video_model.weight_bytes);
        __DMB();
        if (timing_enabled)
        {
            stage_cycles->v3_invoke_cycles =
                iq_npu_cycle_now_fast() - stage_start;
        }
        if (result == 0)
        {
            if (timing_enabled)
            {
                stage_start = iq_npu_cycle_now_fast();
            }
            memcpy(s_iq_npu_heatmaps[RF_V12_CLASS_DJI_VIDEO],
                   s_iq_npu_arena +
                       g_rf_v21_v20_video_model.output_offset[0],
                   RF_V12_HEATMAP_BYTES);
            if (timing_enabled)
            {
                stage_cycles->v3_output_copy_cycles =
                    iq_npu_cycle_now_fast() - stage_start;
            }
        }
    }

    if (stage_cycles != NULL)
    {
        stage_cycles->timing_valid =
            (timing_enabled && (result == 0) &&
             iq_npu_cycle_counter_is_enabled()) ? 1U : 0U;
    }

    rt_mutex_release(&s_iq_npu_mutex);
    return result;
}

static uint32_t iq_npu_checksum(void)
{
    uint32_t checksum = 2166136261U;
    for (uint32_t class_id = 0U; class_id < RF_V12_CLASS_COUNT; ++class_id)
    {
        for (uint32_t i = 0U; i < RF_V12_HEATMAP_BYTES; ++i)
        {
            checksum = (checksum ^
                        (uint8_t)s_iq_npu_heatmaps[class_id][i]) *
                       16777619U;
        }
    }
    return checksum;
}

static rt_bool_t iq_npu_cycle_counter_enable(void)
{
    if (iq_npu_cycle_counter_is_enabled())
    {
        return RT_TRUE;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0UL) = 0xC5ACCE55UL;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        return RT_FALSE;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    return RT_TRUE;
}

static void iq_npu_sort_cycles(uint32_t cycles[IQ_NPU_BENCHMARK_RUNS])
{
    for (uint32_t i = 1U; i < IQ_NPU_BENCHMARK_RUNS; ++i)
    {
        uint32_t value = cycles[i];
        uint32_t j = i;
        while ((j > 0U) && (cycles[j - 1U] > value))
        {
            cycles[j] = cycles[j - 1U];
            --j;
        }
        cycles[j] = value;
    }
}

static void iq_npu_fill_proof_input(void)
{
    for (uint32_t i = 0U; i < RF_V12_FEATURE_BYTES; ++i)
    {
        s_iq_npu_proof_input[i] =
            (int8_t)(uint8_t)((i * 17U) + 3U);
    }
}

static int iq_npu_run_proof(rt_bool_t verbose)
{
    uint32_t sorted_cycles[IQ_NPU_BENCHMARK_RUNS];
    uint32_t start_cycles;
    int result;

    memset((void *)&g_npu_benchmark, 0, sizeof(g_npu_benchmark));
    g_npu_proof[0] = IQ_NPU_PROOF_START_MAGIC;
    g_npu_proof[1] = UINT32_MAX;
    g_npu_proof[2] = 0U;
    g_npu_proof[3] = 0U;
    g_npu_benchmark.runs = IQ_NPU_BENCHMARK_RUNS;
    g_npu_benchmark.core_clock_hz = SystemCoreClock;

    result = iq_npu_model_open();
    g_npu_proof[1] = (uint32_t)result;
    if (result != 0)
    {
        g_npu_proof[0] = IQ_NPU_PROOF_OPEN_ERROR_MAGIC;
        return result;
    }
    if (!iq_npu_cycle_counter_enable())
    {
        g_npu_proof[0] = IQ_NPU_PROOF_INVOKE_ERROR_MAGIC;
        return -4;
    }

    iq_npu_fill_proof_input();
    result = iq_npu_model_invoke(s_iq_npu_proof_input,
                                 RF_V12_FEATURE_BYTES,
                                 NULL);
    if (result != 0)
    {
        g_npu_proof[0] = IQ_NPU_PROOF_INVOKE_ERROR_MAGIC;
        return result;
    }

    for (uint32_t i = 0U; i < IQ_NPU_BENCHMARK_RUNS; ++i)
    {
        __DSB();
        start_cycles = DWT->CYCCNT;
        result = iq_npu_model_invoke(s_iq_npu_proof_input,
                                     RF_V12_FEATURE_BYTES,
                                     NULL);
        __DSB();
        g_npu_benchmark.samples[i] = DWT->CYCCNT - start_cycles;
        sorted_cycles[i] = g_npu_benchmark.samples[i];
        if (result != 0)
        {
            g_npu_proof[0] = IQ_NPU_PROOF_INVOKE_ERROR_MAGIC;
            return result;
        }
    }

    iq_npu_sort_cycles(sorted_cycles);
    g_npu_benchmark.min_cycles = sorted_cycles[0];
    g_npu_benchmark.median_cycles =
        sorted_cycles[IQ_NPU_BENCHMARK_RUNS / 2U];
    g_npu_benchmark.max_cycles =
        sorted_cycles[IQ_NPU_BENCHMARK_RUNS - 1U];
    g_npu_benchmark.checksum = iq_npu_checksum();
    __DMB();
    g_npu_benchmark.magic = IQ_NPU_BENCHMARK_PASS_MAGIC;
    g_npu_proof[2] = g_npu_benchmark.median_cycles;
    g_npu_proof[3] = g_npu_benchmark.checksum;
    __DMB();
    g_npu_proof[0] = IQ_NPU_PROOF_PASS_MAGIC;

    if (verbose)
    {
        rt_kprintf("npu_test: unchanged V2 + V20 V3 Ethos-U55 models\n");
        rt_kprintf("npu_test: input=204x115x4 int8, heatmaps=5x102x58\n");
        rt_kprintf("npu_test: runs=%lu, min/median/max=%lu/%lu/%lu cycles\n",
                   (unsigned long)IQ_NPU_BENCHMARK_RUNS,
                   (unsigned long)g_npu_benchmark.min_cycles,
                   (unsigned long)g_npu_benchmark.median_cycles,
                   (unsigned long)g_npu_benchmark.max_cycles);
    }
    return 0;
}

int iq_npu_model_run_proof(void)
{
    return iq_npu_run_proof(RT_FALSE);
}

static void iq_npu_test_cmd(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    (void)iq_npu_run_proof(RT_TRUE);
}
MSH_CMD_EXPORT_ALIAS(iq_npu_test_cmd, npu_test, run the V2 plus V20 V3 models);
