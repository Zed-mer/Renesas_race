#ifndef DRV_MG996_H
#define DRV_MG996_H

#include "hal_data.h" // 必须包含 FSP 生成的头文件

// 定义 6 自由度机械臂的关节宏
#define SERVO_NUM 6

// 舵机控制结构体
typedef struct {
    float current_angle;          // 当前实际角度 (0.0 ~ 180.0)
    float target_angle;           // 期望到达的目标角度
    float speed_step;             // 运行速度 (每次任务调度的角度增量，值越大转越快)

    // 底层硬件绑定
    const timer_instance_t * p_timer; // 指向 FSP 的定时器实例 (如 &g_timer0)
    gpt_io_pin_t pin;                 // 绑定的输出引脚 (GPT_IO_PIN_GTIOCA 或 GTIOCB)
} Servo_Motor_t;

// 外部可调用的函数声明
void Servo_Init_All(void);
void Servo_SetAngle_Direct(uint8_t servo_id, float angle);
void Servo_SetTargetAngle(uint8_t servo_id, float target_angle, float speed);
void Servo_Update_Task(void);

#endif /* SERVO_CONTROL_H */
