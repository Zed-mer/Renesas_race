/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "app.h"
#include "drv_adc0.h"
#include "drv_uart.h"
#include "hal_data.h"
#include "icm42688.h"
#include "drv_MG996.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
/* 包络检测使用的滑动平均窗口长度。 */
#define ENVELOPE_BUFFER_SIZE          16
/* IMU 主循环期望采样周期：10ms，对应 100Hz。 */
#define IMU_SAMPLE_DT_DEFAULT_SEC     0.01f
#define IMU_SAMPLE_DT_MIN_SEC         0.001f
#define IMU_SAMPLE_DT_MAX_SEC         0.030f
/* Mahony 互补滤波比例项增益，决定姿态误差校正强度。 */
#define MAHONY_KP                     2.5f
/* Mahony 互补滤波积分项增益，用于慢速消除陀螺零偏。 */
#define MAHONY_KI                     0.08f
/* 陀螺零偏标定时，目标有效样本数。 */
#define IMU_CALIBRATION_SAMPLES       200U
/* 标定最多尝试次数，防止长期等不到满足静止条件的数据。 */
#define IMU_CALIBRATION_MAX_ATTEMPTS  400U
/* 判定“静止标定可用”的加速度模长范围，单位 g。 */
#define IMU_STATIC_ACC_MIN_G          0.85f
#define IMU_STATIC_ACC_MAX_G          1.15f
/* 姿态修正时使用加速度的可信区间，避免剧烈运动时被线性加速度误导。 */
#define IMU_CORRECTION_ACC_MIN_G      0.70f
#define IMU_CORRECTION_ACC_MAX_G      1.30f
/* 等待 Data Ready 中断的最长时间，超时后仍会读一次，退化为近似轮询。 */
#define IMU_IRQ_WAIT_TIMEOUT_MS       20U
#define IMU_IDLE_POLL_DELAY_US        200U
#define TELEMETRY_MIN_INTERVAL_US     10000U

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/
/* 
 * 每颗 IMU 的运行时状态。
 * 这里把姿态解算需要长期保存的量都放在一起，便于一套逻辑同时服务两颗 IMU：
 * - quat: 当前姿态四元数
 * - gyro_bias: 上电静止标定得到的陀螺零偏
 * - mahony_integral: Mahony 算法积分项
 * - data_ready: 对应外部中断置位的数据就绪标志
 */
typedef struct st_imu_runtime
{
    Quaternion_t     quat;
    icm42688Float3_t gyro_bias;
    icm42688Float3_t mahony_integral;
    uint32_t         last_sample_time_us;
    volatile bool    data_ready;
} imu_runtime_t;

/* 读取一颗 IMU 的函数入口抽象。
 * 通过传入不同函数，可以让同一套上层逻辑同时兼容 g_spi0 和 g_spi1/SCI_SPI。
 */
typedef void (*imu_read_sample_fn_t)(icm42688Float3_t *acc_g, icm42688Float3_t *gyro_rad_s);

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static float    get_envelope(float sample);
static float    vector_norm(icm42688Float3_t const * p_vector);
static void     quaternion_normalize(Quaternion_t * p_quat);
static void     imu_timebase_init(void);
static uint32_t imu_time_now_us(void);
static float    imu_calc_dt_sec(imu_runtime_t * p_imu, uint32_t sample_time_us);
static void     mahony_reset(imu_runtime_t * p_imu);
static void     mahony_update(imu_runtime_t * p_imu,
                              icm42688Float3_t const * p_acc_g,
                              icm42688Float3_t const * p_gyro_rad_s,
                              float dt_sec);
static uint32_t imu_collect_gyro_bias(imu_runtime_t * p_imu, imu_read_sample_fn_t read_sample);
static void     imu_read_sample(imu_runtime_t * p_imu,
                                imu_read_sample_fn_t read_sample,
                                icm42688Float3_t * p_acc_g,
                                icm42688Float3_t * p_gyro_rad_s);
