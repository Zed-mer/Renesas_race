#ifndef EMG_RUNTIME_H
#define EMG_RUNTIME_H

#include "emg_tune_cfg.h"
#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 这份结构体保存“可以通过串口调参的那组算法参数”。
 * 之所以把参数集中成结构体，而不是散落成一堆全局变量，
 * 是为了同时满足三件事：
 * 1. 固件内部运行时可以统一读写；
 * 2. 串口协议可以批量导出和回灌；
 * 3. 上位机导出的宏定义可以一一对应到默认值。
 */
typedef struct st_emg_tune_params
{
    float    env_attack_alpha;
    float    env_release_alpha;
    float    rest_rise_alpha;
    float    rest_fall_alpha;
    float    peak_decay_alpha;
    float    min_span;
    float    on_ratio;
    float    off_ratio;
    uint16_t enter_count;
    uint16_t exit_count;
    float    deadzone_ratio;
    float    output_gamma;
    float    output_attack_alpha;
    float    output_release_alpha;
} emg_tune_params_t;

/*
 * 这份快照专门给调试串口和上位机使用。
 * 它把算法内部最关键的观测量集中暴露出来，便于判断：
 * - 原始采样有没有在跳；
 * - 滤波后有没有成形；
 * - 包络是否稳定；
 * - rest / peak 是否在合理范围；
 * - 当前 grip 是被阈值卡住了，还是被映射曲线压住了。
 */
typedef struct st_emg_debug_snapshot
{
    uint16_t raw;
    float    filtered;
    float    envelope;
    float    rest;
    float    peak;
    float    th_on;
    float    th_off;
    uint8_t  grip;
    bool     active;
    uint32_t sample_count;
} emg_debug_snapshot_t;

fsp_err_t emg_runtime_init(void);
void      emg_runtime_poll(uint32_t now_us);

/*
 * 正式模式只需要一个 0~100 的 grip 输出。
 * 上层通过这个接口拿到“已经调完算法后的最终结果”，
 * 不需要知道内部 rest / peak / threshold 的细节。
 */
uint8_t   emg_runtime_get_grip_percent(void);

/*
 * 下面这几个接口主要服务于测试模式和调参模式。
 * 纯 EMG 数据流模式用 last_filtered / last_envelope 输出简洁文本，
 * 调参模式则通过 get_params / set_param / get_debug_snapshot 和上位机交互。
 */
uint16_t  emg_runtime_get_last_raw_sample(void);
float     emg_runtime_get_last_filtered_value(void);
float     emg_runtime_get_last_envelope(void);
uint32_t  emg_runtime_get_total_samples(void);
void      emg_runtime_get_params(emg_tune_params_t * p_params);
fsp_err_t emg_runtime_set_param(char const * p_name, float value);
void      emg_runtime_reset_params_to_defaults(void);
void      emg_runtime_get_debug_snapshot(emg_debug_snapshot_t * p_snapshot);

#endif /* EMG_RUNTIME_H */
