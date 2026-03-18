#ifndef IMU_PROTOCOL_H
#define IMU_PROTOCOL_H

#include "imu_app_context.h"

/* 串口协议辅助函数：负责调试文本、标定提示和姿态帧输出。 */
void imu_protocol_send_text(imu_app_context_t * p_ctx, char const * p_text);
void imu_protocol_send_textf(imu_app_context_t * p_ctx, char const * p_format, ...);
void imu_protocol_send_cal_step(imu_app_context_t * p_ctx);
void imu_protocol_send_cal_ok(imu_app_context_t * p_ctx, imu_cal_step_t step);
void imu_protocol_send_cal_error(imu_app_context_t * p_ctx, imu_cal_result_t result, imu_cal_step_t step);
void imu_protocol_send_cal_done(imu_app_context_t * p_ctx);
void imu_protocol_send_cal_state(imu_app_context_t * p_ctx);
void imu_protocol_send_pose_frame(imu_app_context_t * p_ctx, imu_servo_pose_t const * p_pose);
void imu_protocol_handle_uart_commands(imu_app_context_t * p_ctx, uint32_t now_us);

#endif
