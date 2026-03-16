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
#include <stdarg.h>
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
#define IMU_BUTTON_LONG_PRESS_US      2000000U
#define IMU_LED_BLINK_HALF_PERIOD_US  500000U
#define IMU_LED_FLASH_ON_US           100000U
#define IMU_LED_FLASH_GAP_US          100000U
#define IMU_LED_FLASH_TOTAL_US        ((2U * IMU_LED_FLASH_ON_US) + (2U * IMU_LED_FLASH_GAP_US))
#define IMU_LED_WAIT_PAUSE_US         500000U
#define IMU_EULER_SINGULARITY_EPSILON 0.9999999f
#define IMU_RAD_TO_DEG                57.295779513082320876f
#define IMU_STATUS_LED_PIN            BSP_IO_PORT_04_PIN_00
#define IMU_BUTTON_PIN                BSP_IO_PORT_00_PIN_00
#define IMU_AXIS_MIN_RESPONSE_DEG     20.0f
#define IMU_AXIS_DOMINANCE_RATIO      1.5f
#define IMU_CAL_TARGET_DELTA_DEG      90.0f
#define IMU_UART_LINE_MAX_LEN         64U

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

typedef enum e_imu_signal_source
{
    IMU_SIGNAL_SOURCE_UPPER = 0,
    IMU_SIGNAL_SOURCE_RELATIVE = 1,
} imu_signal_source_t;

typedef enum e_imu_component_index
{
    IMU_COMPONENT_X = 0,
    IMU_COMPONENT_Y = 1,
    IMU_COMPONENT_Z = 2,
} imu_component_index_t;

typedef enum e_imu_cal_step
{
    IMU_CAL_STEP_TPOSE = 0,
    IMU_CAL_STEP_HY = 1,
    IMU_CAL_STEP_HZ = 2,
    IMU_CAL_STEP_EZ = 3,
    IMU_CAL_STEP_WX = 4,
    IMU_CAL_STEP_COUNT = 5,
    IMU_CAL_STEP_DONE = 6,
} imu_cal_step_t;

typedef enum e_imu_cal_result
{
    IMU_CAL_RESULT_OK = 0,
    IMU_CAL_RESULT_WEAK = 1,
    IMU_CAL_RESULT_AMBIG = 2,
    IMU_CAL_RESULT_NODATA = 3,
} imu_cal_result_t;

typedef struct st_imu_motion_components
{
    float upper_rad[3];
    float relative_rad[3];
} imu_motion_components_t;

typedef struct st_imu_axis_map
{
    imu_signal_source_t source;
    uint8_t             component;
    float               gain;
    int16_t             center_deg;
    bool                valid;
} imu_axis_map_t;

typedef struct st_imu_calibration_runtime
{
    Quaternion_t upper_offset;
    Quaternion_t lower_offset;
    imu_axis_map_t hY_map;
    imu_axis_map_t hZ_map;
    imu_axis_map_t eZ_map;
    imu_axis_map_t wX_map;
    uint32_t     last_button_time_us;
    uint32_t     button_press_start_us;
    uint32_t     led_flash_start_us;
    volatile bool button_pending;
    bool         button_press_active;
    bool         button_long_handled;
    bool         is_calibrated;
    bool         led_flash_active;
    uint8_t      led_flash_pulses;
    imu_cal_step_t current_step;
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
static bool     imu_is_button_pressed(void);
static void     imu_ascii_to_upper(char * p_text);
static void     imu_send_text(char const * p_text);
static void     imu_send_textf(char const * p_format, ...);
static void     imu_send_cal_step(void);
static void     imu_send_cal_ok(imu_cal_step_t step);
static void     imu_send_cal_error(imu_cal_result_t result, imu_cal_step_t step);
static void     imu_send_cal_done(void);
static void     imu_send_cal_state(void);
static char const * imu_cal_step_name(imu_cal_step_t step);
static char const * imu_cal_result_name(imu_cal_result_t result);
static void     imu_set_flash_pattern(uint8_t pulses, uint32_t now_us);
static void     imu_calibration_reset(void);
static void     imu_calibration_begin(uint32_t now_us);
static bool     imu_capture_motion_components(imu_motion_components_t * p_motion);
static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           float const raw_deg[3],
                                           uint8_t excluded_mask,
                                           imu_signal_source_t source,
                                           int16_t center_deg,
                                           float target_delta_deg);