static bool     imu_try_read_sample(imu_runtime_t * p_imu,
                                    imu_read_sample_fn_t read_sample,
                                    icm42688Float3_t * p_acc_g,
                                    icm42688Float3_t * p_gyro_rad_s,
                                    uint32_t * p_sample_time_us);
static void     imu_fail_stop(uint32_t step, fsp_err_t err);
static uint16_t crc16_ccitt(uint8_t const *data, uint16_t length);
static void     pack_u16_le(uint8_t *buffer, uint16_t value);
static void     pack_u32_le(uint8_t *buffer, uint32_t value);
static void     pack_f32_le(uint8_t *buffer, float value);
static void     send_telemetry_frame(Quaternion_t const *upper_quat,
                                     Quaternion_t const *lower_quat,
                                     uint32_t timestamp_ms);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
uint16_t g_adc_buffer[1] = {0};
volatile uint16_t g_adc_flag = 0;

/* app_test 中做包络检测时用到的滑动窗口缓存。 */
static float envelope_buffer[ENVELOPE_BUFFER_SIZE];
static int   envelope_index = 0;
static float envelope_sum = 0.0f;

/* 上臂 IMU（肱三头肌外侧）的运行时状态。 */
static imu_runtime_t s_upper_imu =
{
    .quat =
    {
        .q0 = 1.0f,
        .q1 = 0.0f,
        .q2 = 0.0f,
        .q3 = 0.0f,
    },
    .gyro_bias =
    {
        .x = 0.0f,
        .y = 0.0f,
        .z = 0.0f,
    },
    .mahony_integral =
    {
        .x = 0.0f,
        .y = 0.0f,
        .z = 0.0f,
    },
    .last_sample_time_us = 0U,
    .data_ready = false,
};

/* 手腕 IMU 的运行时状态。 */
static imu_runtime_t s_lower_imu =
{
    .quat =
    {
        .q0 = 1.0f,
        .q1 = 0.0f,
        .q2 = 0.0f,
        .q3 = 0.0f,
    },
    .gyro_bias =
    {
        .x = 0.0f,
        .y = 0.0f,
        .z = 0.0f,
    },
    .mahony_integral =
    {
        .x = 0.0f,
        .y = 0.0f,
        .z = 0.0f,
    },
    .last_sample_time_us = 0U,
    .data_ready = false,
};

/* telemetry 帧的序号与软件时间戳。 */
static uint16_t s_frame_sequence = 0U;
static uint32_t s_imu_stream_start_time_us = 0U;
static uint32_t s_imu_time_cycles_per_us = 1U;
static uint32_t s_imu_last_cycle_count = 0U;
static uint64_t s_imu_cycle_accumulator = 0U;
static uint32_t s_last_telemetry_time_us = 0U;
/* 调试辅助变量：
 * - s_imu_fail_step: 记录 imu_test 失败在第几步
 * - s_imu_last_error: 记录对应的 FSP 错误码
 * - s_imu_uart_ready: 串口是否已经成功打开，决定能否打印错误信息
 */
static volatile uint32_t  s_imu_fail_step = 0U;
static volatile fsp_err_t s_imu_last_error = FSP_SUCCESS;
static volatile bool      s_imu_uart_ready = false;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
static float get_envelope(float sample)
{
    /* 先减去窗口中最旧的值，再加上当前值，实现 O(1) 的滑动平均。 */
    envelope_sum -= envelope_buffer[envelope_index];
    envelope_sum += sample;
    envelope_buffer[envelope_index] = sample;

    envelope_index = (envelope_index + 1) % ENVELOPE_BUFFER_SIZE;

    return envelope_sum / ENVELOPE_BUFFER_SIZE;
}

static void imu_timebase_init(void)
{
#if BSP_FEATURE_DWT_CYCCNT
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif

    s_imu_last_cycle_count = 0U;
    s_imu_cycle_accumulator = 0U;

    s_imu_time_cycles_per_us = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK) / 1000000U;
    if (0U == s_imu_time_cycles_per_us)
    {
        s_imu_time_cycles_per_us = 1U;
    }
}

