#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "ethosu_driver.h"
#include "hal_data.h"
#include "iq_npu_model.h"
#include "../rf_v31_detection_contract.h"
#include "../rf_v31_model_schedule.h"
#include "../rf_v32_video_width.h"
#include "../rf_v32_width_schedule.h"

#define IQ_NPU_BASE_ADDRESS_COUNT (2U)
#define IQ_NPU_DTCM __attribute__((section(".dtcm"), aligned(32), used))
#define IQ_NPU_SDRAM __attribute__((section(".sdram_noinit"), aligned(32), used))

_Static_assert(RF_V31_SHARED_ARENA_BYTES <=
               RF_V12_SHARED_ARENA_HARD_LIMIT_BYTES,
               "V31 shared arena exceeds the RA8P1 hard limit");
_Static_assert(RF_V31_INPUT_BYTES == RF_V12_FEATURE_BYTES,
               "V31 input size changed");
_Static_assert(RF_V31_HEATMAP_BYTES == RF_V12_HEATMAP_BYTES,
               "V31 heatmap size changed");
_Static_assert((uint32_t)RF_V31_CLASS_COUNT ==
               (uint32_t)RF_V12_CLASS_COUNT,
               "V31 logical class count changed");

static uint8_t s_iq_npu_arena[RF_V31_SHARED_ARENA_BYTES]
    __attribute__((section(".bss.iq_npu_arena"), aligned(32), used));
static int8_t s_iq_npu_heatmaps[RF_V12_CLASS_COUNT][RF_V12_HEATMAP_BYTES]
    IQ_NPU_DTCM;
static int8_t s_iq_npu_proof_input[RF_V12_FEATURE_BYTES] IQ_NPU_SDRAM;
static struct rt_mutex s_iq_npu_mutex;
static rt_bool_t s_iq_npu_mutex_ready;
static rt_bool_t s_iq_npu_opened;

volatile uint32_t g_npu_proof[4] __attribute__((used));
volatile iq_npu_benchmark_proof_t g_npu_benchmark __attribute__((used));

typedef struct st_iq_npu_invoke_context
{
    int result;
} iq_npu_invoke_context_t;

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
    /* V31 requires caller-owned input outside the shared arena. */
    return NULL;
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

static int iq_npu_invoke_blob(const rf_v31_model_blob_t *model)
{
    uint64_t base_addresses[IQ_NPU_BASE_ADDRESS_COUNT] = {0U};
    size_t base_address_sizes[IQ_NPU_BASE_ADDRESS_COUNT] = {0U};
    int result;

    if (model == NULL)
    {
        return -1;
    }
    base_addresses[0] = (uint64_t)(uintptr_t)model->weights;
    base_address_sizes[0] = model->weight_bytes;
    base_addresses[1] = (uint64_t)(uintptr_t)s_iq_npu_arena;
    base_address_sizes[1] = sizeof(s_iq_npu_arena);
    __DMB();
    result = ethosu_invoke_v3(&g_ethosu0,
                              model->command,
                              (int)model->command_bytes,
                              base_addresses,
                              base_address_sizes,
                              (int)IQ_NPU_BASE_ADDRESS_COUNT,
                              NULL);
    __DMB();
    return result;
}

