#include "app_servo_test.h"
#include "app_arm_link.h"
#include "drv_MG996.h"
#include <stdio.h>

void arm_calibration_entry(void)
{
    arm_link_init();

    while (1)
    {
        arm_pose_calib_test(90.0f, 90.0f, 90.0f, 180.0f);
        R_BSP_SoftwareDelay(2000U, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90.0f, 90.0f, 60.0f, 180.0f);
        R_BSP_SoftwareDelay(2000U, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90.0f, 90.0f, 30.0f, 180.0f);
        R_BSP_SoftwareDelay(2000U, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90.0f, 90.0f, 60.0f, 180.0f);
        R_BSP_SoftwareDelay(2000U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

void g_timer4_callback(timer_callback_args_t * p_args)
{
    FSP_PARAMETER_NOT_USED(p_args);
    Servo_Update_Task();
}

void MG996_test(void)
{
    printf("OK");
    Servo_Init_All();
    printf("OK");
    R_GPT_Open(g_timer4.p_ctrl, g_timer4.p_cfg);
    R_GPT_Start(g_timer4.p_ctrl);

    Servo_SetTargetAngle(5U, 120.0f, 0.5f);
    Servo_SetTargetAngle(4U, 80.0f, 0.5f);
    Servo_SetTargetAngle(3U, 80.0f, 0.5f);
    Servo_SetTargetAngle(2U, 82.0f, 0.5f);
    Servo_SetTargetAngle(1U, 30.0f, 0.5f);
    Servo_SetTargetAngle(0U, 50.0f, 0.5f);
    printf("OK111");

    while (1)
    {
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
