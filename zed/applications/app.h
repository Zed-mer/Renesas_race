#ifndef APP_H
#define APP_H

#include "hal_data.h"

/* 板级启动代码会从这里进入不同的应用测试入口。 */
void app_test(void);
void imu_test(void);
void MG996_test(void);

/* 这些中断回调只负责把硬件事件转交给应用层状态机处理。 */
void icu8_callback(external_irq_callback_args_t * p_args);
void icu9_callback(external_irq_callback_args_t * p_args);
void botton6_callback(external_irq_callback_args_t * p_args);
void g_timer_agt1_callback(timer_callback_args_t * p_args);

#endif
