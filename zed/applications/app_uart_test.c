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
#define TELEMETRY_MIN_INTERVAL_US     20000U
#define IMU_BUTTON_DEBOUNCE_US        250000U
#define IMU_LED_BLINK_HALF_PERIOD_US  500000U
#define IMU_LED_FLASH_ON_US           100000U
#define IMU_LED_FLASH_GAP_US          100000U
#define IMU_LED_FLASH_TOTAL_US        ((2U * IMU_LED_FLASH_ON_US) + (2U * IMU_LED_FLASH_GAP_US))
#define IMU_EULER_SINGULARITY_EPSILON 0.9999999f
#define IMU_RAD_TO_DEG                57.295779513082320876f
#define IMU_STATUS_LED_PIN            BSP_IO_PORT_04_PIN_00

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

typedef struct st_imu_servo_pose
{
    uint16_t hY_deg;
    uint16_t hZ_deg;
    uint16_t eZ_deg;
    uint16_t wX_deg;
    uint8_t  grip_percent;
} imu_servo_pose_t;

typedef struct st_imu_calibration_runtime
{
    Quaternion_t upper_offset;
    Quaternion_t lower_offset;
    uint32_t     last_button_time_us;
    uint32_t     led_flash_start_us;
    volatile bool button_pending;
    bool         is_calibrated;
    bool         led_flash_active;
} imu_calibration_runtime_t;

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static float    get_envelope(float sample);
static float    vector_norm(icm42688Float3_t const * p_vector);
static float    clampf(float value, float min_value, float max_value);
static void     quaternion_normalize(Quaternion_t * p_quat);
static void     quaternion_identity(Quaternion_t * p_quat);
static Quaternion_t quaternion_multiply(Quaternion_t const * p_left, Quaternion_t const * p_right);
static Quaternion_t quaternion_inverse(Quaternion_t const * p_quat);
static void     quaternion_to_euler_yxz(Quaternion_t const * p_quat, float * p_x_rad, float * p_y_rad, float * p_z_rad);
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
static int32_t  imu_clamp_int32(int32_t value, int32_t min_value, int32_t max_value);
static void     imu_set_status_led(bool led_on);
static void     imu_update_status_led(uint32_t now_us);
static void     imu_calibration_reset(void);
static void     imu_capture_tpose_calibration(uint32_t now_us);
static void     imu_handle_button_event(uint32_t now_us);
static uint8_t  imu_get_grip_percent(void);
static bool     imu_try_build_servo_pose(imu_servo_pose_t * p_pose);
static void     imu_fail_stop(uint32_t step, fsp_err_t err);
static void     send_pose_frame(imu_servo_pose_t const * p_pose);

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

