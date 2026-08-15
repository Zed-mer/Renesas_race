#include "npu_runner.h"

#include <string.h>
#include "hal_data.h"
#include "npu_model/iq_npu_model.h"

static npu_runner_stats_t g_npu_stats;
static uint32_t g_dwt_ready;

static bool npu_dwt_enabled(void)
{
    return ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U) &&
           ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U) &&
           ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
}

static uint32_t npu_cycle_now(void)
{
    /* CYCCNT is a volatile timestamp, not a memory-ownership boundary.  The
     * model wrapper keeps the DMBs that order NPU arena accesses. */
    __asm volatile ("" ::: "memory");
    return DWT->CYCCNT;
}

static bool npu_enable_dwt(bool *recovered)
{
    if (recovered != NULL)
    {
        *recovered = false;
    }
    if ((g_dwt_ready != 0U) && npu_dwt_enabled())
    {
        return true;
    }
    if ((g_dwt_ready != 0U) && (recovered != NULL))
    {
        *recovered = true;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *((volatile uint32_t *)0xE0001FB0UL) = 0xC5ACCE55UL;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)
    {
        return false;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    g_dwt_ready = npu_dwt_enabled() ? 1U : 0U;
    return g_dwt_ready != 0U;
}

void npu_runner_init(void)
{
    int result;
    memset(&g_npu_stats, 0, sizeof(g_npu_stats));
    result = iq_npu_model_open();
    if (result == 0)
    {
        /* Produce reset-stable hardware proof in the normal RT-Thread startup
         * context.  Debugger-injected calls do not preserve the IRQ/RTOS
         * execution semantics required by the Ethos-U driver. */
        result = iq_npu_model_run_proof();
    }
    g_npu_stats.last_error = result;
    g_npu_stats.ready = (result == 0) ? 1U : 0U;
    g_npu_stats.mask_valid = 0U;
    (void)npu_enable_dwt(NULL);
}

bool npu_runner_infer(const void *features, uint32_t feature_bytes)
{
    int result;
    iq_npu_stage_cycles_t stages;
    uint32_t start;
    uint32_t end;
    bool recovered_start = false;
    bool recovered_end = false;

    g_npu_stats.last_timing_valid = 0U;
    g_npu_stats.last_dwt_recovered = 0U;
    g_npu_stats.last_v2_input_copy_cycles = 0U;
    g_npu_stats.last_v2_invoke_cycles = 0U;
    g_npu_stats.last_v2_output_copy_cycles = 0U;
    g_npu_stats.last_v3_input_copy_cycles = 0U;
    g_npu_stats.last_v3_invoke_cycles = 0U;
    g_npu_stats.last_v3_output_copy_cycles = 0U;
    g_npu_stats.last_stage_timing_valid = 0U;
    memset(&stages, 0, sizeof(stages));

    if ((features == NULL) || (feature_bytes != NPU_RUNNER_INPUT_BYTES) ||
        !npu_enable_dwt(&recovered_start))
    {
        g_npu_stats.last_error = -1;
        return false;
    }

    start = npu_cycle_now();
    result = iq_npu_model_invoke((const int8_t *)features,
                                 feature_bytes,
                                 &stages);
    if (!npu_enable_dwt(&recovered_end))
    {
        g_npu_stats.last_cycles = 0U;
        g_npu_stats.last_error = -2;
        return false;
    }
    end = npu_cycle_now();
    g_npu_stats.last_dwt_recovered = (recovered_start || recovered_end) ? 1U : 0U;
    if (g_npu_stats.last_dwt_recovered != 0U)
    {
        g_npu_stats.dwt_recovery_count++;
    }
    if (!recovered_end)
    {
        g_npu_stats.last_cycles = end - start;
        g_npu_stats.last_timing_valid = 1U;
    }
    else
    {
        g_npu_stats.last_cycles = 0U;
    }
    if (!recovered_end && (stages.timing_valid != 0U))
    {
        g_npu_stats.last_v2_input_copy_cycles =
            stages.v2_input_copy_cycles;
        g_npu_stats.last_v2_invoke_cycles = stages.v2_invoke_cycles;
        g_npu_stats.last_v2_output_copy_cycles =
            stages.v2_output_copy_cycles;
        g_npu_stats.last_v3_input_copy_cycles =
            stages.v3_input_copy_cycles;
        g_npu_stats.last_v3_invoke_cycles = stages.v3_invoke_cycles;
        g_npu_stats.last_v3_output_copy_cycles =
            stages.v3_output_copy_cycles;
        g_npu_stats.last_stage_timing_valid = 1U;
    }
    g_npu_stats.last_error = result;
    if (result != 0)
    {
        g_npu_stats.ready = 0U;
        g_npu_stats.mask_valid = 0U;
        return false;
    }

    g_npu_stats.ready = 1U;
    g_npu_stats.mask_valid = 1U;
    g_npu_stats.inference_count++;
    return true;
}

void npu_runner_result_set(uint32_t class_id, uint16_t score_q15)
{
    if (class_id < NPU_RUNNER_CLASS_COUNT)
    {
        g_npu_stats.last_class = class_id;
        g_npu_stats.last_score_q15 = (int32_t)score_q15;
    }
}

void npu_runner_stats_get(npu_runner_stats_t *stats)
{
    if (stats != NULL)
    {
        *stats = g_npu_stats;
    }
}

const int8_t *npu_runner_heatmap(uint32_t class_id)
{
    return iq_npu_model_heatmap(class_id);
}
