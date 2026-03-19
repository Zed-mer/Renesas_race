#ifndef APP_H
#define APP_H

#include "hal_data.h"

/*
 * 下面这几个宏统一放在 app.h 中集中管理，避免同一个工程里在多个头文件重复定义，
 * 最后导致“某个 .c 文件看到的是 0，另一个 .c 文件看到的是 1”这种很难排查的编译期分叉。
 *
 * 宏之间的组合关系约定如下：
 * 1. IMU_EMG_ONLY_TEST == 1 && emg_dbg == 1
 *    进入“肌电调参模式”。
 *    只初始化 UART、时间基和 EMG 采样链路，不初始化 IMU。
 *    固件发送 EMGDBG 调试帧，并接受 EMGCFG 串口调参命令。
 *
 * 2. IMU_EMG_ONLY_TEST == 1 && emg_dbg == 0
 *    进入“肌电数据流模式”。
 *    同样不初始化 IMU，只持续输出 Filtered: <filtered>, <envelope> 文本流，
 *    方便用串口助手快速看滤波值和包络值是否正常。
 *
 * 3. IMU_EMG_ONLY_TEST == 0 && emg_do == 1
 *    进入“正式融合模式”。
 *    原有 IMU 姿态逻辑继续工作，只把手掌 grip 字段替换成 EMG 算法输出。
 *
 * 4. IMU_EMG_ONLY_TEST == 0 && emg_do == 0
 *    保持当前主业务行为，不让 EMG 正式接管 grip 输出。
 */
#ifndef IMU_EMG_ONLY_TEST
#define IMU_EMG_ONLY_TEST 0
#endif

#ifndef emg_dbg
#define emg_dbg 0
#endif

#ifndef emg_do
#define emg_do 0
#endif

/*
 * change_v9_1 是当前 IMU 标定链路里已经在使用的补丁开关。
 * 这里保留它的唯一真源，其他头文件不再重复定义，避免宏值冲突。
 */
#ifndef change_v9_1
#define change_v9_1 0
#endif

/* 板级入口函数。 */
void imu_test(void);
void MG996_test(void);
void arm_calibration_entry(void);

/*
 * 这些中断回调只负责把硬件事件转交给应用层状态机，
 * 具体业务逻辑仍由应用层统一处理。
 */
void icu8_callback(external_irq_callback_args_t * p_args);
void icu9_callback(external_irq_callback_args_t * p_args);
void botton6_callback(external_irq_callback_args_t * p_args);
void g_timer4_callback(timer_callback_args_t * p_args);

#endif
