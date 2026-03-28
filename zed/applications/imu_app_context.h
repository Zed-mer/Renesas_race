#ifndef IMU_APP_CONTEXT_H
#define IMU_APP_CONTEXT_H

#include "app.h"
#include "hal_data.h"
#include "icm42688.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * applications 层共享的数据字典：
 * 这里定义了算法常量、运行时状态、标定状态和输出帧结构。
 */

/* IMU 应用层共用的配置常量，集中放在这里方便统一调整。 */
#define IMU_SAMPLE_DT_DEFAULT_SEC     0.002f
#define IMU_SAMPLE_DT_MIN_SEC         0.0005f
#define IMU_SAMPLE_DT_MAX_SEC         0.010f
/*
 * 读取频率从 100Hz 提到 500Hz 后，零偏采样的“样本数”也要同步放大，
 * 这样启动零偏平均的总时长仍然维持在大约 2 秒量级，不会因为频率更高反而采得更短。
 */
#define IMU_CALIBRATION_SAMPLES       1000U
#define IMU_CALIBRATION_MAX_ATTEMPTS  2000U
#define IMU_STATIC_ACC_MIN_G          0.85f
#define IMU_STATIC_ACC_MAX_G          1.15f
#define IMU_CORRECTION_ACC_MIN_G      0.70f
#define IMU_CORRECTION_ACC_MAX_G      1.30f
#define IMU_IRQ_WAIT_TIMEOUT_MS       20U
#define IMU_IDLE_POLL_DELAY_US        200U
#define TELEMETRY_MIN_INTERVAL_US     20000U
#define IMU_BUTTON_DEBOUNCE_US        250000U
#define IMU_BUTTON_LONG_PRESS_US      2000000U
#define IMU_LED_FLASH_ON_US           100000U
#define IMU_LED_FLASH_GAP_US          100000U
#define IMU_LED_WAIT_PAUSE_US         500000U
#define IMU_DEG_TO_RAD                0.01745329251994329577f
#define IMU_RAD_TO_DEG                57.295779513082320876f
#define IMU_STATUS_LED_PIN            BSP_IO_PORT_04_PIN_00
#define IMU_BUTTON_PIN                BSP_IO_PORT_00_PIN_00
#define IMU_AXIS_MIN_RESPONSE_DEG     20.0f
#define IMU_CAL_TARGET_DELTA_DEG      90.0f
#define IMU_VECTOR_EPSILON            0.0001f
#define IMU_RUNTIME_RAW_FILTER_ALPHA  0.20f
#define IMU_RUNTIME_STATIC_WINDOW_SAMPLES      150U
#define IMU_RUNTIME_STATIC_ACC_MIN_G           0.92f
#define IMU_RUNTIME_STATIC_ACC_MAX_G           1.08f
#define IMU_RUNTIME_STATIC_GYRO_PEAK_MAX_RAD_S (3.0f * IMU_DEG_TO_RAD)
#define IMU_RUNTIME_STATIC_SPIKE_REJECT_RAD_S  (40.0f * IMU_DEG_TO_RAD)
/*
 * 原来 100Hz 下每帧最多 12deg，相当于约 1200deg/s 的输出变化上限。
 * 提到 500Hz 后改成 2.4deg/帧，保持相近的物理速度上限，避免输出突然变“更猛”。
 */
#define IMU_RUNTIME_MAX_STEP_DEG      2.4f
#define IMU_RUNTIME_OUTPUT_DEADBAND   1
#define IMU_UART_LINE_MAX_LEN         64U
#define IMU_CAL_MIN_BASIS_DET         0.25f
#define IMU_CAL_MIN_AXIS_SEPARATION_DEG 30.0f
#define IMU_CAL_PRIMARY_RESPONSE_TOLERANCE_DEG 40.0f
#define IMU_CAL_MAX_CROSS_LEAK_DEG    35.0f
/*
 * 温度补偿参数：
 * 1. 先对温度做低通，减少瞬时测温噪声直接抖到 bias。
 * 2. 只有温差超过阈值时才学习温度斜率，避免在接近参考温度时被噪声放大。
 * 3. 静止窗口内允许轻微回正常量 bias，把“启动均值没采准”与“温漂”分开处理。
 */
#define IMU_TEMP_COMP_MIN_DELTA_C     0.8f
#define IMU_TEMP_COMP_TEMP_ALPHA      0.02f
#define IMU_TEMP_COMP_REBIAS_ALPHA    0.001f
#define IMU_TEMP_COMP_SLOPE_ALPHA     0.001f
#define IMU_TEMP_COMP_STATIC_GYRO_RAD_S_MAX (4.0f * IMU_DEG_TO_RAD)
#define IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C (0.25f * IMU_DEG_TO_RAD)

typedef void (*imu_read_sample_fn_t)(icm42688Float3_t * p_acc_g,
                                     icm42688Float3_t * p_gyro_rad_s,
                                     float * p_temp_c);

typedef struct st_imu_runtime
{
    /* 单个 IMU 的运行时状态：当前姿态、陀螺零偏和积分项都保存在这里。 */
    Quaternion_t     quat;
    icm42688Float3_t gyro_bias;
    /* 线性温补模型 slope，单位是 rad/s/degC。*/
    icm42688Float3_t gyro_temp_slope;
    icm42688Float3_t mahony_integral;
    /* bias_temperature_c 是采集 gyro_bias 时的参考温度。*/
    float            bias_temperature_c;
    float            current_temperature_c;
    float            filtered_temperature_c;
    uint32_t         last_sample_time_us;
    volatile uint32_t data_ready_time_us;
    volatile uint16_t pending_ready_count;
    uint16_t         static_window_count;
    bool             has_temperature_reference;
    bool             has_filtered_temperature;
    bool             in_static_window;
} imu_runtime_t;