static imu_cal_result_t imu_record_current_step(uint32_t now_us);
static void     imu_handle_calibration_next(uint32_t now_us);
static void     imu_handle_button_event(uint32_t now_us);
static void     imu_handle_uart_commands(uint32_t now_us);
static void     imu_handle_uart_command(char * p_line, uint32_t now_us);
static int32_t  imu_apply_axis_map(imu_axis_map_t const * p_map, imu_motion_components_t const * p_motion);
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
    err = drv_uart_start_rx();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(10U, err);
    }
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
    imu_calibration_begin(imu_time_now_us());
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
        imu_handle_uart_commands(loop_time_us);
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

static bool imu_is_button_pressed(void)
{
    bsp_io_level_t pin_level = BSP_IO_LEVEL_HIGH;

    if (FSP_SUCCESS != R_IOPORT_PinRead(&g_ioport_ctrl, IMU_BUTTON_PIN, &pin_level))
    {
        return false;
    }

    return (BSP_IO_LEVEL_LOW == pin_level);
}

static void imu_ascii_to_upper(char * p_text)
{
    if (NULL == p_text)
    {
        return;
    }

    while ('\0' != *p_text)
    {
        if ((*p_text >= 'a') && (*p_text <= 'z'))
        {
            *p_text = (char) (*p_text - ('a' - 'A'));
        }

        p_text++;
    }
}

static void imu_send_text(char const * p_text)
{
    size_t text_len;

    if ((NULL == p_text) || !s_imu_uart_ready)
    {
        return;
    }

    text_len = strlen(p_text);
    if (0U == text_len)
    {
        return;
    }

    if (FSP_SUCCESS == g_uart7.p_api->write(g_uart7.p_ctrl, (uint8_t const *) p_text, (uint32_t) text_len))
    {
        drv_uart_wait_for_tx();
    }
}

static void imu_send_textf(char const * p_format, ...)
{
    char    frame[96] = {0};
    va_list args;
    int     frame_len;

    if (NULL == p_format)
    {
        return;
    }

    va_start(args, p_format);
    frame_len = vsnprintf(frame, sizeof(frame), p_format, args);
    va_end(args);

    if ((frame_len > 0) && ((size_t) frame_len < sizeof(frame)))
    {
        imu_send_text(frame);
    }
}

static char const * imu_cal_step_name(imu_cal_step_t step)
{
    switch (step)
    {
        case IMU_CAL_STEP_TPOSE:
            return "TPOSE";
        case IMU_CAL_STEP_HY:
            return "HY+";
        case IMU_CAL_STEP_HZ:
            return "HZ-";
        case IMU_CAL_STEP_EZ:
            return "EZ+";
        case IMU_CAL_STEP_WX:
            return "WX+";
        case IMU_CAL_STEP_DONE:
            return "DONE";
        default:
            return "IDLE";
    }
}

static char const * imu_cal_result_name(imu_cal_result_t result)
{
    switch (result)
    {
        case IMU_CAL_RESULT_OK:
            return "OK";
        case IMU_CAL_RESULT_WEAK:
            return "WEAK";
        case IMU_CAL_RESULT_AMBIG:
            return "AMBIG";
        case IMU_CAL_RESULT_NODATA:
            return "NODATA";
        default:
            return "ERR";
    }
}

static void imu_send_cal_step(void)
{
    imu_send_textf("CAL,STEP,%u,%s\r\n",
                   (unsigned int) (s_imu_calibration.current_step + 1U),
                   imu_cal_step_name(s_imu_calibration.current_step));
}

