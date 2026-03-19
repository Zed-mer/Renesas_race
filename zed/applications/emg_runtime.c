#include "emg_runtime.h"
#include "drv_emg_adc.h"
#include <math.h>
#include <string.h>

/*
 * 采样链路固定为 500 Hz，所以软件层仍按 2 ms 的节拍推进一次运行时状态。
 * 这样既能和底层 GPT/ADC 的配置保持一致，也方便主循环用非阻塞方式调用。
 */
#define EMG_SAMPLE_INTERVAL_US 2000U

/*
 * 上电后的前一小段时间用于“热启动”。
 * 这时 rest / peak 还没有建立起来，先给算法一个短暂的自举过程，
 * 避免刚启动时阈值还没成形就把 grip 拉起来。
 */
#define EMG_BOOTSTRAP_SAMPLES 48U

/*
 * 包络窗口允许调节的范围。
 * 默认值和老工程一致是 16，这里给稍宽一些的上下限，方便实验时试手感。
 */
#define EMG_ENVELOPE_WINDOW_MIN 1U
#define EMG_ENVELOPE_WINDOW_MAX 64U

/*
 * 下面这一组参数继续保留在固件内部，用来把包络映射成稳定的 grip 百分比。
 * 它们不再走上位机调参，目的是把“用户可调项”收敛成一个：包络窗口大小。
 * 这样既保留了当前工程较成熟的 grip 映射链路，也让调参动作回到老工程的使用习惯。
 */
#define EMG_REST_RISE_ALPHA      0.002f
#define EMG_REST_FALL_ALPHA      0.10f
#define EMG_PEAK_DECAY_ALPHA     0.004f
#define EMG_MIN_SPAN             18.0f
#define EMG_ON_RATIO             0.22f
#define EMG_OFF_RATIO            0.12f
#define EMG_ENTER_COUNT          3U
#define EMG_EXIT_COUNT           8U
#define EMG_DEADZONE_RATIO       0.10f
#define EMG_OUTPUT_GAMMA         1.20f
#define EMG_OUTPUT_ATTACK_ALPHA  0.30f
#define EMG_OUTPUT_RELEASE_ALPHA 0.12f

typedef struct st_emg_runtime_state
{
    bool                 initialized;
    bool                 active;
    uint16_t             enter_counter;
    uint16_t             exit_counter;
    uint16_t             envelope_index;
    uint32_t             next_sample_time_us;
    uint32_t             total_samples;
    uint16_t             last_raw_sample;
    float                last_filtered_value;
    float                envelope_level;
    float                rest_level;
    float                peak_level;
    float                threshold_on;
    float                threshold_off;
    float                grip_percent_f;
    uint8_t              grip_percent;
    float                envelope_sum;
    float                envelope_buffer[EMG_ENVELOPE_WINDOW_MAX];
    emg_tune_params_t    params;
    emg_debug_snapshot_t debug_snapshot;
} emg_runtime_state_t;

static emg_runtime_state_t s_emg_runtime;

static void      emg_runtime_load_default_params(emg_tune_params_t * p_params);
static void      emg_runtime_reset_dynamic_state(void);
static float     emg_runtime_clampf(float value, float min_value, float max_value);
static bool      emg_runtime_is_low_activity(float envelope);
static void      emg_runtime_refresh_thresholds(void);
static float     emg_runtime_update_envelope(float abs_sample);
static void      emg_runtime_bootstrap(float envelope);
static void      emg_runtime_update_levels(float envelope);
static void      emg_runtime_update_activity(float envelope);
static float     emg_runtime_apply_curve(float normalized);
static void      emg_runtime_update_output(float envelope);
static void      emg_runtime_update_debug_snapshot(void);
static fsp_err_t emg_runtime_validate_window_size(float value);

fsp_err_t emg_runtime_init(void)
{
    fsp_err_t err;

    /*
     * 初始化分两层：
     * 1. 先把运行时状态和默认参数清干净；
     * 2. 再打开底层 ADC + DMAC + GPT + ELC 采样链路。
     *
     * 这样即使硬件初始化失败，软件侧状态也仍然是确定的，便于调试。
     */
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
    emg_runtime_refresh_thresholds();
    emg_runtime_update_debug_snapshot();

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

    /*
     * 这里用软件时间节拍限流，不允许主循环“跑多快就采多快”。
     * 只有到了下一个 2 ms 的采样槽位，才真正触发一次单样本读取。
     */
    if ((int32_t) (now_us - s_emg_runtime.next_sample_time_us) < 0)
    {
        return;
    }

    if (FSP_SUCCESS != drv_emg_adc_read_sample(&raw_sample))
    {
        /*
         * 采样失败时不阻塞上层主循环，只把下一个采样时刻顺延。
         * 这样即使偶发超时，也不会把整条主业务链卡死。
         */
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
        return;
    }

    /*
     * 滤波器本体继续沿用老工程那组固定系数。
     * 这一步输出的是交流成分，后面再取绝对值并做滑动包络。
     */
    filtered_value = drv_emg_filter((float) raw_sample);
    envelope = emg_runtime_update_envelope(fabsf(filtered_value));

    s_emg_runtime.last_raw_sample = raw_sample;
    s_emg_runtime.last_filtered_value = filtered_value;
    s_emg_runtime.total_samples++;

    if (s_emg_runtime.total_samples <= EMG_BOOTSTRAP_SAMPLES)
    {
        emg_runtime_bootstrap(envelope);
    }
    else
    {
        emg_runtime_update_levels(envelope);
        emg_runtime_update_activity(envelope);
        emg_runtime_update_output(envelope);
    }

    emg_runtime_update_debug_snapshot();

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
    return s_emg_runtime.envelope_level;
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

    /*
     * 当前版本只保留一个对外调参量：
     * ENVELOPE_WINDOW_SIZE。
     * 这样上位机、串口协议和默认宏都围绕“窗口长度”这一件事展开，
     * 使用方式就和老工程保持一致了。
     */
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

    /*
     * 改窗口长度时，旧的滑动平均缓存已经没有意义了。
     * 这里主动把运行时动态状态清空，让新窗口从干净状态重新建立。
     */
    s_emg_runtime.params = params;
    emg_runtime_reset_dynamic_state();
    emg_runtime_refresh_thresholds();
    emg_runtime_update_debug_snapshot();

    return FSP_SUCCESS;
}

