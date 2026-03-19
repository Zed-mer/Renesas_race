#include "drv_emg_adc.h"
#include <stdbool.h>
#include <string.h>

#define EMG_ADC_WAIT_TIMEOUT_US 5000U
#define EMG_ADC_WAIT_STEP_US    10U

extern transfer_info_t g_transfer_emg_info;

static volatile bool s_emg_sample_complete = false;
static bool          s_emg_adc_initialized = false;

typedef struct st_emg_biquad_state
{
    float z1;
    float z2;
} emg_biquad_state_t;

typedef struct st_emg_filter_state
{
    bool               initialized;
    emg_biquad_state_t section[4];
} emg_filter_state_t;

static emg_filter_state_t s_emg_filter;

static fsp_err_t drv_emg_accept_open_result(fsp_err_t err);

fsp_err_t drv_emg_adc_init(void)
{
    fsp_err_t err;

    if (s_emg_adc_initialized)
    {
        return FSP_SUCCESS;
    }

    err = drv_emg_accept_open_result(g_adc_emg.p_api->open(g_adc_emg.p_ctrl, g_adc_emg.p_cfg));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_adc_emg.p_api->scanCfg(g_adc_emg.p_ctrl, g_adc_emg.p_channel_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = drv_emg_accept_open_result(g_elc.p_api->open(g_elc.p_ctrl, g_elc.p_cfg));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_elc.p_api->enable(g_elc.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = drv_emg_accept_open_result(g_transfer_emg.p_api->open(g_transfer_emg.p_ctrl, g_transfer_emg.p_cfg));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = drv_emg_accept_open_result(g_timer_emg.p_api->open(g_timer_emg.p_ctrl, g_timer_emg.p_cfg));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_adc_emg.p_api->scanStart(g_adc_emg.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_emg_adc_initialized = true;

    return FSP_SUCCESS;
}

fsp_err_t drv_emg_adc_read_sample(uint16_t * p_sample)
{
    fsp_err_t err;
    uint32_t  waited_us = 0U;

    if (NULL == p_sample)
    {
        return FSP_ERR_ASSERTION;
    }

    if (!s_emg_adc_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    s_emg_sample_complete = false;
    g_transfer_emg_info.p_dest = p_sample;
    g_transfer_emg_info.length = 1U;
    g_transfer_emg_info.num_blocks = 1U;

    err = g_transfer_emg.p_api->reconfigure(g_transfer_emg.p_ctrl, &g_transfer_emg_info);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    (void) g_timer_emg.p_api->stop(g_timer_emg.p_ctrl);

    err = g_timer_emg.p_api->reset(g_timer_emg.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_timer_emg.p_api->start(g_timer_emg.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    while ((!s_emg_sample_complete) && (waited_us < EMG_ADC_WAIT_TIMEOUT_US))
    {
        R_BSP_SoftwareDelay(EMG_ADC_WAIT_STEP_US, BSP_DELAY_UNITS_MICROSECONDS);
        waited_us += EMG_ADC_WAIT_STEP_US;
    }

    err = g_timer_emg.p_api->stop(g_timer_emg.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    return s_emg_sample_complete ? FSP_SUCCESS : FSP_ERR_TIMEOUT;
}

void emg_dma_callback(dmac_callback_args_t * p_args)
{
    (void) p_args;
    s_emg_sample_complete = true;
}

void drv_emg_filter_reset(void)
{
    memset(&s_emg_filter, 0, sizeof(s_emg_filter));
}

float drv_emg_filter(float input)
{
    float output = input;
    float x;

    if (!s_emg_filter.initialized)
    {
        s_emg_filter.initialized = true;
    }

    x = output - (-0.55195385f * s_emg_filter.section[0].z1) - (0.60461714f * s_emg_filter.section[0].z2);
    output = 0.00223489f * x + (0.00446978f * s_emg_filter.section[0].z1) + (0.00223489f * s_emg_filter.section[0].z2);
    s_emg_filter.section[0].z2 = s_emg_filter.section[0].z1;
    s_emg_filter.section[0].z1 = x;

    x = output - (-0.86036562f * s_emg_filter.section[1].z1) - (0.63511954f * s_emg_filter.section[1].z2);
    output = 1.00000000f * x + (2.00000000f * s_emg_filter.section[1].z1) + (1.00000000f * s_emg_filter.section[1].z2);
    s_emg_filter.section[1].z2 = s_emg_filter.section[1].z1;
    s_emg_filter.section[1].z1 = x;

    x = output - (-0.37367240f * s_emg_filter.section[2].z1) - (0.81248708f * s_emg_filter.section[2].z2);
    output = 1.00000000f * x + (-2.00000000f * s_emg_filter.section[2].z1) + (1.00000000f * s_emg_filter.section[2].z2);
    s_emg_filter.section[2].z2 = s_emg_filter.section[2].z1;
    s_emg_filter.section[2].z1 = x;

    x = output - (-1.15601175f * s_emg_filter.section[3].z1) - (0.84761589f * s_emg_filter.section[3].z2);
    output = 1.00000000f * x + (-2.00000000f * s_emg_filter.section[3].z1) + (1.00000000f * s_emg_filter.section[3].z2);
    s_emg_filter.section[3].z2 = s_emg_filter.section[3].z1;
    s_emg_filter.section[3].z1 = x;

    return output;
}

static fsp_err_t drv_emg_accept_open_result(fsp_err_t err)
{
    if ((FSP_SUCCESS == err) || (FSP_ERR_ALREADY_OPEN == err))
    {
        return FSP_SUCCESS;
    }

    return err;
}
