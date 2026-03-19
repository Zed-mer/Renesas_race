#include "emg_runtime.h"
#include "drv_emg_adc.h"
#include <math.h>
#include <string.h>

#define EMG_SAMPLE_INTERVAL_US      2000U
#define EMG_ENVELOPE_WINDOW_SIZE    16U
#define EMG_STARTUP_SAMPLES         32U
#define EMG_REST_RISE_ALPHA         0.002f
#define EMG_REST_FALL_ALPHA         0.100f
#define EMG_PEAK_DECAY_ALPHA        0.004f
#define EMG_MIN_SPAN                18.0f
#define EMG_DEADZONE_RATIO          0.10f
#define EMG_OUTPUT_SMOOTH_ALPHA     0.25f

typedef struct st_emg_runtime_state
{
    bool     initialized;
    uint32_t next_sample_time_us;
    uint32_t total_samples;
    uint16_t last_raw_sample;
    float    last_filtered_value;
    float    last_envelope;
    float    grip_percent_f;
    uint8_t  grip_percent;
    float    rest_level;
    float    peak_level;
    float    envelope_sum;
    uint8_t  envelope_index;
    float    envelope_buffer[EMG_ENVELOPE_WINDOW_SIZE];
} emg_runtime_state_t;

static emg_runtime_state_t s_emg_runtime;

static float emg_runtime_clampf(float value, float min_value, float max_value);
static float emg_runtime_update_envelope(float sample);
static void  emg_runtime_bootstrap(float envelope);
static void  emg_runtime_update_levels(float envelope);
static void  emg_runtime_update_output(float envelope);

fsp_err_t emg_runtime_init(void)
{
    fsp_err_t err;

    memset(&s_emg_runtime, 0, sizeof(s_emg_runtime));
    drv_emg_filter_reset();

    err = drv_emg_adc_init();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_emg_runtime.initialized = true;

    return FSP_SUCCESS;
}

void emg_runtime_poll(uint32_t now_us)
{
    uint16_t raw_sample = 0U;
    float    filtered_value;
    float    envelope;

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

    if (FSP_SUCCESS != drv_emg_adc_read_sample(&raw_sample))
    {
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
        return;
    }

    filtered_value = drv_emg_filter((float) raw_sample);
    envelope = emg_runtime_update_envelope(fabsf(filtered_value));

    s_emg_runtime.last_raw_sample = raw_sample;
    s_emg_runtime.last_filtered_value = filtered_value;
    s_emg_runtime.last_envelope = envelope;
    s_emg_runtime.total_samples++;

    if (s_emg_runtime.total_samples <= EMG_STARTUP_SAMPLES)
    {
        emg_runtime_bootstrap(envelope);
    }
    else
    {
        emg_runtime_update_levels(envelope);
        emg_runtime_update_output(envelope);
    }

    s_emg_runtime.next_sample_time_us += EMG_SAMPLE_INTERVAL_US;
    if ((int32_t) (now_us - s_emg_runtime.next_sample_time_us) >= 0)
    {
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
    }
}

uint8_t emg_runtime_get_grip_percent(void)
{
    return s_emg_runtime.grip_percent;
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
    return s_emg_runtime.last_envelope;
}

uint32_t emg_runtime_get_total_samples(void)
{
    return s_emg_runtime.total_samples;
}

static float emg_runtime_clampf(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static float emg_runtime_update_envelope(float sample)
{
    s_emg_runtime.envelope_sum -= s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index];
    s_emg_runtime.envelope_sum += sample;
    s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index] = sample;
    s_emg_runtime.envelope_index = (uint8_t) ((s_emg_runtime.envelope_index + 1U) % EMG_ENVELOPE_WINDOW_SIZE);

    return s_emg_runtime.envelope_sum / (float) EMG_ENVELOPE_WINDOW_SIZE;
}

static void emg_runtime_bootstrap(float envelope)
{
    s_emg_runtime.rest_level = envelope;
    s_emg_runtime.peak_level = envelope + EMG_MIN_SPAN;
    s_emg_runtime.grip_percent_f = 0.0f;
    s_emg_runtime.grip_percent = 0U;
}

static void emg_runtime_update_levels(float envelope)
{
    float rest_target = envelope;
    float peak_floor;

    if (rest_target < s_emg_runtime.rest_level)
    {
        s_emg_runtime.rest_level += EMG_REST_FALL_ALPHA * (rest_target - s_emg_runtime.rest_level);
    }
    else
    {
        s_emg_runtime.rest_level += EMG_REST_RISE_ALPHA * (rest_target - s_emg_runtime.rest_level);
    }

    if (envelope > s_emg_runtime.peak_level)
    {
        s_emg_runtime.peak_level = envelope;
    }
    else
    {
        peak_floor = s_emg_runtime.rest_level + EMG_MIN_SPAN;
        s_emg_runtime.peak_level += EMG_PEAK_DECAY_ALPHA * (peak_floor - s_emg_runtime.peak_level);
    }

    if (s_emg_runtime.peak_level < (s_emg_runtime.rest_level + EMG_MIN_SPAN))
    {
        s_emg_runtime.peak_level = s_emg_runtime.rest_level + EMG_MIN_SPAN;
    }
}

static void emg_runtime_update_output(float envelope)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;
    float normalized;
    float target_percent;

    if (span < EMG_MIN_SPAN)
    {
        span = EMG_MIN_SPAN;
    }

    normalized = (envelope - s_emg_runtime.rest_level) / span;
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    if (normalized <= EMG_DEADZONE_RATIO)
    {
        target_percent = 0.0f;
    }
    else
    {
        target_percent = (normalized - EMG_DEADZONE_RATIO) / (1.0f - EMG_DEADZONE_RATIO);
        target_percent = emg_runtime_clampf(target_percent, 0.0f, 1.0f) * 100.0f;
    }

    s_emg_runtime.grip_percent_f += EMG_OUTPUT_SMOOTH_ALPHA * (target_percent - s_emg_runtime.grip_percent_f);
    s_emg_runtime.grip_percent_f = emg_runtime_clampf(s_emg_runtime.grip_percent_f, 0.0f, 100.0f);
    s_emg_runtime.grip_percent = (uint8_t) (s_emg_runtime.grip_percent_f + 0.5f);
}
