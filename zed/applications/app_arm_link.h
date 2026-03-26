#ifndef APP_ARM_LINK_H
#define APP_ARM_LINK_H

#include "hal_data.h"
#include "imu_app_context.h"
#include <stdint.h>

typedef struct st_arm_emg_servo0_cfg
{
    float envelope_open;
    float envelope_close;
    float envelope_alpha;
    float grip_alpha;
    float grip_deadband_percent;
    float grip_hold_percent;
    float servo_open_angle;
    float servo_close_angle;
    float servo_speed_step;
} arm_emg_servo0_cfg_t;

void      arm_link_init(void);
void      arm_apply_imu_pose_to_servos(const imu_servo_pose_t * p_pose);
void      arm_apply_emg_envelope_to_servo0(float envelope);
uint8_t   arm_get_emg_grip_percent(void);
float     arm_get_emg_filtered_envelope(void);
void      arm_get_emg_servo0_config(arm_emg_servo0_cfg_t * p_cfg);
void      arm_reset_emg_servo0_config(void);
fsp_err_t arm_set_emg_servo0_param(char const * p_name, float value);
void      arm_pose_calib_test(float base_deg, float shoulder_deg, float elbow_deg, float wrist_deg);

#endif
