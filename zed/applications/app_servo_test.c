#include "app_servo_test.h"
#include "drv_MG996.h"
#include <math.h>
#include <stdio.h>


#include "app_arm_link.h"

void arm_calibration_entry(void)
{
    arm_link_init();

    while (1)
    {
        /* 基准姿态 */
        /* 只测上位机舵机1 -> 代码舵机5 */
//        arm_pose_calib_test(90, 60, 90, 0);
//        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);

        arm_pose_calib_test(90, 90, 90, 180);
        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90, 90, 60, 180);
        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90, 90, 30, 180);
        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);
        arm_pose_calib_test(90, 90, 60, 180);
        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);
//        arm_pose_calib_test(90, 90, 120, 180);
//        R_BSP_SoftwareDelay(2000, BSP_DELAY_UNITS_MILLISECONDS);

    }
}


void g_timer4_callback(timer_callback_args_t *p_args)
{

        Servo_Update_Task();

}

void MG996_test(void)
{
    /* 这是舵机测试入口，与 IMU 功能相互独立。 */
    printf("OK");
    Servo_Init_All();
    printf("OK");
    R_GPT_Open(g_timer4.p_ctrl, g_timer4.p_cfg);
    R_GPT_Start(g_timer4.p_ctrl);

    Servo_SetTargetAngle(5, 120.0f, 0.5f);
    Servo_SetTargetAngle(4, 80.0f, 0.5f);
    Servo_SetTargetAngle(3, 80.0f, 0.5f);
    Servo_SetTargetAngle(2, 82.0f, 0.5f);
    Servo_SetTargetAngle(1, 30.0f, 0.5f);
    Servo_SetTargetAngle(0, 50.0f, 0.5f);
    printf("OK111");

    /* 主循环里持续输出节拍，便于观察 AGT 和舵机任务是否正常运行。 */
    while (1)
    {
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
