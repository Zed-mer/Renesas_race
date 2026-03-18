#ifndef APP_SERVO_TEST_H
#define APP_SERVO_TEST_H

#include "hal_data.h"

/* AGT 定时器回调用固定节拍驱动舵机轨迹更新。 */
void g_timer_agt1_callback(timer_callback_args_t * p_args);
/* 独立舵机测试入口，用于上电联调和角度验证。 */
void MG996_test(void);

#endif
