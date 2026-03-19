#include "emg_runtime.h"
#include "drv_emg_adc.h"
#include <math.h>
#include <string.h>

/*
 * 500Hz 采样意味着相邻样本间隔固定为 2ms。
 * 运行时层不直接碰定时器配置，而是按照这个软件时间片节奏去拉取单次采样，
 * 这样既能保证算法和硬件层解耦，也便于将来替换成别的采样后端。
 */
#define EMG_SAMPLE_INTERVAL_US      2000U

/*
 * 启动最开始的几十个点先拿来做“基线热启动”，
 * 目的是避免刚开机时 rest / peak 还是零，导致前几帧 grip 映射异常。
 */
#define EMG_BOOTSTRAP_SAMPLES       48U

typedef struct st_emg_runtime_state
{
    bool                 initialized;
    bool                 active;
    uint16_t             enter_counter;
    uint16_t             exit_counter;
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
    emg_tune_params_t    params;
    emg_debug_snapshot_t debug_snapshot;
} emg_runtime_state_t;

static emg_runtime_state_t s_emg_runtime;

static void      emg_runtime_load_default_params(emg_tune_params_t * p_params);
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
static fsp_err_t emg_runtime_validate_float_range(float value, float min_value, float max_value);
static fsp_err_t emg_runtime_validate_count(float value, uint16_t min_value, uint16_t max_value);