void emg_runtime_reset_params_to_defaults(void)
{
    emg_runtime_load_default_params(&s_emg_runtime.params);
    emg_runtime_reset_dynamic_state();
    emg_runtime_refresh_thresholds();
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
    s_emg_runtime.active = false;
    s_emg_runtime.enter_counter = 0U;
    s_emg_runtime.exit_counter = 0U;
    s_emg_runtime.envelope_index = 0U;
    s_emg_runtime.next_sample_time_us = 0U;
    s_emg_runtime.total_samples = 0U;
    s_emg_runtime.last_raw_sample = 0U;
    s_emg_runtime.last_filtered_value = 0.0f;
    s_emg_runtime.envelope_level = 0.0f;
    s_emg_runtime.rest_level = 0.0f;
    s_emg_runtime.peak_level = EMG_MIN_SPAN;
    s_emg_runtime.threshold_on = EMG_MIN_SPAN * EMG_ON_RATIO;
    s_emg_runtime.threshold_off = EMG_MIN_SPAN * EMG_OFF_RATIO;
    s_emg_runtime.grip_percent_f = 0.0f;
    s_emg_runtime.grip_percent = 0U;
    s_emg_runtime.envelope_sum = 0.0f;
    memset(s_emg_runtime.envelope_buffer, 0, sizeof(s_emg_runtime.envelope_buffer));
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

static bool emg_runtime_is_low_activity(float envelope)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;
    float quiet_threshold;

    if (span < EMG_MIN_SPAN)
    {
        span = EMG_MIN_SPAN;
    }

    /*
     * rest 只在“明显不像主动收缩”的区域里更新，
     * 避免用户长时间收缩时把静息基线错误抬高。
     */
    quiet_threshold = s_emg_runtime.rest_level + (span * (EMG_ON_RATIO * 0.5f));

    return (envelope <= quiet_threshold) || (!s_emg_runtime.active);
}

static void emg_runtime_refresh_thresholds(void)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;

    if (span < EMG_MIN_SPAN)
    {
        span = EMG_MIN_SPAN;
    }

    s_emg_runtime.threshold_on = s_emg_runtime.rest_level + (span * EMG_ON_RATIO);
    s_emg_runtime.threshold_off = s_emg_runtime.rest_level + (span * EMG_OFF_RATIO);
}

static float emg_runtime_update_envelope(float abs_sample)
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

    /*
     * 这里改回老工程同款包络算法：
     * 1. 先对滤波后的肌电取绝对值；
     * 2. 再做固定窗口滑动平均。
     *
     * 和之前的 attack / release 包络不同，这里没有快起慢落的额外时间常数，
     * 包络平滑程度完全由“窗口长度”决定，更直观，也更容易和老工程直接对照。
     */
    s_emg_runtime.envelope_sum -= s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index];
    s_emg_runtime.envelope_buffer[s_emg_runtime.envelope_index] = abs_sample;
    s_emg_runtime.envelope_sum += abs_sample;

    s_emg_runtime.envelope_index++;
    if (s_emg_runtime.envelope_index >= window_size)
    {
        s_emg_runtime.envelope_index = 0U;
    }

    s_emg_runtime.envelope_level = s_emg_runtime.envelope_sum / (float) window_size;

    return s_emg_runtime.envelope_level;
}

static void emg_runtime_bootstrap(float envelope)
{
    if (1U == s_emg_runtime.total_samples)
    {
        s_emg_runtime.rest_level = envelope;
        s_emg_runtime.peak_level = envelope + EMG_MIN_SPAN;
    }
    else
    {
        s_emg_runtime.rest_level += 0.10f * (envelope - s_emg_runtime.rest_level);

        if (s_emg_runtime.peak_level < (envelope + EMG_MIN_SPAN))
        {
            s_emg_runtime.peak_level = envelope + EMG_MIN_SPAN;
        }
    }

    s_emg_runtime.active = false;
    s_emg_runtime.enter_counter = 0U;
    s_emg_runtime.exit_counter = 0U;
    s_emg_runtime.grip_percent_f = 0.0f;
    s_emg_runtime.grip_percent = 0U;
    emg_runtime_refresh_thresholds();
}

