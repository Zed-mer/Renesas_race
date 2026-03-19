#ifndef EMG_RUNTIME_H
#define EMG_RUNTIME_H

#include "emg_tune_cfg.h"
#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * 当前版本对外只开放一个调参量：包络滑动窗口长度。
 * 这样做是为了和老工程 ra6m5_ds_musle 的“16 点滑动平均包络”保持一致，
 * 让调参动作重新回到“调窗口大小”这个更直观的维度。
 */
typedef struct st_emg_tune_params
{
    uint16_t envelope_window_size;
} emg_tune_params_t;

/*
 * 调试快照仍然保留完整内部观测量，方便上位机判断：
 * - filtered 是否正常成形；
 * - envelope 是否过抖或过钝；
 * - rest / peak / threshold 是否处于合理区间；
 * - active 和 grip 是否符合预期。
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
uint8_t   emg_runtime_get_grip_percent(void);

/*
 * 下面这些接口主要服务于纯 EMG 文本流模式和调参模式。
 * 正式主业务只需要最终的 grip 百分比。
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
