#ifndef IMU_CALIBRATION_H
#define IMU_CALIBRATION_H

#include "imu_app_context.h"

/* 清空已学习到的标定结果，并回到标定流程的第一步。 */
void imu_calibration_reset(imu_app_context_t * p_ctx);
/* 启动或重新启动整套引导式标定流程。 */
void imu_calibration_begin(imu_app_context_t * p_ctx, uint32_t now_us);
/* 在用户完成当前姿态摆放后，推进到下一步标定。 */
void imu_calibration_handle_next(imu_app_context_t * p_ctx, uint32_t now_us);
/* 在标定有效时，把最新 IMU 姿态换算成目标舵机角度。 */
bool imu_try_build_servo_pose(imu_app_context_t * p_ctx, imu_servo_pose_t * p_pose);

#endif
