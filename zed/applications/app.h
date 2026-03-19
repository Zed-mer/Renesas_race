#ifndef APP_H
#define APP_H

#include "hal_data.h"

//当宏定义为1，那就只有肌电发挥作用
#ifndef IMU_EMG_ONLY_TEST
#define IMU_EMG_ONLY_TEST 0
#endif


//当宏定义为1，打开IMU的补丁
#ifndef change_v9_1
#define change_v9_1 0
#endif

/* 板级启动代码会从这里进入不同的应用测试入口。 */
//void emgsignal_test(void);
//void zhua_test(void);
void imu_test(void);
void MG996_test(void);
void arm_calibration_entry(void);
/* 这些中断回调只负责把硬件事件转交给应用层状态机处理。 */
void icu8_callback(external_irq_callback_args_t * p_args);
void icu9_callback(external_irq_callback_args_t * p_args);
void botton6_callback(external_irq_callback_args_t * p_args);
void g_timer4_callback(timer_callback_args_t * p_args);

#endif