fsp_err_t emg_runtime_init(void)
{
    fsp_err_t err;

    /*
     * 初始化分成两部分：
     * 1. 先把运行时状态和默认参数清干净；
     * 2. 再打开底层 ADC / DMAC / GPT / ELC 采样链路。
     *
     * 这样做的好处是：一旦硬件初始化失败，软件状态也仍然是可预期的“全零 + 默认参数”。
     */
    memset(&s_emg_runtime, 0, sizeof(s_emg_runtime));
    emg_runtime_load_default_params(&s_emg_runtime.params);
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
     * 这里用软件时间戳节流，不让主循环“跑多快就采多快”。
     * 只要当前时间还没到下一个 2ms 采样槽位，就直接返回。
     */
    if ((int32_t) (now_us - s_emg_runtime.next_sample_time_us) < 0)
    {
        return;
    }

    if (FSP_SUCCESS != drv_emg_adc_read_sample(&raw_sample))
    {
        /*
         * 采样失败时不阻塞主循环，只把下一个采样时刻顺延。
         * 这样即使偶发超时，也不会把整个上层业务卡死。
         */
        s_emg_runtime.next_sample_time_us = now_us + EMG_SAMPLE_INTERVAL_US;
        return;
    }

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
     * 串口协议直接用字符串参数名，是为了让上位机导出的宏名、串口调参名、固件内部字段名
     * 尽量保持一致，减少“页面上叫 A，代码里叫 B，导出时又叫 C”的沟通成本。
     */
    if (0 == strcmp(p_name, "ENV_ATTACK_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.env_attack_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "ENV_RELEASE_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.env_release_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "REST_RISE_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.0001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.rest_rise_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "REST_FALL_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.0001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.rest_fall_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "PEAK_DECAY_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.0001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.peak_decay_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "MIN_SPAN"))
    {
        err = emg_runtime_validate_float_range(value, 1.0f, 10000.0f);
        if (FSP_SUCCESS == err)
        {
            params.min_span = value;
        }
    }
    else if (0 == strcmp(p_name, "ON_RATIO"))
    {
        err = emg_runtime_validate_float_range(value, 0.01f, 0.95f);
        if (FSP_SUCCESS == err)
        {
            params.on_ratio = value;
        }
    }
    else if (0 == strcmp(p_name, "OFF_RATIO"))
    {
        err = emg_runtime_validate_float_range(value, 0.0f, 0.90f);
        if (FSP_SUCCESS == err)
        {
            params.off_ratio = value;
        }
    }
    else if (0 == strcmp(p_name, "ENTER_COUNT"))
    {
        err = emg_runtime_validate_count(value, 1U, 1000U);
        if (FSP_SUCCESS == err)
        {
            params.enter_count = (uint16_t) (value + 0.5f);
        }
    }
    else if (0 == strcmp(p_name, "EXIT_COUNT"))
    {
        err = emg_runtime_validate_count(value, 1U, 1000U);
        if (FSP_SUCCESS == err)
        {
            params.exit_count = (uint16_t) (value + 0.5f);
        }
    }
    else if (0 == strcmp(p_name, "DEADZONE_RATIO"))
    {
        err = emg_runtime_validate_float_range(value, 0.0f, 0.95f);
        if (FSP_SUCCESS == err)
        {
            params.deadzone_ratio = value;
        }
    }
    else if (0 == strcmp(p_name, "OUTPUT_GAMMA"))
    {
        err = emg_runtime_validate_float_range(value, 0.1f, 10.0f);
        if (FSP_SUCCESS == err)
        {
            params.output_gamma = value;
        }
    }
    else if (0 == strcmp(p_name, "OUTPUT_ATTACK_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.output_attack_alpha = value;
        }
    }
    else if (0 == strcmp(p_name, "OUTPUT_RELEASE_ALPHA"))
    {
        err = emg_runtime_validate_float_range(value, 0.001f, 1.0f);
        if (FSP_SUCCESS == err)
        {
            params.output_release_alpha = value;
        }
    }

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (params.on_ratio <= params.off_ratio)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    s_emg_runtime.params = params;
    emg_runtime_refresh_thresholds();
    emg_runtime_update_debug_snapshot();

    return FSP_SUCCESS;
}

void emg_runtime_reset_params_to_defaults(void)
{
    emg_runtime_load_default_params(&s_emg_runtime.params);
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

    p_params->env_attack_alpha = EMG_TUNE_ENV_ATTACK_ALPHA;
    p_params->env_release_alpha = EMG_TUNE_ENV_RELEASE_ALPHA;
    p_params->rest_rise_alpha = EMG_TUNE_REST_RISE_ALPHA;
    p_params->rest_fall_alpha = EMG_TUNE_REST_FALL_ALPHA;
    p_params->peak_decay_alpha = EMG_TUNE_PEAK_DECAY_ALPHA;
    p_params->min_span = EMG_TUNE_MIN_SPAN;
    p_params->on_ratio = EMG_TUNE_ON_RATIO;
    p_params->off_ratio = EMG_TUNE_OFF_RATIO;
    p_params->enter_count = EMG_TUNE_ENTER_COUNT;
    p_params->exit_count = EMG_TUNE_EXIT_COUNT;
    p_params->deadzone_ratio = EMG_TUNE_DEADZONE_RATIO;
    p_params->output_gamma = EMG_TUNE_OUTPUT_GAMMA;
    p_params->output_attack_alpha = EMG_TUNE_OUTPUT_ATTACK_ALPHA;
    p_params->output_release_alpha = EMG_TUNE_OUTPUT_RELEASE_ALPHA;
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

    if (span < s_emg_runtime.params.min_span)
    {
        span = s_emg_runtime.params.min_span;
    }

    /*
     * rest 只在“当前明显不像主动收缩”的区域里更新，
     * 这样可以避免用户长时间收缩时，静息基线被错误抬高。
     */
    quiet_threshold = s_emg_runtime.rest_level + (span * (s_emg_runtime.params.on_ratio * 0.5f));

    return (envelope <= quiet_threshold) || (!s_emg_runtime.active);
}

static void emg_runtime_refresh_thresholds(void)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;

    if (span < s_emg_runtime.params.min_span)
    {
        span = s_emg_runtime.params.min_span;
    }

    s_emg_runtime.threshold_on = s_emg_runtime.rest_level + (span * s_emg_runtime.params.on_ratio);
    s_emg_runtime.threshold_off = s_emg_runtime.rest_level + (span * s_emg_runtime.params.off_ratio);
}

static float emg_runtime_update_envelope(float abs_sample)
{
    float alpha;

    /*
     * 攻击 / 释放分离是这套算法比简单滑动平均更“成熟”的关键之一：
     * - 包络抬升时用较大的 attack，保证抓握响应不拖；
     * - 包络回落时用较小的 release，保证松手过程更稳。
     */
    alpha = (abs_sample > s_emg_runtime.envelope_level) ?
            s_emg_runtime.params.env_attack_alpha :
            s_emg_runtime.params.env_release_alpha;

    s_emg_runtime.envelope_level += alpha * (abs_sample - s_emg_runtime.envelope_level);

    return s_emg_runtime.envelope_level;
}

static void emg_runtime_bootstrap(float envelope)
{
    if (1U == s_emg_runtime.total_samples)
    {
        s_emg_runtime.rest_level = envelope;
        s_emg_runtime.peak_level = envelope + s_emg_runtime.params.min_span;
    }
    else
    {
        s_emg_runtime.rest_level += 0.10f * (envelope - s_emg_runtime.rest_level);

        if (s_emg_runtime.peak_level < (envelope + s_emg_runtime.params.min_span))
        {
            s_emg_runtime.peak_level = envelope + s_emg_runtime.params.min_span;
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
            s_emg_runtime.rest_level += s_emg_runtime.params.rest_fall_alpha * (envelope - s_emg_runtime.rest_level);
        }
        else
        {
            s_emg_runtime.rest_level += s_emg_runtime.params.rest_rise_alpha * (envelope - s_emg_runtime.rest_level);
        }
    }
    else if (envelope < s_emg_runtime.rest_level)
    {
        /*
         * 即使当前不属于低活动区，只要包络已经掉到 rest 以下，
         * 仍允许基线以“下降通道”快速跟下来，避免低估静息恢复速度。
         */
        s_emg_runtime.rest_level += s_emg_runtime.params.rest_fall_alpha * (envelope - s_emg_runtime.rest_level);
    }

    if (envelope > s_emg_runtime.peak_level)
    {
        s_emg_runtime.peak_level = envelope;
    }
    else
    {
        peak_floor = s_emg_runtime.rest_level + s_emg_runtime.params.min_span;
        s_emg_runtime.peak_level += s_emg_runtime.params.peak_decay_alpha * (peak_floor - s_emg_runtime.peak_level);
    }

    if (s_emg_runtime.peak_level < (s_emg_runtime.rest_level + s_emg_runtime.params.min_span))
    {
        s_emg_runtime.peak_level = s_emg_runtime.rest_level + s_emg_runtime.params.min_span;
    }

    emg_runtime_refresh_thresholds();
}

static void emg_runtime_update_activity(float envelope)
{
    if (!s_emg_runtime.active)
    {
        if (envelope >= s_emg_runtime.threshold_on)
        {
            if (s_emg_runtime.enter_counter < s_emg_runtime.params.enter_count)
            {
                s_emg_runtime.enter_counter++;
            }
        }
        else
        {
            s_emg_runtime.enter_counter = 0U;
        }

        if (s_emg_runtime.enter_counter >= s_emg_runtime.params.enter_count)
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
            if (s_emg_runtime.exit_counter < s_emg_runtime.params.exit_count)
            {
                s_emg_runtime.exit_counter++;
            }
        }
        else
        {
            s_emg_runtime.exit_counter = 0U;
        }

        if (s_emg_runtime.exit_counter >= s_emg_runtime.params.exit_count)
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

    if (normalized <= s_emg_runtime.params.deadzone_ratio)
    {
        return 0.0f;
    }

    normalized = (normalized - s_emg_runtime.params.deadzone_ratio) /
                 (1.0f - s_emg_runtime.params.deadzone_ratio);
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    /*
     * gamma 曲线本质上是在调整“手感”：
     * - gamma > 1 时，前段更稳，后段更容易拉满；
     * - gamma < 1 时，前段更灵敏。
     */
    return powf(normalized, s_emg_runtime.params.output_gamma);
}

static void emg_runtime_update_output(float envelope)
{
    float span = s_emg_runtime.peak_level - s_emg_runtime.rest_level;
    float normalized;
    float target_percent;
    float alpha;

    if (span < s_emg_runtime.params.min_span)
    {
        span = s_emg_runtime.params.min_span;
    }

    normalized = (envelope - s_emg_runtime.rest_level) / span;
    normalized = emg_runtime_clampf(normalized, 0.0f, 1.0f);

    if (!s_emg_runtime.active)
    {
        /*
         * 这里显式地把“未激活态”压回 0，是为了达成用户要的稳零防抖：
         * 即使 envelope 在死区附近有轻微起伏，只要没通过激活判定，就不输出抓握。
         */
        target_percent = 0.0f;
    }
    else
    {
        target_percent = emg_runtime_apply_curve(normalized) * 100.0f;
    }

    alpha = (target_percent > s_emg_runtime.grip_percent_f) ?
            s_emg_runtime.params.output_attack_alpha :
            s_emg_runtime.params.output_release_alpha;

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

static fsp_err_t emg_runtime_validate_float_range(float value, float min_value, float max_value)
{
    if ((!isfinite(value)) || (value < min_value) || (value > max_value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    return FSP_SUCCESS;
}

static fsp_err_t emg_runtime_validate_count(float value, uint16_t min_value, uint16_t max_value)
{
    float rounded_value;

    if (!isfinite(value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    rounded_value = floorf(value + 0.5f);
    if ((rounded_value < (float) min_value) || (rounded_value > (float) max_value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    return FSP_SUCCESS;
}
