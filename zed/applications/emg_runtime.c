#include "emg_runtime.h"
#include "drv_emg_adc.h"
#include <math.h>
#include <string.h>

#define EMG_SAMPLE_INTERVAL_US 2000U
#define EMG_ENVELOPE_WINDOW_MIN 1U
#define EMG_ENVELOPE_WINDOW_MAX 128U
#define EMG_ENVELOPE_SCALE      2L

typedef struct st_emg_runtime_state
{
    bool                 initialized;
    uint16_t             envelope_index;
    uint32_t             next_sample_time_us;
    uint32_t             total_samples;
    uint16_t             last_raw_sample;
    float                last_filtered_value;
    int32_t              last_envelope_value;
    int32_t              envelope_sum;
    int32_t              envelope_buffer[EMG_ENVELOPE_WINDOW_MAX];
    emg_tune_params_t    params;
    emg_debug_snapshot_t debug_snapshot;
} emg_runtime_state_t;

static emg_runtime_state_t s_emg_runtime;

static void      emg_runtime_load_default_params(emg_tune_params_t * p_params);
static void      emg_runtime_reset_dynamic_state(void);
static void      emg_runtime_update_debug_snapshot(void);
static fsp_err_t emg_runtime_validate_window_size(float value);
static int32_t   emg_runtime_update_envelope(int32_t abs_sample);

fsp_err_t emg_runtime_init(void)
{
    fsp_err_t err;

    memset(&s_emg_runtime, 0, sizeof(s_emg_runtime));
    emg_runtime_load_default_params(&s_emg_runtime.params);
    emg_runtime_reset_dynamic_state();
    drv_emg_filter_reset();

    err = drv_emg_adc_init();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_emg_runtime.initialized = true;
    emg_runtime_update_debug_snapshot();

    return FSP_SUCCESS;
}

void emg_runtime_poll(uint32_t now_us)
{
    if (!s_emg_runtime.initialized)
    {
        return;
    }

    if (0U == s_emg_runtime.next_sample_time_us)
    {
        s_emg_runtime.next_sample_time_us = now_us;
    }

    if ((int32_t) (now_us - s_emg_runtime.next_sample_time_us) < 0)
    {
        return;
    }

    if (FSP_SUCCESS != emg_runtime_process_next_sample())
    {
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
        return;
    }

    s_emg_runtime.next_sample_time_us += EMG_SAMPLE_INTERVAL_US;
    if ((int32_t) (now_us - s_emg_runtime.next_sample_time_us) >= 0)
    {
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
    }
}

