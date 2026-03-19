#ifndef IMU_PROTOCOL_H
#define IMU_PROTOCOL_H

#include "imu_app_context.h"

/*
 * 串口协议模块统一负责三类文本报文：
 * 1. IMU 原有的 CAL 标定状态报文；
 * 2. IMU 原有的 POSE 实时姿态报文；
 * 3. 本次新增的 EMGCFG / EMGDBG 调参与调试报文。
 *
 * 这样做的目的是把“格式化字符串”和“业务状态机”拆开，
 * 避免应用层到处散落 printf，后续改协议时更容易收口。
 */
void imu_protocol_send_text(imu_app_context_t * p_ctx, char const * p_text);
void imu_protocol_send_textf(imu_app_context_t * p_ctx, char const * p_format, ...);
void imu_protocol_send_cal_step(imu_app_context_t * p_ctx);
void imu_protocol_send_cal_ok(imu_app_context_t * p_ctx, imu_cal_step_t step);
void imu_protocol_send_cal_error(imu_app_context_t * p_ctx, imu_cal_result_t result, imu_cal_step_t step);
void imu_protocol_send_cal_done(imu_app_context_t * p_ctx);
void imu_protocol_send_cal_state(imu_app_context_t * p_ctx);
void imu_protocol_send_pose_frame(imu_app_context_t * p_ctx, imu_servo_pose_t const * p_pose);
void imu_protocol_send_emg_debug_frame(imu_app_context_t * p_ctx);
void imu_protocol_handle_uart_commands(imu_app_context_t * p_ctx, uint32_t now_us);

#endif
