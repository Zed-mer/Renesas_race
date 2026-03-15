/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "app.h"
#include "drv_adc0.h"
#include "drv_uart.h"
#include "hal_data.h"
#include "icm42688.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "drv_MG996.h"
/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define ENVELOPE_BUFFER_SIZE          16
#define IMU_SAMPLE_DT_SEC            0.01f
#define MAHONY_KP                    2.5f
#define MAHONY_KI                    0.08f
#define IMU_CALIBRATION_SAMPLES      200U
#define IMU_CALIBRATION_MAX_ATTEMPTS 400U
#define IMU_STATIC_ACC_MIN_G         0.85f
#define IMU_STATIC_ACC_MAX_G         1.15f
#define IMU_CORRECTION_ACC_MIN_G     0.70f
#define IMU_CORRECTION_ACC_MAX_G     1.30f
#define IMU_POLL_INTERVAL_MS         10U
#define IMU_IRQ_WAIT_TIMEOUT_MS      20U

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static float get_envelope(float sample);
static float vector_norm(icm42688Float3_t const * v);
static void quaternion_normalize(Quaternion_t * q);
static void mahony_reset(void);
static void mahony_update(icm42688Float3_t const *acc_g, icm42688Float3_t const *gyro_rad_s, float dt_sec);
static uint32_t imu_collect_gyro_bias(icm42688Float3_t *gyro_bias);
static void imu_read_sample(icm42688Float3_t *acc_g, icm42688Float3_t *gyro_rad_s);
static uint16_t crc16_ccitt(uint8_t const *data, uint16_t length);
static void pack_u16_le(uint8_t *buffer, uint16_t value);
static void pack_u32_le(uint8_t *buffer, uint32_t value);
static void pack_f32_le(uint8_t *buffer, float value);
static void send_telemetry_frame(Quaternion_t const *upper_quat, Quaternion_t const *lower_quat);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
uint16_t g_adc_buffer[1] = {0};
volatile uint16_t g_adc_flag = 0;

static float envelope_buffer[ENVELOPE_BUFFER_SIZE];
static int envelope_index = 0;
static float envelope_sum = 0;

static Quaternion_t s_quat = {1.0f, 0.0f, 0.0f, 0.0f};
static Quaternion_t s_quat_reserved = {1.0f, 0.0f, 0.0f, 0.0f};
static icm42688Float3_t s_gyro_bias = {0.0f, 0.0f, 0.0f};
static icm42688Float3_t s_mahony_integral = {0.0f, 0.0f, 0.0f};
static volatile bool s_imu_data_ready = false;
static uint16_t s_frame_sequence = 0U;
static uint32_t s_frame_timestamp_ms = 0U;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
static float get_envelope(float sample)
{
    envelope_sum -= envelope_buffer[envelope_index];
    envelope_sum += sample;
    envelope_buffer[envelope_index] = sample;

    envelope_index = (envelope_index + 1) % ENVELOPE_BUFFER_SIZE;

    return envelope_sum / ENVELOPE_BUFFER_SIZE;
}

void app_test(void)
{
    adcdrvinit();
    while (1)
    {
        float filtered_value;
        float envelope_value;

        ADCDrvRead(g_adc_buffer, 1);
        filtered_value = Filter((float) g_adc_buffer[0]);
        envelope_value = get_envelope(fabsf(filtered_value));
        printf("Filtered: %.2f, %.2f\r\n", filtered_value, envelope_value);
    }
}

void icu8_callback(external_irq_callback_args_t *p_args)
{
    if ((NULL != p_args) && (8 == p_args->channel))
    {
        s_imu_data_ready = true;
    }
}

void imu_test(void)
{
    fsp_err_t err;
    uint32_t valid_calibration_samples;

    err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return;
    }

    err = bsp_Icm42688Init();
    if (FSP_SUCCESS != err)
    {
        return;
    }

    err = R_ICU_ExternalIrqOpen(g_external_irq8.p_ctrl, g_external_irq8.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return;
    }

    err = R_ICU_ExternalIrqEnable(g_external_irq8.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return;
    }

    s_imu_data_ready = false;
    s_frame_sequence = 0U;
    s_frame_timestamp_ms = 0U;
    mahony_reset();

    valid_calibration_samples = imu_collect_gyro_bias(&s_gyro_bias);
    (void) valid_calibration_samples;

    while (1)
    {
        icm42688Float3_t acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t gyro_rad_s = {0.0f, 0.0f, 0.0f};

        imu_read_sample(&acc_g, &gyro_rad_s);

        gyro_rad_s.x -= s_gyro_bias.x;
        gyro_rad_s.y -= s_gyro_bias.y;
        gyro_rad_s.z -= s_gyro_bias.z;

        mahony_update(&acc_g, &gyro_rad_s, IMU_SAMPLE_DT_SEC);
        send_telemetry_frame(&s_quat, &s_quat_reserved);
    }
}

static float vector_norm(icm42688Float3_t const * v)
{
    return sqrtf((v->x * v->x) + (v->y * v->y) + (v->z * v->z));
}

