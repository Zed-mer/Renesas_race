#include "drv_MG996.h"

// 实例化 6 个舵机的全局数组，并绑定底层硬件资源
//初始姿态，举手
// 假设：关节0,1绑GPT0；关节2,3绑GPT1；关节4,5绑GPT2
/* Runtime joint mapping used by app_arm_link:
 * servo 0 gripper, 1 wrist twist, 2 elbow, 3/4 shoulder split, 5 base.
 */
Servo_Motor_t g_servos[SERVO_NUM] = {
    // 初始角度, 目标角度, 默认步长, 定时器实例, 引脚通道
    {30.0f, 30.0f, 0.1f, &g_timer1, GPT_IO_PIN_GTIOCA}, // 舵机 0 (Base)
    {180.0f, 180.0f, 0.1f, &g_timer1, GPT_IO_PIN_GTIOCB}, // 舵机 1 (Shoulder)
    {160.0f,  160.0f, 0.1f, &g_timer2, GPT_IO_PIN_GTIOCA}, // 舵机 2 (Elbow)
    {150.0f, 150.0f, 0.1f, &g_timer2, GPT_IO_PIN_GTIOCB}, // 舵机3  (Wrist Pitch)
    {90.0f, 90.0f, 0.1f, &g_timer3, GPT_IO_PIN_GTIOCA}, // 舵机 4 (Wrist Roll)
    {120.0f, 120.0f, 0.1f, &g_timer3, GPT_IO_PIN_GTIOCB}  // 舵机 5 (Gripper)
};
///*底盘舵机5角度大于90向右转（主视图）；肩膀舵机4角度90直立位，大于90前倾；///
/// *舵机3角度大于90下压，90度伸直手臂；舵机2角度90时，伸直手臂，大于90度弯曲小臂；///
/// 手腕舵机1为180度时，手掌平，180，翻转手掌；舵机0 90度时机械爪闭合；*///
/**
 * @brief 初始化所有舵机的底层定时器
 */
/**
 * @brief 初始化所有舵机的底层定时器，并执行安全开机锁定
 */
void Servo_Init_All(void)
{
    // 1. 开启 3 个 GPT 定时器输出 PWM 波形
    R_GPT_Open(g_timer1.p_ctrl, g_timer1.p_cfg);
    R_GPT_Start(g_timer1.p_ctrl);

    R_GPT_Open(g_timer2.p_ctrl, g_timer2.p_cfg);
    R_GPT_Start(g_timer2.p_ctrl);

    R_GPT_Open(g_timer3.p_ctrl, g_timer3.p_cfg);
    R_GPT_Start(g_timer3.p_ctrl);

    // 2. 开机瞬间，立刻发送第一帧真实的 PWM 信号，把舵机物理锁定在安全位置
    // 这时候 g_servos[i].current_angle 就是你在数组里填的初始安全值
    for (uint8_t i = 0; i < SERVO_NUM; i++)
    {
        // 调用直接设置函数，强制下发 PWM，并同步结构体里的 current_angle
        Servo_SetAngle_Direct(i, g_servos[i].current_angle);
    }
}

/**
 * @brief 底层驱动：立即将指定舵机转到某个角度 (不带缓动)
 * @param servo_id 舵机编号 (1 ~ 6)
 * @param angle 目标角度 (0.0 ~ 270.0)
 */
void Servo_SetAngle_Direct(uint8_t servo_id, float angle)
{
    if (servo_id >= SERVO_NUM) return; // 安全检查

    // 1. 限制机械死区 (根据舵机 ID 独立判断)
    if (servo_id == 0)
    {
        // 舵机 0 (机械爪) 的特定安全范围：10 度到 90 度
        if (angle < 0.0f) angle = 0.0f;
        if (angle > 90.0f) angle = 90.0f;
    }
    else
    {
        // 其他舵机的常规范围：0 度到 270 度
        if (angle < 0.0f) angle = 0.0f;
        if (angle > 180.0f) angle = 180.0f;
    }

    Servo_Motor_t *servo = &g_servos[servo_id];

    // 2. 动态获取当前定时器的周期计数值
    timer_info_t info;
    R_GPT_InfoGet(servo->p_timer->p_ctrl, &info);
    uint32_t period_counts = info.period_counts;

    // 3. 计算脉宽占空比 (【注意】已修正公式！)
    // 0.5ms (2.5%) 对应 0 度，2.5ms (12.5%)
    // 所以这里的映射分母必须是 180.0f
    float duty_ratio = 0.025f + (angle / 180.0f) * 0.1f;

    uint32_t duty_counts = (uint32_t)(period_counts * duty_ratio);

    // 4. 下发到底层寄存器
    R_GPT_DutyCycleSet(servo->p_timer->p_ctrl, duty_counts, servo->pin);

    // 更新当前状态
    servo->current_angle = angle;
}

/**
 * @brief 上层 API：设置舵机目标角度和运动速度 (配合 Task 使用实现平滑转动)
 */
void Servo_SetTargetAngle(uint8_t servo_id, float target_angle, float speed_step)
{
    if (servo_id >= SERVO_NUM) return;

    if (target_angle < 0.0f) target_angle = 0.0f;
    if (target_angle > 270.0f) target_angle = 270.0f;

    g_servos[servo_id].target_angle = target_angle;
    g_servos[servo_id].speed_step = speed_step;
}

/**
 * @brief 舵机平滑运动刷新任务 (需要放到定时器中断或 RTOS 任务中周期调用，例如每 10ms 或 20ms 一次)
 */
void Servo_Update_Task(void)
{
    for (uint8_t i = 0; i < SERVO_NUM; i++) {
        Servo_Motor_t *servo = &g_servos[i];

        // 如果当前角度没有达到目标角度，则进行步进逼近
        if (servo->current_angle != servo->target_angle) {

            // 计算差值
            float diff = servo->target_angle - servo->current_angle;

            // 如果差值小于步长，直接到位
            if (diff > -servo->speed_step && diff < servo->speed_step) {
                Servo_SetAngle_Direct(i, servo->target_angle);
            }
            // 否则按照步长递增或递减
            else if (diff > 0) {
                Servo_SetAngle_Direct(i, servo->current_angle + servo->speed_step);
            }
            else {
                Servo_SetAngle_Direct(i, servo->current_angle - servo->speed_step);
            }
        }
    }
}
