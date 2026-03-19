#ifndef APP_ARM_LINK_H
#define APP_ARM_LINK_H

#include "imu_app_context.h"

//**上位机舵机1 -> 代码舵机5
//上位机舵机2 -> 代码舵机4
//上位机舵机3 -> 代码舵机3 和 代码舵机2 联动
//上位机舵机4 -> 代码舵机1
//机械爪       -> 代码舵机0（后面由肌电控制）
//**
void arm_link_init(void);
void arm_apply_imu_pose_to_servos(const imu_servo_pose_t *pose);
void arm_pose_calib_test(float a1, float a2, float a3, float a4);
#endif
