#ifndef EMG_RUNTIME_H
#define EMG_RUNTIME_H

#include "hal_data.h"
#include <stdint.h>

fsp_err_t emg_runtime_init(void);
void      emg_runtime_poll(uint32_t now_us);
uint8_t   emg_runtime_get_grip_percent(void);
uint16_t  emg_runtime_get_last_raw_sample(void);
float     emg_runtime_get_last_filtered_value(void);
float     emg_runtime_get_last_envelope(void);
uint32_t  emg_runtime_get_total_samples(void);

#endif /* EMG_RUNTIME_H */