static uint32_t imu_time_now_us(void)
{
#if BSP_FEATURE_DWT_CYCCNT
    uint32_t current_cycle_count = DWT->CYCCNT;
    uint32_t cycle_delta = current_cycle_count - s_imu_last_cycle_count;

    s_imu_last_cycle_count = current_cycle_count;
    s_imu_cycle_accumulator += cycle_delta;

    return (uint32_t) (s_imu_cycle_accumulator / s_imu_time_cycles_per_us);
#else
    return 0U;
#endif
}

static float imu_calc_dt_sec(imu_runtime_t * p_imu, uint32_t sample_time_us)
{
    float dt_sec = IMU_SAMPLE_DT_DEFAULT_SEC;

    if (0U != p_imu->last_sample_time_us)
    {
        uint32_t delta_us = sample_time_us - p_imu->last_sample_time_us;

        dt_sec = (float) delta_us / 1000000.0f;
        if (dt_sec < IMU_SAMPLE_DT_MIN_SEC)
        {
            dt_sec = IMU_SAMPLE_DT_MIN_SEC;
        }
        else if (dt_sec > IMU_SAMPLE_DT_MAX_SEC)
        {
            dt_sec = IMU_SAMPLE_DT_DEFAULT_SEC;
        }
    }

    p_imu->last_sample_time_us = sample_time_us;

    return dt_sec;
}

void app_test(void)
{
    /* 这是另一个测试入口，用于 ADC 采样和包络检测。 */
    adcdrvinit();

    while (1)
    {
        float filtered_value;
        float envelope_value;

        ADCDrvRead(g_adc_buffer, 1);
        /* 先进行数字滤波，再计算包络。 */
        filtered_value = Filter((float) g_adc_buffer[0]);
        envelope_value = get_envelope(fabsf(filtered_value));
        printf("Filtered: %.2f, %.2f\r\n", filtered_value, envelope_value);
    }
}

void icu8_callback(external_irq_callback_args_t *p_args)
{
    /* IRQ8 对应上臂 IMU 的 Data Ready。 */
    if ((NULL != p_args) && (8 == p_args->channel))
    {
        s_upper_imu.data_ready = true;
    }
}

void icu9_callback(external_irq_callback_args_t *p_args)
{
    /* IRQ9 对应手腕 IMU 的 Data Ready。 */
    if ((NULL != p_args) && (9 == p_args->channel))
    {
        s_lower_imu.data_ready = true;
    }
}

void botton6_callback(external_irq_callback_args_t *p_args)
{
    /* 当前按钮中断未使用，先显式忽略参数。 */
    FSP_PARAMETER_NOT_USED(p_args);
}

