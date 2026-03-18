#ifndef IMU_APP_CONTEXT_H
#define IMU_APP_CONTEXT_H

#include "hal_data.h"
#include "icm42688.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_SAMPLE_DT_DEFAULT_SEC     0.01f
#define IMU_SAMPLE_DT_MIN_SEC         0.001f
#define IMU_SAMPLE_DT_MAX_SEC         0.030f
#define IMU_CALIBRATION_SAMPLES       200U
#define IMU_CALIBRATION_MAX_ATTEMPTS  400U
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
#define IMU_RAD_TO_DEG                57.295779513082320876f
#define IMU_STATUS_LED_PIN            BSP_IO_PORT_04_PIN_00
#define IMU_BUTTON_PIN                BSP_IO_PORT_00_PIN_00
#define IMU_AXIS_MIN_RESPONSE_DEG     20.0f
#define IMU_CAL_TARGET_DELTA_DEG      90.0f
#define IMU_VECTOR_EPSILON            0.0001f
#define IMU_RUNTIME_RAW_FILTER_ALPHA  0.20f
#define IMU_RUNTIME_MAX_STEP_DEG      12.0f
#define IMU_RUNTIME_OUTPUT_DEADBAND   1
#define IMU_UART_LINE_MAX_LEN         64U

typedef void (*imu_read_sample_fn_t)(icm42688Float3_t * p_acc_g, icm42688Float3_t * p_gyro_rad_s);

typedef struct st_imu_runtime
{
    Quaternion_t     quat;
    icm42688Float3_t gyro_bias;
    icm42688Float3_t mahony_integral;
    uint32_t         last_sample_time_us;
    volatile bool    data_ready;
} imu_runtime_t;

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

typedef enum e_imu_cal_step
{
    IMU_CAL_STEP_TPOSE = 0,
    IMU_CAL_STEP_HY = 1,
    IMU_CAL_STEP_HZ = 2,
    IMU_CAL_STEP_EZ = 3,
    IMU_CAL_STEP_WX = 4,
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
    Quaternion_t upper_bone;
    Quaternion_t relative_bone;
} imu_motion_components_t;

typedef enum e_imu_angle_measure
{
    IMU_ANGLE_MEASURE_SWING = 0,
    IMU_ANGLE_MEASURE_TWIST = 1,
} imu_angle_measure_t;

typedef struct st_imu_axis_map
{
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

typedef struct st_imu_calibration_runtime
{
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
    uint32_t cycles_per_us;
    uint32_t last_cycle_count;
    uint64_t cycle_accumulator;
} imu_timebase_t;

typedef struct st_imu_app_context
{
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