static void imu_send_cal_ok(imu_cal_step_t step)
{
    imu_send_textf("CAL,OK,%u,%s\r\n",
                   (unsigned int) (step + 1U),
                   imu_cal_step_name(step));
}

static void imu_send_cal_error(imu_cal_result_t result, imu_cal_step_t step)
{
    imu_send_textf("CAL,ERR,%s,%s\r\n",
                   imu_cal_result_name(result),
                   imu_cal_step_name(step));
}

static void imu_send_cal_done(void)
{
    imu_send_text("CAL,DONE\r\n");
}

static void imu_send_cal_state(void)
{
    if (s_imu_calibration.is_calibrated)
    {
        imu_send_text("CAL,STATE,5,DONE,1\r\n");
    }
    else
    {
        imu_send_textf("CAL,STATE,%u,%s,0\r\n",
                       (unsigned int) (s_imu_calibration.current_step + 1U),
                       imu_cal_step_name(s_imu_calibration.current_step));
    }
}

static void imu_set_flash_pattern(uint8_t pulses, uint32_t now_us)
{
    s_imu_calibration.led_flash_active = (pulses > 0U);
    s_imu_calibration.led_flash_pulses = pulses;
    s_imu_calibration.led_flash_start_us = now_us;
}

static void imu_update_status_led(uint32_t now_us)
{
    bool led_on = false;

    if (s_imu_calibration.led_flash_active)
    {
        uint32_t elapsed_us = now_us - s_imu_calibration.led_flash_start_us;
        uint32_t pulse_period_us = IMU_LED_FLASH_ON_US + IMU_LED_FLASH_GAP_US;
        uint32_t pattern_window_us = (uint32_t) s_imu_calibration.led_flash_pulses * pulse_period_us;

        if (elapsed_us < pattern_window_us)
        {
            led_on = ((elapsed_us % pulse_period_us) < IMU_LED_FLASH_ON_US);
            imu_set_status_led(led_on);
            return;
        }

        s_imu_calibration.led_flash_active = false;
    }

    if (s_imu_calibration.is_calibrated)
    {
        imu_set_status_led(true);
        return;
    }

    {
        uint32_t wait_pulses = (uint32_t) (s_imu_calibration.current_step + 1U);
        uint32_t pulse_period_us = IMU_LED_FLASH_ON_US + IMU_LED_FLASH_GAP_US;
        uint32_t pulse_window_us = wait_pulses * pulse_period_us;
        uint32_t cycle_window_us = pulse_window_us + IMU_LED_WAIT_PAUSE_US;
        uint32_t cycle_offset_us = now_us % cycle_window_us;

        if (cycle_offset_us < pulse_window_us)
        {
            led_on = ((cycle_offset_us % pulse_period_us) < IMU_LED_FLASH_ON_US);
        }
    }

    imu_set_status_led(led_on);
}

static void imu_calibration_reset(void)
{
    memset(&s_imu_calibration, 0, sizeof(s_imu_calibration));
    quaternion_identity(&s_imu_calibration.upper_offset);
    quaternion_identity(&s_imu_calibration.lower_offset);
    s_imu_calibration.current_step = IMU_CAL_STEP_TPOSE;
    s_last_telemetry_time_us = 0U;
}

static void imu_calibration_begin(uint32_t now_us)
{
    imu_calibration_reset();
    s_imu_calibration.last_button_time_us = now_us - IMU_BUTTON_DEBOUNCE_US;
    imu_set_flash_pattern(1U, now_us);
    imu_send_cal_step();
}

