#ifndef APP_H
#define APP_H

#include "hal_data.h"

void app_test(void);
void imu_test(void);
void MG996_test(void);
void icu8_callback(external_irq_callback_args_t * p_args);
void icu9_callback(external_irq_callback_args_t * p_args);
void botton6_callback(external_irq_callback_args_t * p_args);
void g_timer_agt1_callback(timer_callback_args_t * p_args);

#endif