typedef struct st_imu_servo_pose
{
    /* 下游机械臂控制端期望接收的舵机输出帧。 */
    uint16_t hY_deg;
    uint16_t hZ_deg;
    uint16_t eZ_deg;
    uint16_t wX_deg;
    uint8_t  grip_percent;
} imu_servo_pose_t;

typedef enum e_imu_signal_source
{
    /* 上臂 IMU 主要驱动肩部自由度，相对姿态主要驱动前臂自由度。 */
    IMU_SIGNAL_SOURCE_UPPER = 0,
    IMU_SIGNAL_SOURCE_RELATIVE = 1,
} imu_signal_source_t;

typedef enum e_imu_cal_step
{
    /* 引导式标定流程的步骤定义，用户通过按键或串口一步步推进。 */
    IMU_CAL_STEP_TPOSE = 0,
    IMU_CAL_STEP_HY = 1,
    IMU_CAL_STEP_HZ = 2,
    IMU_CAL_STEP_EZ = 3,
    IMU_CAL_STEP_WX = 4,
    IMU_CAL_STEP_DONE = 6,
} imu_cal_step_t;

typedef enum e_imu_cal_result
{
    /* 每次记录标定姿态后的结果码，会通过串口回传给上位机或串口助手。 */
    IMU_CAL_RESULT_OK = 0,
    IMU_CAL_RESULT_WEAK = 1,
    IMU_CAL_RESULT_AMBIG = 2,
    IMU_CAL_RESULT_NODATA = 3,
} imu_cal_result_t;

typedef struct st_imu_motion_components
{
    /* 从两个 IMU 姿态中拆出来的骨段姿态，供映射逻辑继续使用。 */
    Quaternion_t upper_bone;
    Quaternion_t relative_bone;
} imu_motion_components_t;

typedef enum e_imu_angle_measure
{
    /* Swing 表示绕轴外的摆动，Twist 表示绕学习到的主轴旋转。 */
    IMU_ANGLE_MEASURE_SWING = 0,
    IMU_ANGLE_MEASURE_TWIST = 1,
} imu_angle_measure_t;

typedef struct st_imu_axis_map
{
    /* 某一个运动分量到某一个舵机输出通道的学习结果。 */
    imu_signal_source_t source;
    imu_angle_measure_t measure;
    icm42688Float3_t    axis;
    icm42688Float3_t    reference;
    float               filtered_raw_deg;
    float               gain;
    float               last_raw_deg;
    int16_t             center_deg;
    int16_t             last_output_deg;
    bool                valid;
    bool                has_filtered_raw;
    bool                has_reference;
    bool                has_last_raw;
    bool                has_last_output;
} imu_axis_map_t;

typedef struct st_imu_fk_pair_state
{
    float last_primary_deg;
    float last_secondary_deg;
    bool  initialized;
} imu_fk_pair_state_t;

typedef struct st_imu_cal_quality_metrics
{
    float basis_det;
    float axis_separation_deg;
    float primary_response_1_deg;
    float primary_response_2_deg;
    float cross_leak_1_deg;
    float cross_leak_2_deg;
} imu_cal_quality_metrics_t;

typedef struct st_imu_calibration_runtime
{
    /* 标定过程中逐步积累出来的中间状态和最终映射参数。 */
    Quaternion_t    upper_offset;
    Quaternion_t    lower_offset;
    Quaternion_t    upper_hy_pose;
    Quaternion_t    upper_hz_pose;
    Quaternion_t    relative_ez_pose;
    Quaternion_t    relative_wx_pose;
    imu_axis_map_t  hY_map;
    imu_axis_map_t  hZ_map;
    imu_axis_map_t  eZ_map;
    imu_axis_map_t  wX_map;
    imu_fk_pair_state_t upper_fk_state;
    imu_fk_pair_state_t lower_fk_state;
    imu_cal_quality_metrics_t upper_quality;
    imu_cal_quality_metrics_t lower_quality;
    uint32_t        last_button_time_us;
    uint32_t        button_press_start_us;
    uint32_t        led_flash_start_us;
    volatile bool   button_pending;
    bool            button_press_active;
    bool            button_long_handled;
    bool            is_calibrated;
    bool            led_flash_active;
    uint8_t         led_flash_pulses;
    imu_cal_step_t  current_step;
} imu_calibration_runtime_t;

typedef struct st_imu_timebase
{
    /* 由 DWT 周期计数器换算出来的微秒级时间基。 */
    uint32_t cycles_per_us;
    uint32_t last_cycle_count;
    uint64_t cycle_accumulator;
} imu_timebase_t;

typedef struct st_imu_app_context
{
    /* IMU 应用的顶层上下文，运行时更新、标定、串口协议都共享这份状态。 */
    imu_runtime_t             upper_imu;
    imu_runtime_t             lower_imu;
    imu_calibration_runtime_t calibration;
    imu_timebase_t            timebase;
    uint32_t                  last_telemetry_time_us;
    volatile uint32_t         fail_step;
    volatile fsp_err_t        last_error;
    volatile bool             uart_ready;
} imu_app_context_t;

#endif