static bool imu_capture_motion_components(imu_motion_components_t * p_motion)
{
    Quaternion_t upper_bone;
    Quaternion_t lower_bone;
    Quaternion_t upper_bone_inverse;
    Quaternion_t relative_bone;

    if ((NULL == p_motion) ||
        (0U == s_upper_imu.last_sample_time_us) ||
        (0U == s_lower_imu.last_sample_time_us))
    {
        return false;
    }

    upper_bone = quaternion_multiply(&s_upper_imu.quat, &s_imu_calibration.upper_offset);
    lower_bone = quaternion_multiply(&s_lower_imu.quat, &s_imu_calibration.lower_offset);
    upper_bone_inverse = quaternion_inverse(&upper_bone);
    relative_bone = quaternion_multiply(&upper_bone_inverse, &lower_bone);

    quaternion_to_euler_yxz(&upper_bone,
                            &p_motion->upper_rad[IMU_COMPONENT_X],
                            &p_motion->upper_rad[IMU_COMPONENT_Y],
                            &p_motion->upper_rad[IMU_COMPONENT_Z]);
    quaternion_to_euler_yxz(&relative_bone,
                            &p_motion->relative_rad[IMU_COMPONENT_X],
                            &p_motion->relative_rad[IMU_COMPONENT_Y],
                            &p_motion->relative_rad[IMU_COMPONENT_Z]);

    return true;
}

static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           float const raw_deg[3],
                                           uint8_t excluded_mask,
                                           imu_signal_source_t source,
                                           int16_t center_deg,
                                           float target_delta_deg)
{
    int   best_index = -1;
    float best_abs_deg = 0.0f;
    float second_abs_deg = 0.0f;

    if ((NULL == p_map) || (NULL == raw_deg))
    {
        return IMU_CAL_RESULT_AMBIG;
    }

    for (int i = 0; i < 3; i++)
    {
        float abs_deg;

        if (0U != (excluded_mask & (1U << i)))
        {
            continue;
        }

        abs_deg = fabsf(raw_deg[i]);
        if (abs_deg > best_abs_deg)
        {
            second_abs_deg = best_abs_deg;
            best_abs_deg = abs_deg;
            best_index = i;
        }
        else if (abs_deg > second_abs_deg)
        {
            second_abs_deg = abs_deg;
        }
    }

    if (best_index < 0)
    {
        return IMU_CAL_RESULT_AMBIG;
    }

    if (best_abs_deg < IMU_AXIS_MIN_RESPONSE_DEG)
    {
        return IMU_CAL_RESULT_WEAK;
    }

    if ((second_abs_deg > 0.0f) && (best_abs_deg < (second_abs_deg * IMU_AXIS_DOMINANCE_RATIO)))
    {
        return IMU_CAL_RESULT_AMBIG;
    }

    p_map->source = source;
    p_map->component = (uint8_t) best_index;
    p_map->gain = target_delta_deg / raw_deg[best_index];
    p_map->center_deg = center_deg;
    p_map->valid = true;

    return IMU_CAL_RESULT_OK;
}