static void emg_runtime_update_levels(float envelope)
{
    float peak_floor;

    if (emg_runtime_is_low_activity(envelope))
    {
        if (envelope < s_emg_runtime.rest_level)
        {
            s_emg_runtime.rest_level += EMG_REST_FALL_ALPHA * (envelope - s_emg_runtime.rest_level);
        }
        else
        {
            s_emg_runtime.rest_level += EMG_REST_RISE_ALPHA * (envelope - s_emg_runtime.rest_level);
        }
    }
    else if (envelope < s_emg_runtime.rest_level)
    {
        /*
         * 即使当前不属于低活动区，只要包络已经落到 rest 以下，
         * 仍然允许基线通过“下降通道”快速跟下来，避免静息恢复过慢。
         */
        s_emg_runtime.rest_level += EMG_REST_FALL_ALPHA * (envelope - s_emg_runtime.rest_level);
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

    emg_runtime_refresh_thresholds();
}

static void emg_runtime_update_activity(float envelope)
{
    if (!s_emg_runtime.active)
    {
        if (envelope >= s_emg_runtime.threshold_on)
        {
            if (s_emg_runtime.enter_counter < EMG_ENTER_COUNT)
            {
                s_emg_runtime.enter_counter++;
            }
        }
        else
        {
            s_emg_runtime.enter_counter = 0U;
        }

        if (s_emg_runtime.enter_counter >= EMG_ENTER_COUNT)
        {
            s_emg_runtime.active = true;
            s_emg_runtime.enter_counter = 0U;
            s_emg_runtime.exit_counter = 0U;
        }
    }
    else
    {
        if (envelope <= s_emg_runtime.threshold_off)
        {
            if (s_emg_runtime.exit_counter < EMG_EXIT_COUNT)
            {
                s_emg_runtime.exit_counter++;
            }
        }
        else
        {
            s_emg_runtime.exit_counter = 0U;
        }

        if (s_emg_runtime.exit_counter >= EMG_EXIT_COUNT)
        {
            s_emg_runtime.active = false;
            s_emg_runtime.enter_counter = 0U;
            s_emg_runtime.exit_counter = 0U;
        }
    }
}

static float emg_runtime_apply_curve(float normalized)
{
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    if (normalized <= EMG_DEADZONE_RATIO)
    {
        return 0.0f;
    }

    normalized = (normalized - EMG_DEADZONE_RATIO) / (1.0f - EMG_DEADZONE_RATIO);
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    /*
     * gamma 曲线本质上是在调“手感”：
     * - gamma > 1 时，前段更稳，后段更容易拉满；
     * - gamma < 1 时，前段更灵敏。
     */
    return powf(normalized, EMG_OUTPUT_GAMMA);
}

static void emg_runtime_update_output(float envelope)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;
    float normalized;
    float target_percent;
    float alpha;

    if (span < EMG_MIN_SPAN)
    {
        span = EMG_MIN_SPAN;
    }

    normalized = (envelope - s_emg_runtime.rest_level) / span;
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    if (!s_emg_runtime.active)
    {
        /*
         * 这里显式地把“未激活态”压回 0，
         * 目的是让静息噪声即使在阈值附近轻微波动，也不会直接映射成 grip。
         */
        target_percent = 0.0f;
    }
    else
    {
        target_percent = emg_runtime_apply_curve(normalized) * 100.0f;
    }

    alpha = (target_percent > s_emg_runtime.grip_percent_f) ?
            EMG_OUTPUT_ATTACK_ALPHA :
            EMG_OUTPUT_RELEASE_ALPHA;

    s_emg_runtime.grip_percent_f += alpha * (target_percent - s_emg_runtime.grip_percent_f);
    s_emg_runtime.grip_percent_f = emg_runtime_clampf(s_emg_runtime.grip_percent_f, 0.0f, 100.0f);
    s_emg_runtime.grip_percent = (uint8_t) (s_emg_runtime.grip_percent_f + 0.5f);
}

static void emg_runtime_update_debug_snapshot(void)
{
    s_emg_runtime.debug_snapshot.raw = s_emg_runtime.last_raw_sample;
    s_emg_runtime.debug_snapshot.filtered = s_emg_runtime.last_filtered_value;
    s_emg_runtime.debug_snapshot.envelope = s_emg_runtime.envelope_level;
    s_emg_runtime.debug_snapshot.rest = s_emg_runtime.rest_level;
    s_emg_runtime.debug_snapshot.peak = s_emg_runtime.peak_level;
    s_emg_runtime.debug_snapshot.th_on = s_emg_runtime.threshold_on;
    s_emg_runtime.debug_snapshot.th_off = s_emg_runtime.threshold_off;
    s_emg_runtime.debug_snapshot.grip = s_emg_runtime.grip_percent;
    s_emg_runtime.debug_snapshot.active = s_emg_runtime.active;
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