static uint32_t s_imu_time_cycles_per_us = 1U;
static uint32_t s_imu_last_cycle_count = 0U;
static uint64_t s_imu_cycle_accumulator = 0U;
static uint32_t s_last_telemetry_time_us = 0U;
static imu_calibration_runtime_t s_imu_calibration = {0};
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
    if ((NULL != p_args) && (6 == p_args->channel))
    {
        s_imu_calibration.button_pending = true;
    }
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
    mahony_reset(&s_upper_imu);
    mahony_reset(&s_lower_imu);
    imu_calibration_reset();
    imu_update_status_led(imu_time_now_us());

    /* 7. 分别采集两颗 IMU 的静止数据，估计陀螺零偏。 */
    upper_calibration_samples = imu_collect_gyro_bias(&s_upper_imu, bsp_IcmGetScaledData);
    lower_calibration_samples = imu_collect_gyro_bias(&s_lower_imu, bsp_IcmSciGetScaledData);
    (void) upper_calibration_samples;
    (void) lower_calibration_samples;
    s_upper_imu.data_ready = false;
    s_lower_imu.data_ready = false;
    s_last_telemetry_time_us = 0U;

    /* 8. 打开并使能用户按键中断，用于执行 T-Pose 一键标定。 */
    err = R_ICU_ExternalIrqOpen(g_external_irq6.p_ctrl, g_external_irq6.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(8U, err);
    }

    err = R_ICU_ExternalIrqEnable(g_external_irq6.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(9U, err);
    }

    s_imu_calibration.button_pending = false;

    /* 9. 主循环：
     *    - 等待/读取上臂 IMU
     *    - 等待/读取手腕 IMU
     *    - 分别扣除各自陀螺零偏
     *    - 分别执行 Mahony 姿态更新
     *    - 响应按键采样并执行 T-Pose 固连标定
     *    - 校准完成后发送 POSE 文本协议
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
        uint32_t         loop_time_us;
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

        loop_time_us = (upper_updated || lower_updated) ? frame_sample_time_us : imu_time_now_us();

        imu_handle_button_event(loop_time_us);
        imu_update_status_led(loop_time_us);

        /* 校准完成后，按网页协议发送姿态文本。 */
        if ((upper_updated || lower_updated) &&
            s_imu_calibration.is_calibrated &&
            ((0U == s_last_telemetry_time_us) ||
             ((loop_time_us - s_last_telemetry_time_us) >= TELEMETRY_MIN_INTERVAL_US)))
        {
            imu_servo_pose_t pose = {0U, 0U, 0U, 0U, 0U};

            if (imu_try_build_servo_pose(&pose))
            {
                send_pose_frame(&pose);
                s_last_telemetry_time_us = loop_time_us;
            }
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

static float clampf(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
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

static void quaternion_identity(Quaternion_t * p_quat)
{
    if (NULL == p_quat)
    {
        return;
    }

    p_quat->q0 = 1.0f;
    p_quat->q1 = 0.0f;
    p_quat->q2 = 0.0f;
    p_quat->q3 = 0.0f;
}

static Quaternion_t quaternion_multiply(Quaternion_t const * p_left, Quaternion_t const * p_right)
{
    Quaternion_t result = {0.0f, 0.0f, 0.0f, 0.0f};

    result.q0 = (p_left->q0 * p_right->q0) - (p_left->q1 * p_right->q1) -
                (p_left->q2 * p_right->q2) - (p_left->q3 * p_right->q3);
    result.q1 = (p_left->q0 * p_right->q1) + (p_left->q1 * p_right->q0) +
                (p_left->q2 * p_right->q3) - (p_left->q3 * p_right->q2);
    result.q2 = (p_left->q0 * p_right->q2) - (p_left->q1 * p_right->q3) +
                (p_left->q2 * p_right->q0) + (p_left->q3 * p_right->q1);
    result.q3 = (p_left->q0 * p_right->q3) + (p_left->q1 * p_right->q2) -
                (p_left->q2 * p_right->q1) + (p_left->q3 * p_right->q0);
    quaternion_normalize(&result);

    return result;
}

static Quaternion_t quaternion_inverse(Quaternion_t const * p_quat)
{
    Quaternion_t result = {0.0f, 0.0f, 0.0f, 0.0f};

    result.q0 = p_quat->q0;
    result.q1 = -p_quat->q1;
    result.q2 = -p_quat->q2;
    result.q3 = -p_quat->q3;
    quaternion_normalize(&result);

    return result;
}

static void quaternion_to_euler_yxz(Quaternion_t const * p_quat, float * p_x_rad, float * p_y_rad, float * p_z_rad)
{
    float x = p_quat->q1;
    float y = p_quat->q2;
    float z = p_quat->q3;
    float w = p_quat->q0;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;
    float m11 = 1.0f - (2.0f * (yy + zz));
    float m13 = 2.0f * (xz + wy);
    float m21 = 2.0f * (xy + wz);
    float m22 = 1.0f - (2.0f * (xx + zz));
    float m23 = 2.0f * (yz - wx);
    float m31 = 2.0f * (xz - wy);
    float m33 = 1.0f - (2.0f * (xx + yy));
    float x_rad;
    float y_rad;
    float z_rad;

    x_rad = asinf(-clampf(m23, -1.0f, 1.0f));

    if (fabsf(m23) < IMU_EULER_SINGULARITY_EPSILON)
    {
        y_rad = atan2f(m13, m33);
        z_rad = atan2f(m21, m22);
    }
    else
    {
        y_rad = atan2f(-m31, m11);
        z_rad = 0.0f;
    }

    if (NULL != p_x_rad)
    {
        *p_x_rad = x_rad;
    }

    if (NULL != p_y_rad)
    {
        *p_y_rad = y_rad;
    }

    if (NULL != p_z_rad)
    {
        *p_z_rad = z_rad;
    }
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

        imu_update_status_led(imu_time_now_us());

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

static int32_t imu_clamp_int32(int32_t value, int32_t min_value, int32_t max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static void imu_set_status_led(bool led_on)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl,
                             IMU_STATUS_LED_PIN,
                             led_on ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}

static void imu_update_status_led(uint32_t now_us)
{
    bool led_on = false;

    if (s_imu_calibration.led_flash_active)
    {
        uint32_t elapsed_us = now_us - s_imu_calibration.led_flash_start_us;

        if (elapsed_us < IMU_LED_FLASH_TOTAL_US)
        {
            if (elapsed_us < IMU_LED_FLASH_ON_US)
            {
                led_on = true;
            }
            else if (elapsed_us < (IMU_LED_FLASH_ON_US + IMU_LED_FLASH_GAP_US))
            {
                led_on = false;
            }
            else if (elapsed_us < ((2U * IMU_LED_FLASH_ON_US) + IMU_LED_FLASH_GAP_US))
            {
                led_on = true;
            }
            else
            {
                led_on = false;
            }

            imu_set_status_led(led_on);
            return;
        }

        s_imu_calibration.led_flash_active = false;
    }

    if (s_imu_calibration.is_calibrated)
    {
        led_on = true;
    }
    else
    {
        led_on = (((now_us / IMU_LED_BLINK_HALF_PERIOD_US) % 2U) == 0U);
    }

    imu_set_status_led(led_on);
}

static void imu_calibration_reset(void)
{
    quaternion_identity(&s_imu_calibration.upper_offset);
    quaternion_identity(&s_imu_calibration.lower_offset);
    s_imu_calibration.button_pending = false;
    s_imu_calibration.is_calibrated = false;
    s_imu_calibration.led_flash_active = false;
    s_imu_calibration.led_flash_start_us = 0U;
}

static void imu_capture_tpose_calibration(uint32_t now_us)
{
    s_imu_calibration.upper_offset = quaternion_inverse(&s_upper_imu.quat);
    s_imu_calibration.lower_offset = quaternion_inverse(&s_lower_imu.quat);
    s_imu_calibration.is_calibrated = true;
    s_imu_calibration.led_flash_active = true;
    s_imu_calibration.led_flash_start_us = now_us;
    s_last_telemetry_time_us = 0U;
}

static void imu_handle_button_event(uint32_t now_us)
{
    if (!s_imu_calibration.button_pending)
    {
        return;
    }

    s_imu_calibration.button_pending = false;

    if ((now_us - s_imu_calibration.last_button_time_us) < IMU_BUTTON_DEBOUNCE_US)
    {
        return;
    }

    s_imu_calibration.last_button_time_us = now_us;

    if ((0U == s_upper_imu.last_sample_time_us) || (0U == s_lower_imu.last_sample_time_us))
    {
        return;
    }

    /* 单键动作改为“T-Pose 一键重标定”：每次短按都以当前姿态重新建立固连偏差。 */
    imu_capture_tpose_calibration(now_us);
}

static uint8_t imu_get_grip_percent(void)
{
    return 0U;
}

static bool imu_try_build_servo_pose(imu_servo_pose_t * p_pose)
{
    Quaternion_t upper_bone;
    Quaternion_t lower_bone;
    Quaternion_t upper_bone_inverse;
    Quaternion_t relative_bone;
    float        upper_y_rad = 0.0f;
    float        upper_z_rad = 0.0f;
    float        relative_x_rad = 0.0f;
    float        relative_z_rad = 0.0f;
    int32_t      hY_deg;
    int32_t      hZ_deg;
    int32_t      eZ_deg;
    int32_t      wX_deg;

    if ((NULL == p_pose) || !s_imu_calibration.is_calibrated)
    {
        return false;
    }

    upper_bone = quaternion_multiply(&s_upper_imu.quat, &s_imu_calibration.upper_offset);
    lower_bone = quaternion_multiply(&s_lower_imu.quat, &s_imu_calibration.lower_offset);
    upper_bone_inverse = quaternion_inverse(&upper_bone);
    relative_bone = quaternion_multiply(&upper_bone_inverse, &lower_bone);

    quaternion_to_euler_yxz(&upper_bone, NULL, &upper_y_rad, &upper_z_rad);
    quaternion_to_euler_yxz(&relative_bone, &relative_x_rad, NULL, &relative_z_rad);

    /* 协议仍使用 0~180 的人体角表示，因此对带符号角做 +90 平移。 */
    hY_deg = (int32_t) roundf((upper_y_rad * IMU_RAD_TO_DEG) + 90.0f);
    hZ_deg = (int32_t) roundf((upper_z_rad * IMU_RAD_TO_DEG) + 90.0f);
    /* 协议定义 eZ 为肘部绕 Z 轴角度，因此这里取相对姿态的 Z 分量。 */
    eZ_deg = (int32_t) roundf(relative_z_rad * IMU_RAD_TO_DEG);
    wX_deg = (int32_t) roundf((relative_x_rad * IMU_RAD_TO_DEG) + 90.0f);

    p_pose->hY_deg = (uint16_t) imu_clamp_int32(hY_deg, 0, 180);
    p_pose->hZ_deg = (uint16_t) imu_clamp_int32(hZ_deg, 0, 180);
    p_pose->eZ_deg = (uint16_t) imu_clamp_int32(eZ_deg, 0, 180);
    p_pose->wX_deg = (uint16_t) imu_clamp_int32(wX_deg, 0, 180);
    p_pose->grip_percent = (uint8_t) imu_clamp_int32((int32_t) imu_get_grip_percent(), 0, 100);

    return true;
}

static void send_pose_frame(imu_servo_pose_t const * p_pose)
{
    char frame[32] = {0};
    int  frame_len;

    if (NULL == p_pose)
    {
        return;
    }

    frame_len = snprintf(frame,
                         sizeof(frame),
                         "POSE,%u,%u,%u,%u,%u\r\n",
                         (unsigned int) p_pose->hY_deg,
                         (unsigned int) p_pose->hZ_deg,
                         (unsigned int) p_pose->eZ_deg,
                         (unsigned int) p_pose->wX_deg,
                         (unsigned int) p_pose->grip_percent);

    if ((frame_len > 0) && ((size_t) frame_len < sizeof(frame)))
    {
        if (FSP_SUCCESS == g_uart7.p_api->write(g_uart7.p_ctrl, (uint8_t const *) frame, (uint32_t) frame_len))
        {
            drv_uart_wait_for_tx();
        }
    }
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