static imu_cal_result_t imu_record_current_step(uint32_t now_us)
{
    imu_motion_components_t motion = {0};
    float upper_deg[3] = {0.0f, 0.0f, 0.0f};
    float relative_deg[3] = {0.0f, 0.0f, 0.0f};

    (void) now_us;

    if ((0U == s_upper_imu.last_sample_time_us) || (0U == s_lower_imu.last_sample_time_us))
    {
        return IMU_CAL_RESULT_NODATA;
    }

    if (IMU_CAL_STEP_TPOSE == s_imu_calibration.current_step)
    {
        s_imu_calibration.upper_offset = quaternion_inverse(&s_upper_imu.quat);
        s_imu_calibration.lower_offset = quaternion_inverse(&s_lower_imu.quat);
        s_imu_calibration.hY_map.valid = false;
        s_imu_calibration.hZ_map.valid = false;
        s_imu_calibration.eZ_map.valid = false;
        s_imu_calibration.wX_map.valid = false;
        s_imu_calibration.is_calibrated = false;
        s_last_telemetry_time_us = 0U;
        return IMU_CAL_RESULT_OK;
    }

    if (!imu_capture_motion_components(&motion))
    {
        return IMU_CAL_RESULT_NODATA;
    }

    for (int i = 0; i < 3; i++)
    {
        upper_deg[i] = motion.upper_rad[i] * IMU_RAD_TO_DEG;
        relative_deg[i] = motion.relative_rad[i] * IMU_RAD_TO_DEG;
    }

    switch (s_imu_calibration.current_step)
    {
        case IMU_CAL_STEP_HY:
            return imu_learn_axis_map(&s_imu_calibration.hY_map,
                                      upper_deg,
                                      0U,
                                      IMU_SIGNAL_SOURCE_UPPER,
                                      90,
                                      IMU_CAL_TARGET_DELTA_DEG);

        case IMU_CAL_STEP_HZ:
            return imu_learn_axis_map(&s_imu_calibration.hZ_map,
                                      upper_deg,
                                      s_imu_calibration.hY_map.valid ? (uint8_t) (1U << s_imu_calibration.hY_map.component) : 0U,
                                      IMU_SIGNAL_SOURCE_UPPER,
                                      90,
                                      -IMU_CAL_TARGET_DELTA_DEG);

        case IMU_CAL_STEP_EZ:
            return imu_learn_axis_map(&s_imu_calibration.eZ_map,
                                      relative_deg,
                                      0U,
                                      IMU_SIGNAL_SOURCE_RELATIVE,
                                      0,
                                      IMU_CAL_TARGET_DELTA_DEG);

        case IMU_CAL_STEP_WX:
            return imu_learn_axis_map(&s_imu_calibration.wX_map,
                                      relative_deg,
                                      s_imu_calibration.eZ_map.valid ? (uint8_t) (1U << s_imu_calibration.eZ_map.component) : 0U,
                                      IMU_SIGNAL_SOURCE_RELATIVE,
                                      90,
                                      IMU_CAL_TARGET_DELTA_DEG);

        default:
            return IMU_CAL_RESULT_AMBIG;
    }
}

static void imu_handle_calibration_next(uint32_t now_us)
{
    imu_cal_result_t result;
    imu_cal_step_t   finished_step;

    if (s_imu_calibration.is_calibrated && (IMU_CAL_STEP_DONE == s_imu_calibration.current_step))
    {
        imu_send_cal_done();
        return;
    }

    result = imu_record_current_step(now_us);
    finished_step = s_imu_calibration.current_step;

    if (IMU_CAL_RESULT_OK != result)
    {
        imu_set_flash_pattern(4U, now_us);
        imu_send_cal_error(result, finished_step);
        return;
    }

    imu_set_flash_pattern(2U, now_us);
    imu_send_cal_ok(finished_step);

    if (IMU_CAL_STEP_WX == finished_step)
    {
        s_imu_calibration.is_calibrated = s_imu_calibration.hY_map.valid &&
                                          s_imu_calibration.hZ_map.valid &&
                                          s_imu_calibration.eZ_map.valid &&
                                          s_imu_calibration.wX_map.valid;

        if (s_imu_calibration.is_calibrated)
        {
            s_imu_calibration.current_step = IMU_CAL_STEP_DONE;
            s_last_telemetry_time_us = 0U;
            imu_send_cal_done();
        }
        else
        {
            imu_send_cal_error(IMU_CAL_RESULT_AMBIG, finished_step);
        }
        return;
    }

    s_imu_calibration.current_step = (imu_cal_step_t) (finished_step + 1);
    imu_send_cal_step();
}

static void imu_handle_button_event_legacy(uint32_t now_us)
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
    imu_calibration_begin(now_us);
}

static uint8_t imu_get_grip_percent(void)
{
    return 0U;
}

static bool imu_try_build_servo_pose_legacy(imu_servo_pose_t * p_pose)
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