void imu_test(void)
{
    fsp_err_t err;
    uint32_t  upper_calibration_samples;
    uint32_t  lower_calibration_samples;

    /* 1. 打开 UART7，用于将双 IMU 姿态结果发送到上位机。 */
    err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(1U, err);
    }
    s_imu_uart_ready = true;
    imu_timebase_init();

    /* 2. 初始化上臂 IMU（硬件 SPI0）。 */
    err = bsp_Icm42688Init();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(2U, err);
    }

    /* 3. 初始化手腕 IMU（SCI_SPI / g_spi1）。 */
    err = bsp_Icm42688SciInit();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(3U, err);
    }

    /* 4. 打开并使能上臂 IMU 的外部中断。 */
    err = R_ICU_ExternalIrqOpen(g_external_irq8.p_ctrl, g_external_irq8.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(4U, err);
    }

    err = R_ICU_ExternalIrqEnable(g_external_irq8.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(5U, err);
    }

    /* 5. 打开并使能手腕 IMU 的外部中断。 */
    err = R_ICU_ExternalIrqOpen(g_external_irq9.p_ctrl, g_external_irq9.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(6U, err);
    }

    err = R_ICU_ExternalIrqEnable(g_external_irq9.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(7U, err);
    }

    /* 6. 清空上电状态，保证本次运行不受上次结果影响。 */
    s_upper_imu.data_ready = false;
    s_lower_imu.data_ready = false;
    s_upper_imu.gyro_bias.x = 0.0f;
    s_upper_imu.gyro_bias.y = 0.0f;
    s_upper_imu.gyro_bias.z = 0.0f;
    s_lower_imu.gyro_bias.x = 0.0f;
    s_lower_imu.gyro_bias.y = 0.0f;
    s_lower_imu.gyro_bias.z = 0.0f;
    s_frame_sequence = 0U;
    mahony_reset(&s_upper_imu);
    mahony_reset(&s_lower_imu);

    /* 7. 分别采集两颗 IMU 的静止数据，估计陀螺零偏。 */
    upper_calibration_samples = imu_collect_gyro_bias(&s_upper_imu, bsp_IcmGetScaledData);
    lower_calibration_samples = imu_collect_gyro_bias(&s_lower_imu, bsp_IcmSciGetScaledData);
    (void) upper_calibration_samples;
    (void) lower_calibration_samples;
    s_upper_imu.data_ready = false;
    s_lower_imu.data_ready = false;
    s_imu_stream_start_time_us = imu_time_now_us();
    s_last_telemetry_time_us = 0U;

    /* 8. 主循环：
     *    - 等待/读取上臂 IMU
     *    - 等待/读取手腕 IMU
     *    - 分别扣除各自陀螺零偏
     *    - 分别执行 Mahony 姿态更新
     *    - 将两套四元数打包发给上位机
     */
    while (1)
    {
        icm42688Float3_t upper_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t upper_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        uint32_t         upper_sample_time_us = 0U;
        uint32_t         lower_sample_time_us = 0U;
        uint32_t         frame_sample_time_us = 0U;
        bool             upper_updated = false;
        bool             lower_updated = false;

        /* 分别从两颗 IMU 读取当前一帧加速度和角速度。 */
        upper_updated = imu_try_read_sample(&s_upper_imu,
                                            bsp_IcmGetScaledData,
                                            &upper_acc_g,
                                            &upper_gyro_rad_s,
                                            &upper_sample_time_us);
        lower_updated = imu_try_read_sample(&s_lower_imu,
                                            bsp_IcmSciGetScaledData,
                                            &lower_acc_g,
                                            &lower_gyro_rad_s,
                                            &lower_sample_time_us);

        /* 扣除上电标定得到的静态零偏。 */
        if (upper_updated)
        {
            upper_gyro_rad_s.x -= s_upper_imu.gyro_bias.x;
            upper_gyro_rad_s.y -= s_upper_imu.gyro_bias.y;
            upper_gyro_rad_s.z -= s_upper_imu.gyro_bias.z;
            mahony_update(&s_upper_imu,
                          &upper_acc_g,
                          &upper_gyro_rad_s,
                          imu_calc_dt_sec(&s_upper_imu, upper_sample_time_us));
            frame_sample_time_us = upper_sample_time_us;
        }

        if (lower_updated)
        {
            lower_gyro_rad_s.x -= s_lower_imu.gyro_bias.x;
            lower_gyro_rad_s.y -= s_lower_imu.gyro_bias.y;
            lower_gyro_rad_s.z -= s_lower_imu.gyro_bias.z;
            mahony_update(&s_lower_imu,
                          &lower_acc_g,
                          &lower_gyro_rad_s,
                          imu_calc_dt_sec(&s_lower_imu, lower_sample_time_us));
            if ((!upper_updated) || (lower_sample_time_us > frame_sample_time_us))
            {
                frame_sample_time_us = lower_sample_time_us;
            }
        }

        /* 两颗 IMU 各自独立做姿态解算。 */

        /* 按既有协议发送 upper/lower 两套四元数。 */
        if ((upper_updated || lower_updated) &&
            ((0U == s_last_telemetry_time_us) ||
             ((frame_sample_time_us - s_last_telemetry_time_us) >= TELEMETRY_MIN_INTERVAL_US)))
        {
            send_telemetry_frame(&s_upper_imu.quat,
                                 &s_lower_imu.quat,
                                 (frame_sample_time_us - s_imu_stream_start_time_us) / 1000U);
            s_last_telemetry_time_us = frame_sample_time_us;
        }
        else
        {
            R_BSP_SoftwareDelay(IMU_IDLE_POLL_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
        }
    }
}

