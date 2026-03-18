#ifndef IMU_CALIBRATION_H
#define IMU_CALIBRATION_H

#include "imu_app_context.h"

void imu_calibration_reset(imu_app_context_t * p_ctx);
void imu_calibration_begin(imu_app_context_t * p_ctx, uint32_t now_us);
void imu_calibration_handle_next(imu_app_context_t * p_ctx, uint32_t now_us);
bool imu_try_build_servo_pose(imu_app_context_t * p_ctx, imu_servo_pose_t * p_pose);

#endif