static void send_pose_frame_legacy(imu_servo_pose_t const * p_pose)
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
static void imu_handle_button_event(uint32_t now_us)
{
    if (s_imu_calibration.button_pending)
    {
        s_imu_calibration.button_pending = false;

        if (((now_us - s_imu_calibration.last_button_time_us) >= IMU_BUTTON_DEBOUNCE_US) &&
            imu_is_button_pressed())
        {
            s_imu_calibration.last_button_time_us = now_us;
            s_imu_calibration.button_press_start_us = now_us;
            s_imu_calibration.button_press_active = true;
            s_imu_calibration.button_long_handled = false;
        }
    }

    if (!s_imu_calibration.button_press_active)
    {
        return;
    }

    if (imu_is_button_pressed())
    {
        if ((!s_imu_calibration.button_long_handled) &&
            ((now_us - s_imu_calibration.button_press_start_us) >= IMU_BUTTON_LONG_PRESS_US))
        {
            s_imu_calibration.button_long_handled = true;
            s_imu_calibration.button_press_active = false;
            imu_calibration_begin(now_us);
        }

        return;
    }

    s_imu_calibration.button_press_active = false;
    if (!s_imu_calibration.button_long_handled)
    {
        imu_handle_calibration_next(now_us);
    }
}

static void imu_handle_uart_commands(uint32_t now_us)
{
    char line[IMU_UART_LINE_MAX_LEN] = {0};

    while (drv_uart_read_line(line, sizeof(line)))
    {
        if ('\0' == line[0])
        {
            continue;
        }

        imu_handle_uart_command(line, now_us);
    }
}

static void imu_handle_uart_command(char * p_line, uint32_t now_us)
{
    if (NULL == p_line)
    {
        return;
    }

    imu_ascii_to_upper(p_line);

    if (0 == strcmp(p_line, "CAL,START"))
    {
        imu_calibration_begin(now_us);
    }
    else if (0 == strcmp(p_line, "CAL,NEXT"))
    {
        imu_handle_calibration_next(now_us);
    }
    else if (0 == strcmp(p_line, "CAL,RESET"))
    {
        imu_calibration_begin(now_us);
    }
    else if (0 == strcmp(p_line, "CAL,STATUS"))
    {
        imu_send_cal_state();
    }
}

static int32_t imu_apply_axis_map(imu_axis_map_t const * p_map, imu_motion_components_t const * p_motion)
{
    float input_deg = 0.0f;

    if ((NULL == p_map) || (NULL == p_motion) || !p_map->valid || (p_map->component > IMU_COMPONENT_Z))
    {
        return 0;
    }

    if (IMU_SIGNAL_SOURCE_UPPER == p_map->source)
    {
        input_deg = p_motion->upper_rad[p_map->component] * IMU_RAD_TO_DEG;
    }
    else
    {
        input_deg = p_motion->relative_rad[p_map->component] * IMU_RAD_TO_DEG;
    }

    return (int32_t) roundf((float) p_map->center_deg + (p_map->gain * input_deg));
}

static bool imu_try_build_servo_pose(imu_servo_pose_t * p_pose)
{
    imu_motion_components_t motion = {0};
    int32_t                 hY_deg;
    int32_t                 hZ_deg;
    int32_t                 eZ_deg;
    int32_t                 wX_deg;

    if ((NULL == p_pose) ||
        !s_imu_calibration.is_calibrated ||
        !s_imu_calibration.hY_map.valid ||
        !s_imu_calibration.hZ_map.valid ||
        !s_imu_calibration.eZ_map.valid ||
        !s_imu_calibration.wX_map.valid)
    {
        return false;
    }

    if (!imu_capture_motion_components(&motion))
    {
        return false;
    }

    hY_deg = imu_apply_axis_map(&s_imu_calibration.hY_map, &motion);
    hZ_deg = imu_apply_axis_map(&s_imu_calibration.hZ_map, &motion);
    eZ_deg = imu_apply_axis_map(&s_imu_calibration.eZ_map, &motion);
    wX_deg = imu_apply_axis_map(&s_imu_calibration.wX_map, &motion);

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
        imu_send_text(frame);
    }
}

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