static float vector_norm(icm42688Float3_t const * p_vector)
{
    /* 计算三维向量的欧氏范数。 */
    return sqrtf((p_vector->x * p_vector->x) + (p_vector->y * p_vector->y) + (p_vector->z * p_vector->z));
}

static void quaternion_normalize(Quaternion_t * p_quat)
{
    /* 四元数积分后会因为浮点误差偏离单位模，因此每次更新后都要归一化。 */
    float norm = sqrtf((p_quat->q0 * p_quat->q0) + (p_quat->q1 * p_quat->q1) +
                       (p_quat->q2 * p_quat->q2) + (p_quat->q3 * p_quat->q3));

    if (norm <= 0.0f)
    {
        /* 极端情况下退回单位四元数，避免出现 NaN 或无效姿态。 */
        p_quat->q0 = 1.0f;
        p_quat->q1 = 0.0f;
        p_quat->q2 = 0.0f;
        p_quat->q3 = 0.0f;
        return;
    }

    p_quat->q0 /= norm;
    p_quat->q1 /= norm;
    p_quat->q2 /= norm;
    p_quat->q3 /= norm;
}

static void mahony_reset(imu_runtime_t * p_imu)
{
    /* 将姿态重置为“无旋转”，并清空积分项。 */
    p_imu->quat.q0 = 1.0f;
    p_imu->quat.q1 = 0.0f;
    p_imu->quat.q2 = 0.0f;
    p_imu->quat.q3 = 0.0f;

    p_imu->mahony_integral.x = 0.0f;
    p_imu->mahony_integral.y = 0.0f;
    p_imu->mahony_integral.z = 0.0f;
    p_imu->last_sample_time_us = 0U;
}

static void mahony_update(imu_runtime_t * p_imu,
                          icm42688Float3_t const * p_acc_g,
                          icm42688Float3_t const * p_gyro_rad_s,
                          float dt_sec)
{
    /* 先以陀螺积分值为基础，再根据加速度给出姿态校正。 */
    float acc_norm;
    float gx = p_gyro_rad_s->x;
    float gy = p_gyro_rad_s->y;
    float gz = p_gyro_rad_s->z;

    /* 只有在加速度模长接近 1g 时，才认为当前重力方向可靠，可用于校正姿态。 */
    acc_norm = vector_norm(p_acc_g);
    if ((acc_norm >= IMU_CORRECTION_ACC_MIN_G) && (acc_norm <= IMU_CORRECTION_ACC_MAX_G))
    {
        /* 将测得的加速度方向归一化，视为当前“重力方向测量值”。 */
        float ax = p_acc_g->x / acc_norm;
        float ay = p_acc_g->y / acc_norm;
        float az = p_acc_g->z / acc_norm;
        /* 由当前四元数估计出机体系下的重力方向。 */
        float vx = 2.0f * ((p_imu->quat.q1 * p_imu->quat.q3) - (p_imu->quat.q0 * p_imu->quat.q2));
        float vy = 2.0f * ((p_imu->quat.q0 * p_imu->quat.q1) + (p_imu->quat.q2 * p_imu->quat.q3));
        float vz = (p_imu->quat.q0 * p_imu->quat.q0) - (p_imu->quat.q1 * p_imu->quat.q1) -
                   (p_imu->quat.q2 * p_imu->quat.q2) + (p_imu->quat.q3 * p_imu->quat.q3);
        /* 两个方向向量叉乘得到姿态误差。 */
        float ex = (ay * vz) - (az * vy);
        float ey = (az * vx) - (ax * vz);
        float ez = (ax * vy) - (ay * vx);

        /* 积分项可逐步吸收慢速零偏。 */
        p_imu->mahony_integral.x += MAHONY_KI * ex * dt_sec;
        p_imu->mahony_integral.y += MAHONY_KI * ey * dt_sec;
        p_imu->mahony_integral.z += MAHONY_KI * ez * dt_sec;

        /* 用比例项 + 积分项修正陀螺角速度。 */
        gx += (MAHONY_KP * ex) + p_imu->mahony_integral.x;
        gy += (MAHONY_KP * ey) + p_imu->mahony_integral.y;
        gz += (MAHONY_KP * ez) + p_imu->mahony_integral.z;
    }

    {
        /* 使用修正后的角速度积分四元数微分方程。 */
        float q0 = p_imu->quat.q0;
        float q1 = p_imu->quat.q1;
        float q2 = p_imu->quat.q2;
        float q3 = p_imu->quat.q3;
        float half_dt = 0.5f * dt_sec;

        p_imu->quat.q0 += (-q1 * gx - q2 * gy - q3 * gz) * half_dt;
        p_imu->quat.q1 += (q0 * gx + q2 * gz - q3 * gy) * half_dt;
        p_imu->quat.q2 += (q0 * gy - q1 * gz + q3 * gx) * half_dt;
        p_imu->quat.q3 += (q0 * gz + q1 * gy - q2 * gx) * half_dt;
    }

    quaternion_normalize(&p_imu->quat);
}