static void quaternion_normalize(Quaternion_t * q)
{
    float norm = sqrtf((q->q0 * q->q0) + (q->q1 * q->q1) + (q->q2 * q->q2) + (q->q3 * q->q3));

    if (norm <= 0.0f)
    {
        q->q0 = 1.0f;
        q->q1 = 0.0f;
        q->q2 = 0.0f;
        q->q3 = 0.0f;
        return;
    }

    q->q0 /= norm;
    q->q1 /= norm;
    q->q2 /= norm;
    q->q3 /= norm;
}

static void mahony_reset(void)
{
    s_quat.q0 = 1.0f;
    s_quat.q1 = 0.0f;
    s_quat.q2 = 0.0f;
    s_quat.q3 = 0.0f;

    s_mahony_integral.x = 0.0f;
    s_mahony_integral.y = 0.0f;
    s_mahony_integral.z = 0.0f;
}

static void mahony_update(icm42688Float3_t const *acc_g, icm42688Float3_t const *gyro_rad_s, float dt_sec)
{
    float acc_norm;
    float gx = gyro_rad_s->x;
    float gy = gyro_rad_s->y;
    float gz = gyro_rad_s->z;

    acc_norm = vector_norm(acc_g);
    if ((acc_norm >= IMU_CORRECTION_ACC_MIN_G) && (acc_norm <= IMU_CORRECTION_ACC_MAX_G))
    {
        float ax = acc_g->x / acc_norm;
        float ay = acc_g->y / acc_norm;
        float az = acc_g->z / acc_norm;
        float vx = 2.0f * ((s_quat.q1 * s_quat.q3) - (s_quat.q0 * s_quat.q2));
        float vy = 2.0f * ((s_quat.q0 * s_quat.q1) + (s_quat.q2 * s_quat.q3));
        float vz = (s_quat.q0 * s_quat.q0) - (s_quat.q1 * s_quat.q1) - (s_quat.q2 * s_quat.q2) + (s_quat.q3 * s_quat.q3);
        float ex = (ay * vz) - (az * vy);
        float ey = (az * vx) - (ax * vz);
        float ez = (ax * vy) - (ay * vx);

        s_mahony_integral.x += MAHONY_KI * ex * dt_sec;
        s_mahony_integral.y += MAHONY_KI * ey * dt_sec;
        s_mahony_integral.z += MAHONY_KI * ez * dt_sec;

        gx += (MAHONY_KP * ex) + s_mahony_integral.x;
        gy += (MAHONY_KP * ey) + s_mahony_integral.y;
        gz += (MAHONY_KP * ez) + s_mahony_integral.z;
    }

    {
        float q0 = s_quat.q0;
        float q1 = s_quat.q1;
        float q2 = s_quat.q2;
        float q3 = s_quat.q3;
        float half_dt = 0.5f * dt_sec;

        s_quat.q0 += (-q1 * gx - q2 * gy - q3 * gz) * half_dt;
        s_quat.q1 += (q0 * gx + q2 * gz - q3 * gy) * half_dt;
        s_quat.q2 += (q0 * gy - q1 * gz + q3 * gx) * half_dt;
        s_quat.q3 += (q0 * gz + q1 * gy - q2 * gx) * half_dt;
    }

    quaternion_normalize(&s_quat);
}

static uint32_t imu_collect_gyro_bias(icm42688Float3_t *gyro_bias)
{
    uint32_t attempts = 0;
    uint32_t valid_samples = 0;
    icm42688Float3_t gyro_sum = {0.0f, 0.0f, 0.0f};

    while ((attempts < IMU_CALIBRATION_MAX_ATTEMPTS) && (valid_samples < IMU_CALIBRATION_SAMPLES))
    {
        icm42688Float3_t acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t gyro_rad_s = {0.0f, 0.0f, 0.0f};
        float acc_norm;

        imu_read_sample(&acc_g, &gyro_rad_s);
        attempts++;
        acc_norm = vector_norm(&acc_g);
        if ((acc_norm < IMU_STATIC_ACC_MIN_G) || (acc_norm > IMU_STATIC_ACC_MAX_G))
        {
            continue;
        }

        gyro_sum.x += gyro_rad_s.x;
        gyro_sum.y += gyro_rad_s.y;
        gyro_sum.z += gyro_rad_s.z;
        valid_samples++;
    }

    if (0U == valid_samples)
    {
        gyro_bias->x = 0.0f;
        gyro_bias->y = 0.0f;
        gyro_bias->z = 0.0f;
        return 0U;
    }

    gyro_bias->x = gyro_sum.x / (float) valid_samples;
    gyro_bias->y = gyro_sum.y / (float) valid_samples;
    gyro_bias->z = gyro_sum.z / (float) valid_samples;

    return valid_samples;
}

