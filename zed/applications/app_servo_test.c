#include "app_servo_test.h"
#include "drv_MG996.h"
#include <stdio.h>

/* 仅用于调试观察定时器回调是否还在正常运行。 */
static volatile uint16_t s_agt_tick_count = 0U;

void g_timer_agt1_callback(timer_callback_args_t * p_args)
{
    if (TIMER_EVENT_CYCLE_END == p_args->event)
    {
        /* 每次定时器周期结束都推进一次舵机更新任务。 */
        s_agt_tick_count++;
        Servo_Update_Task();
    }
}

void MG996_test(void)
{
    /* 初始化全部舵机，移动到一组固定角度，并持续打印心跳信息方便观察。 */
    printf("OK");
    Servo_Init_All();
    printf("OK");
    R_AGT_Open(g_timer_agt1.p_ctrl, g_timer_agt1.p_cfg);
    R_AGT_Start(g_timer_agt1.p_ctrl);

    Servo_SetTargetAngle(5, 120.0f, 0.5f);
    Servo_SetTargetAngle(4, 80.0f, 0.5f);
    Servo_SetTargetAngle(3, 80.0f, 0.5f);
    Servo_SetTargetAngle(2, 82.0f, 0.5f);
    Servo_SetTargetAngle(1, 30.0f, 0.5f);
    Servo_SetTargetAngle(0, 50.0f, 0.5f);
    printf("OK111");

    while (1)
    {
        printf("AGT Tick: %d\r\n", s_agt_tick_count);
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