static uint32_t imu_collect_gyro_bias(imu_runtime_t * p_imu, imu_read_sample_fn_t read_sample)
{
    /* 标定思想：
     * 在静止阶段多次采样，当加速度模长接近 1g 时，认为样本可信，
     * 对陀螺角速度取平均，作为该 IMU 的上电静态零偏。
     */
    uint32_t         attempts = 0U;
    uint32_t         valid_samples = 0U;
    icm42688Float3_t gyro_sum = {0.0f, 0.0f, 0.0f};

    while ((attempts < IMU_CALIBRATION_MAX_ATTEMPTS) && (valid_samples < IMU_CALIBRATION_SAMPLES))
    {
        icm42688Float3_t acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t gyro_rad_s = {0.0f, 0.0f, 0.0f};
        float            acc_norm;

        imu_read_sample(p_imu, read_sample, &acc_g, &gyro_rad_s);
        attempts++;

        /* 只有静止样本才用于估计零偏。 */
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
        /* 如果一帧有效样本都没有，就退化成零偏为 0。 */
        p_imu->gyro_bias.x = 0.0f;
        p_imu->gyro_bias.y = 0.0f;
        p_imu->gyro_bias.z = 0.0f;
        return 0U;
    }

    p_imu->gyro_bias.x = gyro_sum.x / (float) valid_samples;
    p_imu->gyro_bias.y = gyro_sum.y / (float) valid_samples;
    p_imu->gyro_bias.z = gyro_sum.z / (float) valid_samples;

    return valid_samples;
}

static void imu_read_sample(imu_runtime_t * p_imu,
                            imu_read_sample_fn_t read_sample,
                            icm42688Float3_t * p_acc_g,
                            icm42688Float3_t * p_gyro_rad_s)
{
    /* 优先等待 Data Ready 中断，避免盲读重复数据。 */
    uint32_t wait_ms = 0U;

    while ((!p_imu->data_ready) && (wait_ms < IMU_IRQ_WAIT_TIMEOUT_MS))
    {
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        wait_ms++;
    }

    /* 读完一帧后清掉标志，等待下一次外部中断再次置位。 */
    p_imu->data_ready = false;
    read_sample(p_acc_g, p_gyro_rad_s);
}

