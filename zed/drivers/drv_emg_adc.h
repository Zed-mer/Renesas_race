#ifndef DRV_EMG_ADC_H
#define DRV_EMG_ADC_H

#include "hal_data.h"
#include <stdint.h>

fsp_err_t drv_emg_adc_init(void);
fsp_err_t drv_emg_adc_read_sample(uint16_t * p_sample);
void      drv_emg_filter_reset(void);
float     drv_emg_filter(float input);
void      emg_dma_callback(dmac_callback_args_t * p_args);

#endif /* DRV_EMG_ADC_H */
