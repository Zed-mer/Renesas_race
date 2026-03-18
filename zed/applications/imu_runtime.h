#ifndef IMU_RUNTIME_H
#define IMU_RUNTIME_H

#include "imu_app_context.h"

typedef void (*imu_runtime_wait_hook_t)(void * p_context, uint32_t now_us);

void     imu_runtime_reset(imu_runtime_t * p_imu);
void     imu_timebase_init(imu_timebase_t * p_timebase);
uint32_t imu_time_now_us(imu_timebase_t * p_timebase);
float    imu_calc_dt_sec(imu_runtime_t * p_imu, uint32_t sample_time_us);
void     imu_mahony_update(imu_runtime_t * p_imu,
                           icm42688Float3_t const * p_acc_g,
                           icm42688Float3_t const * p_gyro_rad_s,
                           float dt_sec);
uint32_t imu_collect_gyro_bias(imu_runtime_t * p_imu,
                               imu_read_sample_fn_t read_sample,
                               imu_timebase_t * p_timebase,
                               imu_runtime_wait_hook_t wait_hook,
                               void * p_wait_context);
void     imu_read_sample_blocking(imu_runtime_t * p_imu,
                                  imu_read_sample_fn_t read_sample,
                                  icm42688Float3_t * p_acc_g,
                                  icm42688Float3_t * p_gyro_rad_s);
bool     imu_try_read_sample(imu_runtime_t * p_imu,
                             imu_read_sample_fn_t read_sample,
                             icm42688Float3_t * p_acc_g,
                             icm42688Float3_t * p_gyro_rad_s,
                             uint32_t * p_sample_time_us,
                             imu_timebase_t * p_timebase);

#endif