static bool imu_try_read_sample(imu_runtime_t * p_imu,
                                imu_read_sample_fn_t read_sample,
                                icm42688Float3_t * p_acc_g,
                                icm42688Float3_t * p_gyro_rad_s,
                                uint32_t * p_sample_time_us)
{
    if (!p_imu->data_ready)
    {
        return false;
    }

    p_imu->data_ready = false;
    read_sample(p_acc_g, p_gyro_rad_s);

    if (NULL != p_sample_time_us)
    {
        *p_sample_time_us = imu_time_now_us();
    }

    return true;
}

static void imu_fail_stop(uint32_t step, fsp_err_t err)
{
    /* 把失败现场保存在全局变量里，方便直接在调试器窗口观察。 */
    s_imu_fail_step = step;
    s_imu_last_error = err;

    /* 如果 UART 已经成功打开，则顺手把失败信息打到串口。 */
    if (s_imu_uart_ready)
    {
        printf("imu_test failed: step=%lu err=%d\r\n", (unsigned long) step, (int) err);
    }

    /* 停在这里，不再 return 回 main，避免又掉回 Reset_Handler 的死循环。 */
    while (1)
    {
        R_BSP_SoftwareDelay(100U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

static uint16_t crc16_ccitt(uint8_t const *data, uint16_t length)
{
    /* 对 telemetry 数据帧做 CRC16-CCITT 校验。 */
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
    /* 按小端序打包 16bit 数据。 */
    buffer[0] = (uint8_t) (value & 0xFFU);
    buffer[1] = (uint8_t) ((value >> 8) & 0xFFU);
}

static void pack_u32_le(uint8_t *buffer, uint32_t value)
{
    /* 按小端序打包 32bit 数据。 */
    buffer[0] = (uint8_t) (value & 0xFFU);
    buffer[1] = (uint8_t) ((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t) ((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t) ((value >> 24) & 0xFFU);
}

static void pack_f32_le(uint8_t *buffer, float value)
{
    /* 将 float 的二进制表示原样拷贝，再按小端序写入缓冲区。 */
    uint32_t raw = 0U;

    memcpy(&raw, &value, sizeof(raw));
    pack_u32_le(buffer, raw);
}

static void send_telemetry_frame(Quaternion_t const *upper_quat,
                                 Quaternion_t const *lower_quat,
                                 uint32_t timestamp_ms)
{
    /* 
     * 数据帧格式：
     * [0..1]   帧头 0xA5 0x5A
     * [2..3]   协议版本/类型
     * [4..5]   帧序号
     * [6..9]   时间戳(ms)
     * [10..25] 上臂四元数 q0~q3
     * [26..41] 手腕四元数 q0~q3
     * [42..45] 预留字段
     * [46..47] CRC16
     */
    uint8_t  frame[48] = {0};
    uint16_t crc;

    frame[0] = 0xA5U;
    frame[1] = 0x5AU;
    frame[2] = 0x01U;
    frame[3] = 0x01U;

    pack_u16_le(&frame[4], s_frame_sequence);
    pack_u32_le(&frame[6], timestamp_ms);

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
        /* 阻塞等待发送完成，确保帧不会在下一次写入时被覆盖。 */
        drv_uart_wait_for_tx();
    }

    /* 软件维护序号与时间戳，便于上位机按时序还原数据。 */
    s_frame_sequence = (uint16_t) (s_frame_sequence + 1U);
}

/* 机械臂测试相关的全局节拍计数。 */
volatile uint16_t g_agt_tick_count = 0;

void g_timer_agt1_callback(timer_callback_args_t *p_args)
{
    /* AGT 周期中断里驱动舵机任务调度。 */
    if (TIMER_EVENT_CYCLE_END == p_args->event)
    {
        g_agt_tick_count++;
        Servo_Update_Task();
    }
}

void MG996_test(void)
{
    /* 这是舵机测试入口，与 IMU 功能相互独立。 */
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

    /* 主循环里持续输出节拍，便于观察 AGT 和舵机任务是否正常运行。 */
    while (1)
    {
        printf("AGT Tick: %d\r\n", g_agt_tick_count);
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