static bool iq_npu_invoke_v31(const rf_v31_model_blob_t *model,
                              uint8_t *shared_arena,
                              size_t shared_arena_bytes,
                              void *context)
{
    iq_npu_invoke_context_t *invoke_context =
        (iq_npu_invoke_context_t *)context;
    if ((model == NULL) || (invoke_context == NULL) ||
        (shared_arena != s_iq_npu_arena) ||
        (shared_arena_bytes != sizeof(s_iq_npu_arena)))
    {
        return false;
    }
    invoke_context->result = iq_npu_invoke_blob(model);
    return invoke_context->result == 0;
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

int iq_npu_model_invoke_with_absolute_at_frequency(
    const int8_t *immutable_features,
    const int8_t *absolute_features,
    uint32_t feature_bytes,
    uint64_t capture_center_frequency_hz,
    iq_npu_stage_cycles_t *stage_cycles)
{
    iq_npu_invoke_context_t invoke_context = {-1};
    rf_v31_schedule_status_t status;
    uint32_t stage_start = 0U;
    rt_bool_t timing_enabled;
    const uintptr_t input_address = (uintptr_t)immutable_features;
    const uintptr_t arena_start = (uintptr_t)s_iq_npu_arena;
    const uintptr_t arena_end = arena_start + sizeof(s_iq_npu_arena);

    (void)absolute_features;
    (void)capture_center_frequency_hz;
    if ((immutable_features == NULL) ||
        (feature_bytes != RF_V31_INPUT_BYTES) ||
        ((input_address >= arena_start) && (input_address < arena_end)))
    {
        return -1;
    }
    if (stage_cycles != NULL)
    {
        memset(stage_cycles, 0, sizeof(*stage_cycles));
    }
    if (iq_npu_model_open() != 0)
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
    status = rf_v31_run_selected_models(
        immutable_features,
        s_iq_npu_arena,
        sizeof(s_iq_npu_arena),
        s_iq_npu_heatmaps,
        iq_npu_invoke_v31,
        &invoke_context);
    if (timing_enabled)
    {
        stage_cycles->v2_invoke_cycles =
            iq_npu_cycle_now_fast() - stage_start;
        stage_cycles->timing_valid =
            ((status == RF_V31_SCHEDULE_OK) &&
             iq_npu_cycle_counter_is_enabled()) ? 1U : 0U;
    }
    rt_mutex_release(&s_iq_npu_mutex);
    if (status != RF_V31_SCHEDULE_OK)
    {
        return -100 - (int)status;
    }
    return invoke_context.result;
}

int iq_npu_model_invoke_with_absolute(
    const int8_t *immutable_features,
    const int8_t *absolute_features,
    uint32_t feature_bytes,
    iq_npu_stage_cycles_t *stage_cycles)
{
    return iq_npu_model_invoke_with_absolute_at_frequency(
        immutable_features,
        absolute_features,
        feature_bytes,
        0U,
        stage_cycles);
}

int iq_npu_model_invoke(const int8_t *immutable_features,
                        uint32_t feature_bytes,
                        iq_npu_stage_cycles_t *stage_cycles)
{
    return iq_npu_model_invoke_with_absolute(
        immutable_features,
        immutable_features,
        feature_bytes,
        stage_cycles);
}

static int32_t iq_npu_round_divide_i64(int64_t numerator, int64_t denominator)
{
    if (numerator >= 0)
    {
        return (int32_t)((numerator + denominator / 2) / denominator);
    }
    return (int32_t)(-((-numerator + denominator / 2) / denominator));
}

int iq_npu_model_classify_video_width(
    const int8_t *features,
    uint32_t feature_bytes,
    int32_t center_frequency_offset_hz,
    int32_t center_sample,
    rf_v32_width_track_t *track,
    int32_t *bandwidth_hz)
{
    iq_npu_invoke_context_t invoke_context = {-1};
    rf_v32_cpu_width_evidence_t cpu_evidence;
    rf_v31_schedule_status_t status = RF_V31_SCHEDULE_INVOKE_FAILED;
    rt_bool_t npu_ready;
    int8_t output_code = 0;
    int32_t center_row_q8;
    int32_t center_column_q8;
    int32_t selected_width;
    int8_t *roi;

    if ((features == NULL) || (track == NULL) || (bandwidth_hz == NULL) ||
        (feature_bytes != RF_V32_SOURCE_BYTES) ||
        (center_sample < 0) ||
        (center_sample > (int32_t)RF_V12_TILE_SAMPLES))
    {
        return -1;
    }
    if (RT_EOK != iq_npu_lock_init())
    {
        return -2;
    }
    npu_ready = (iq_npu_model_open() == 0) ? RT_TRUE : RT_FALSE;
    if (RT_EOK != rt_mutex_take(&s_iq_npu_mutex, RT_WAITING_FOREVER))
    {
        return -3;
    }

    center_row_q8 = iq_npu_round_divide_i64(
        (int64_t)(center_frequency_offset_hz + INT32_C(28000000)) *
            RF_V32_SOURCE_FREQUENCY_BINS * 256,
        INT32_C(56000000)) - 128;
    center_column_q8 = iq_npu_round_divide_i64(
        (int64_t)center_sample * RF_V32_SOURCE_TIME_BINS * 256,
        RF_V12_TILE_SAMPLES) - 128;
    roi = (int8_t *)(s_iq_npu_arena + RF_V32_WIDTH_INPUT_OFFSET);
    if (!rf_v32_extract_width_roi(features,
                                  feature_bytes,
                                  center_row_q8,
                                  center_column_q8,
                                  roi,
                                  RF_V32_WIDTH_INPUT_BYTES))
    {
        rt_mutex_release(&s_iq_npu_mutex);
        return -4;
    }
    memset(&cpu_evidence, 0, sizeof(cpu_evidence));
    (void)rf_v32_cpu_width_classify(
        roi, RF_V32_WIDTH_INPUT_BYTES, &cpu_evidence);
    if (npu_ready)
    {
        status = rf_v32_run_width_specialist(
            roi,
            s_iq_npu_arena,
            sizeof(s_iq_npu_arena),
            &output_code,
            iq_npu_invoke_v31,
            &invoke_context);
    }
    selected_width = rf_v32_width_track_apply(
        track,
        cpu_evidence.available != 0U,
        cpu_evidence.bandwidth_hz,
        (npu_ready && (status == RF_V31_SCHEDULE_OK)) ? 1 : 0,
        output_code);
    rt_mutex_release(&s_iq_npu_mutex);
    if (selected_width == 0)
    {
        return -5;
    }
    *bandwidth_hz = selected_width;
    return 0;
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
    result = iq_npu_model_invoke(
        s_iq_npu_proof_input, RF_V12_FEATURE_BYTES, NULL);
    if (result != 0)
    {
        g_npu_proof[0] = IQ_NPU_PROOF_INVOKE_ERROR_MAGIC;
        return result;
    }
    for (uint32_t i = 0U; i < IQ_NPU_BENCHMARK_RUNS; ++i)
    {
        __DSB();
        start_cycles = DWT->CYCCNT;
        result = iq_npu_model_invoke(
            s_iq_npu_proof_input, RF_V12_FEATURE_BYTES, NULL);
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
        rt_kprintf("npu_test: V31 Q10 four-model Ethos-U55 schedule\n");
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
MSH_CMD_EXPORT_ALIAS(iq_npu_test_cmd, npu_test, run V31 Q10 models);