static void imu_read_sample(icm42688Float3_t *acc_g, icm42688Float3_t *gyro_rad_s)
{
    uint32_t wait_ms = 0;

    while ((!s_imu_data_ready) && (wait_ms < IMU_IRQ_WAIT_TIMEOUT_MS))
    {
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        wait_ms++;
    }

    s_imu_data_ready = false;
    bsp_IcmGetScaledData(acc_g, gyro_rad_s);

    if (wait_ms < IMU_IRQ_WAIT_TIMEOUT_MS)
    {
        R_BSP_SoftwareDelay(IMU_POLL_INTERVAL_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

static uint16_t crc16_ccitt(uint8_t const *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    for (i = 0; i < length; i++)
    {
        uint8_t bit;

        crc ^= (uint16_t) (data[i] << 8);
        for (bit = 0; bit < 8U; bit++)
        {
            if (0U != (crc & 0x8000U))
            {
                crc = (uint16_t) ((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void pack_u16_le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t) (value & 0xFFU);
    buffer[1] = (uint8_t) ((value >> 8) & 0xFFU);
}

static void pack_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t) (value & 0xFFU);
    buffer[1] = (uint8_t) ((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t) ((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t) ((value >> 24) & 0xFFU);
}

static void pack_f32_le(uint8_t *buffer, float value)
{
    uint32_t raw = 0U;

    memcpy(&raw, &value, sizeof(raw));
    pack_u32_le(buffer, raw);
}

static void send_telemetry_frame(Quaternion_t const *upper_quat, Quaternion_t const *lower_quat)
{
    uint8_t frame[48] = {0};
    uint16_t crc;

    frame[0] = 0xA5U;
    frame[1] = 0x5AU;
    frame[2] = 0x01U;
    frame[3] = 0x01U;

    pack_u16_le(&frame[4], s_frame_sequence);
    pack_u32_le(&frame[6], s_frame_timestamp_ms);

    pack_f32_le(&frame[10], upper_quat->q0);
    pack_f32_le(&frame[14], upper_quat->q1);
    pack_f32_le(&frame[18], upper_quat->q2);
    pack_f32_le(&frame[22], upper_quat->q3);

    pack_f32_le(&frame[26], lower_quat->q0);
    pack_f32_le(&frame[30], lower_quat->q1);
    pack_f32_le(&frame[34], lower_quat->q2);
    pack_f32_le(&frame[38], lower_quat->q3);

    frame[42] = 0U;
    frame[43] = 0U;
    pack_u16_le(&frame[44], 0U);

    crc = crc16_ccitt(frame, 46U);
    pack_u16_le(&frame[46], crc);

    if (FSP_SUCCESS == g_uart7.p_api->write(g_uart7.p_ctrl, frame, sizeof(frame)))
    {
        drv_uart_wait_for_tx();
    }

    s_frame_sequence = (uint16_t) (s_frame_sequence + 1U);
    s_frame_timestamp_ms += (uint32_t) (IMU_SAMPLE_DT_SEC * 1000.0f);
}

//******机械臂测试******//

volatile uint16_t g_agt_tick_count = 0;
void g_timer_agt1_callback(timer_callback_args_t *p_args)
{
    // 检查中断事件类型是否为周期结束 (Timer 溢出)
    if (TIMER_EVENT_CYCLE_END == p_args->event)
    {
        g_agt_tick_count++;
        // 调用舵机刷新任务
        Servo_Update_Task();
    }
}
void MG996_test()
{
    printf("OK");
    /* 1. 初始化所有硬件 */
        // 初始化 6 个舵机的 PWM 输出 (开启前面配置的 3 个 GPT)
        Servo_Init_All();
        printf("OK");
        // 初始化并开启我们刚刚配置的 AGT 定时器
        R_AGT_Open(g_timer_agt1.p_ctrl, g_timer_agt1.p_cfg);
        R_AGT_Start(g_timer_agt1.p_ctrl);

        /* 2. 发送运动指令 (非阻塞式) */
        // 让底座(舵机0)以 0.5 度的步长缓慢转到 135 度
        Servo_SetTargetAngle(5, 120, 0.5f);
        Servo_SetTargetAngle(4, 80, 0.5f);
        Servo_SetTargetAngle(3, 80, 0.5f);
        Servo_SetTargetAngle(2, 82, 0.5f);
        Servo_SetTargetAngle(1, 30.0f, 0.5f);
        // 让机械爪(舵机0)以 2.0 度的步长快速闭合到 30 度
        Servo_SetTargetAngle(0, 50, 0.5f);
        printf("OK111");

        /* 3. 主循环 */
        while (1)
        {
            // 现在的 while(1) 里面什么都不用管了！非常清爽！
            // 机械臂会在后台按照指令自己平滑移动。
            // 您可以在这里做其他任何事情，比如：
            // 1. 处理串口/蓝牙发来的上位机指令
            // 2. 刷新 OLED 屏幕显示当前各个关节的角度
            // 3. 运行复杂的逆运动学(IK)解算逻辑
            printf("AGT Tick: %d\r\n", g_agt_tick_count);
            R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS); // 随便给个延时让CPU喘口气
        }
}
