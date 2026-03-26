#ifndef EMG_RUNTIME_H
#define EMG_RUNTIME_H

#include "emg_tune_cfg.h"
#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct st_emg_tune_params
{
    uint16_t envelope_window_size;
} emg_tune_params_t;

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
fsp_err_t emg_runtime_process_next_sample(void);
uint8_t   emg_runtime_get_grip_percent(void);
uint16_t  emg_runtime_get_last_raw_sample(void);
float     emg_runtime_get_last_filtered_value(void);
float     emg_runtime_get_last_envelope(void);
uint32_t  emg_runtime_get_total_samples(void);
void      emg_runtime_get_params(emg_tune_params_t * p_params);
fsp_err_t emg_runtime_set_param(char const * p_name, float value);
void      emg_runtime_reset_params_to_defaults(void);
void      emg_runtime_get_debug_snapshot(emg_debug_snapshot_t * p_snapshot);

#endif /* EMG_RUNTIME_H */