fsp_err_t emg_runtime_process_next_sample(void)
{
    fsp_err_t err;
    uint16_t  raw_sample = 0U;
    float     filtered_value;
    int32_t   abs_filtered_value;

    if (!s_emg_runtime.initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    err = drv_emg_adc_read_sample(&raw_sample);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    filtered_value = drv_emg_filter((float) raw_sample);
    abs_filtered_value = (int32_t) fabsf(filtered_value);

    s_emg_runtime.last_raw_sample = raw_sample;
    s_emg_runtime.last_filtered_value = filtered_value;
    s_emg_runtime.last_envelope_value = emg_runtime_update_envelope(abs_filtered_value);
    s_emg_runtime.total_samples++;
    emg_runtime_update_debug_snapshot();

    return FSP_SUCCESS;
}

uint8_t emg_runtime_get_grip_percent(void)
{
    return 0U;
}

uint16_t emg_runtime_get_last_raw_sample(void)
{
    return s_emg_runtime.last_raw_sample;
}

float emg_runtime_get_last_filtered_value(void)
{
    return s_emg_runtime.last_filtered_value;
}

float emg_runtime_get_last_envelope(void)
{
    return (float) s_emg_runtime.last_envelope_value;
}

uint32_t emg_runtime_get_total_samples(void)
{
    return s_emg_runtime.total_samples;
}

void emg_runtime_get_params(emg_tune_params_t * p_params)
{
    if (NULL == p_params)
    {
        return;
    }

    *p_params = s_emg_runtime.params;
}

fsp_err_t emg_runtime_set_param(char const * p_name, float value)
{
    emg_tune_params_t params = s_emg_runtime.params;
    fsp_err_t         err = FSP_ERR_INVALID_ARGUMENT;

    if ((NULL == p_name) || !isfinite(value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (0 == strcmp(p_name, "ENVELOPE_WINDOW_SIZE"))
    {
        err = emg_runtime_validate_window_size(value);
        if (FSP_SUCCESS == err)
        {
            params.envelope_window_size = (uint16_t) floorf(value + 0.5f);
        }
    }

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_emg_runtime.params = params;
    emg_runtime_reset_dynamic_state();
    drv_emg_filter_reset();
    emg_runtime_update_debug_snapshot();

    return FSP_SUCCESS;
}

void emg_runtime_reset_params_to_defaults(void)
{
    emg_runtime_load_default_params(&s_emg_runtime.params);
    emg_runtime_reset_dynamic_state();
    drv_emg_filter_reset();
    emg_runtime_update_debug_snapshot();
}

void emg_runtime_get_debug_snapshot(emg_debug_snapshot_t * p_snapshot)
{
    if (NULL == p_snapshot)
    {
        return;
    }

    *p_snapshot = s_emg_runtime.debug_snapshot;
}

static void emg_runtime_load_default_params(emg_tune_params_t * p_params)
{
    if (NULL == p_params)
    {
        return;
    }

    p_params->envelope_window_size = EMG_TUNE_ENVELOPE_WINDOW_SIZE;
}

static void emg_runtime_reset_dynamic_state(void)
{
    s_emg_runtime.envelope_index = 0U;
    s_emg_runtime.next_sample_time_us = 0U;
    s_emg_runtime.total_samples = 0U;
    s_emg_runtime.last_raw_sample = 0U;
    s_emg_runtime.last_filtered_value = 0.0f;
    s_emg_runtime.last_envelope_value = 0L;
    s_emg_runtime.envelope_sum = 0L;
    memset(s_emg_runtime.envelope_buffer, 0, sizeof(s_emg_runtime.envelope_buffer));
}

static int32_t emg_runtime_update_envelope(int32_t abs_sample)
{
    uint16_t window_size = s_emg_runtime.params.envelope_window_size;

    if (window_size < EMG_ENVELOPE_WINDOW_MIN)
    {
        window_size = EMG_ENVELOPE_WINDOW_MIN;
    }
    else if (window_size > EMG_ENVELOPE_WINDOW_MAX)
    {
        window_size = EMG_ENVELOPE_WINDOW_MAX;
    }

    s_emg_runtime.envelope_sum -= s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index];
    s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index] = abs_sample;
    s_emg_runtime.envelope_sum += abs_sample;

    s_emg_runtime.envelope_index++;
    if (s_emg_runtime.envelope_index >= window_size)
    {
        s_emg_runtime.envelope_index = 0U;
    }

    return (s_emg_runtime.envelope_sum / (int32_t) window_size) * EMG_ENVELOPE_SCALE;
}

static void emg_runtime_update_debug_snapshot(void)
{
    s_emg_runtime.debug_snapshot.raw = s_emg_runtime.last_raw_sample;
    s_emg_runtime.debug_snapshot.filtered = s_emg_runtime.last_filtered_value;
    s_emg_runtime.debug_snapshot.envelope = (float) s_emg_runtime.last_envelope_value;
    s_emg_runtime.debug_snapshot.rest = 0.0f;
    s_emg_runtime.debug_snapshot.peak = 0.0f;
    s_emg_runtime.debug_snapshot.th_on = 0.0f;
    s_emg_runtime.debug_snapshot.th_off = 0.0f;
    s_emg_runtime.debug_snapshot.grip = 0U;
    s_emg_runtime.debug_snapshot.active = false;
    s_emg_runtime.debug_snapshot.sample_count = s_emg_runtime.total_samples;
}

static fsp_err_t emg_runtime_validate_window_size(float value)
{
    float rounded_value;

    if (!isfinite(value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    rounded_value = floorf(value + 0.5f);
    if ((rounded_value < (float) EMG_ENVELOPE_WINDOW_MIN) || (rounded_value > (float) EMG_ENVELOPE_WINDOW_MAX))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    return FSP_SUCCESS;
}
