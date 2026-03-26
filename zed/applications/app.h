#ifndef APP_H
#define APP_H

#include "hal_data.h"

/* Board-level entry points for the IMU application. */
void imu_test(void);
void adc_emg_print_test(void);
void MG996_test(void);
void arm_calibration_entry(void);

/* IRQ callbacks only forward hardware events into the app state machine. */
void icu8_callback(external_irq_callback_args_t * p_args);
void icu9_callback(external_irq_callback_args_t * p_args);
void botton6_callback(external_irq_callback_args_t * p_args);
void g_timer4_callback(timer_callback_args_t * p_args);

#endif
