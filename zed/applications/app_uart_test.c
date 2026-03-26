#include "app.h"
#include "drv_uart.h"
#include "hal_data.h"
#include "icm42688.h"
#include "imu_app_context.h"
#include "imu_calibration.h"
#include "imu_protocol.h"
#include "imu_runtime.h"
#include "emg_runtime.h"
#include <math.h>
#include <stdio.h>
#include "app_arm_link.h" //映射角度

static imu_app_context_t s_imu_app = {0};

#define IMU_DEBUG_ZERO_DRIFT_ONLY               0
#define IMU_DEBUG_ZERO_DRIFT_REPORT_INTERVAL_US 500000U
#define IMU_DEBUG_ZERO_DRIFT_SPIKE_DPS          0.20f

static void imu_fail_stop(uint32_t step, fsp_err_t err);
static void imu_set_status_led(bool led_on);
static void imu_update_status_led(uint32_t now_us);
static bool imu_is_button_pressed(void);
static void imu_handle_button_event(uint32_t now_us);
static void imu_status_led_wait_hook(void * p_context, uint32_t now_us);
static float imu_monitor_vector_norm_dps(icm42688Float3_t const * p_value_dps);
static float imu_monitor_max_abs_axis_dps(icm42688Float3_t const * p_value_dps);
static char const * imu_monitor_grade(float residual_norm_dps, float activity_norm_dps);
static void imu_print_zero_drift_metrics(char const * p_label,
                                         imu_runtime_t const * p_imu,
                                         icm42688Float3_t const * p_sum_dps,
                                         icm42688Float3_t const * p_abs_sum_dps,
                                         icm42688Float3_t const * p_effective_bias_dps,
                                         uint32_t processed_count,
                                         uint32_t ready_count,
                                         uint32_t window_us);
static void imu_run_zero_drift_monitor(void);
/* Mainline firmware keeps only the IMU acquisition and calibration path. */

void icu8_callback(external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (8 == p_args->channel))
    {
        imu_mark_data_ready(&s_imu_app.upper_imu, &s_imu_app.timebase);
    }
}

void icu9_callback(external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (9 == p_args->channel))
    {
        imu_mark_data_ready(&s_imu_app.lower_imu, &s_imu_app.timebase);
    }
}

void botton6_callback(external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (6 == p_args->channel))
    {
        s_imu_app.calibration.button_pending = true;
    }
}

void imu_test(void)
{
    fsp_err_t err;
    uint32_t  upper_calibration_samples;
    uint32_t  lower_calibration_samples;

    err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(1U, err);
    }
    s_imu_app.uart_ready = true;

    err = drv_uart_start_rx();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(10U, err);
    }

    imu_timebase_init(&s_imu_app.timebase);

    err = bsp_Icm42688Init();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(2U, err);
    }

    err = bsp_Icm42688SciInit();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(3U, err);
    }

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

    imu_runtime_reset(&s_imu_app.upper_imu);
    imu_runtime_reset(&s_imu_app.lower_imu);
    imu_calibration_begin(&s_imu_app, imu_time_now_us(&s_imu_app.timebase));
    imu_update_status_led(imu_time_now_us(&s_imu_app.timebase));

    upper_calibration_samples = imu_collect_gyro_bias(&s_imu_app.upper_imu,
                                                      bsp_IcmGetScaledData,
                                                      &s_imu_app.timebase,
                                                      imu_status_led_wait_hook,
                                                      &s_imu_app);
    lower_calibration_samples = imu_collect_gyro_bias(&s_imu_app.lower_imu,
                                                      bsp_IcmSciGetScaledData,
                                                      &s_imu_app.timebase,
                                                      imu_status_led_wait_hook,
                                                      &s_imu_app);
    (void) upper_calibration_samples;
    (void) lower_calibration_samples;
    s_imu_app.upper_imu.pending_ready_count = 0U;
    s_imu_app.lower_imu.pending_ready_count = 0U;
    s_imu_app.last_telemetry_time_us = 0U;

#if IMU_DEBUG_ZERO_DRIFT_ONLY
    imu_run_zero_drift_monitor();
#endif

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

    s_imu_app.calibration.button_pending = false;
    //
    arm_link_init();
    //
    while (1)
    {
        icm42688Float3_t upper_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t upper_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        float            upper_temperature_c = s_imu_app.upper_imu.current_temperature_c;
        float            lower_temperature_c = s_imu_app.lower_imu.current_temperature_c;
        uint32_t         upper_sample_time_us = 0U;
        uint32_t         lower_sample_time_us = 0U;
        uint32_t         upper_ready_count = 0U;
        uint32_t         lower_ready_count = 0U;
        uint32_t         frame_sample_time_us = 0U;
        uint32_t         loop_time_us;
        bool             upper_updated;
        bool             lower_updated;

        upper_updated = imu_try_read_sample(&s_imu_app.upper_imu,
                                            bsp_IcmGetScaledData,
                                            &upper_acc_g,
                                            &upper_gyro_rad_s,
                                            &upper_temperature_c,
                                            &upper_sample_time_us,
                                            &upper_ready_count,
                                            &s_imu_app.timebase);
        lower_updated = imu_try_read_sample(&s_imu_app.lower_imu,
                                            bsp_IcmSciGetScaledData,
                                            &lower_acc_g,
                                            &lower_gyro_rad_s,
                                            &lower_temperature_c,
                                            &lower_sample_time_us,
                                            &lower_ready_count,
                                            &s_imu_app.timebase);

        if (upper_updated)
        {
            imu_apply_temperature_compensation(&s_imu_app.upper_imu,
                                               &upper_acc_g,
                                               &upper_gyro_rad_s,
                                               upper_temperature_c,
                                               &upper_gyro_rad_s,
                                               NULL);
            if (!imu_should_reject_static_spike(&s_imu_app.upper_imu, &upper_acc_g, &upper_gyro_rad_s))
            {
                imu_mahony_update(&s_imu_app.upper_imu,
                                  &upper_acc_g,
                                  &upper_gyro_rad_s,
                                  imu_calc_dt_sec(&s_imu_app.upper_imu, upper_sample_time_us));
                frame_sample_time_us = upper_sample_time_us;
            }
            else
            {
                upper_updated = false;
            }
        }

        if (lower_updated)
        {
            imu_apply_temperature_compensation(&s_imu_app.lower_imu,
                                               &lower_acc_g,
                                               &lower_gyro_rad_s,
                                               lower_temperature_c,
                                               &lower_gyro_rad_s,
                                               NULL);
            if (!imu_should_reject_static_spike(&s_imu_app.lower_imu, &lower_acc_g, &lower_gyro_rad_s))
            {
                imu_mahony_update(&s_imu_app.lower_imu,
                                  &lower_acc_g,
                                  &lower_gyro_rad_s,
                                  imu_calc_dt_sec(&s_imu_app.lower_imu, lower_sample_time_us));
                if ((!upper_updated) || (lower_sample_time_us > frame_sample_time_us))
                {
                    frame_sample_time_us = lower_sample_time_us;
                }
            }
            else
            {
                lower_updated = false;
            }
        }

        loop_time_us = (upper_updated || lower_updated) ? frame_sample_time_us : imu_time_now_us(&s_imu_app.timebase);

        imu_handle_button_event(loop_time_us);
        imu_protocol_handle_uart_commands(&s_imu_app, loop_time_us);
        imu_update_status_led(loop_time_us);

        if ((upper_updated || lower_updated) &&
            s_imu_app.calibration.is_calibrated &&
            ((0U == s_imu_app.last_telemetry_time_us) ||
             ((loop_time_us - s_imu_app.last_telemetry_time_us) >= TELEMETRY_MIN_INTERVAL_US)))
        {
            imu_servo_pose_t pose = {0U, 0U, 0U, 0U, 0U};

            if (imu_try_build_servo_pose(&s_imu_app, &pose))
            {
                imu_protocol_send_pose_frame(&s_imu_app, &pose);
                /* Drive the local arm mirror from the same pose frame we publish. */
                arm_apply_imu_pose_to_servos(&pose);
                s_imu_app.last_telemetry_time_us = loop_time_us;
            }
        }
        else
        {
            R_BSP_SoftwareDelay(IMU_IDLE_POLL_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
        }
    }
}

void adc_emg_print_test(void)
{
    fsp_err_t err;

    err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(101U, err);
    }
    s_imu_app.uart_ready = true;

    err = drv_uart_start_rx();
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        imu_fail_stop(102U, err);
    }

    err = emg_runtime_init();
    if (FSP_SUCCESS != err)
    {
        imu_fail_stop(103U, err);
    }

    printf("ADC_EMG_TEST,START\r\n");

    while (1)
    {
        err = emg_runtime_process_next_sample();
        if (FSP_SUCCESS == err)
        {
            printf("%.2f,%ld\r\n",
                   (double) emg_runtime_get_last_filtered_value(),
                   (long) emg_runtime_get_last_envelope());
        }
        else
        {
            printf("ADC_EMG_TEST,ERR,%d\r\n", (int) err);
            R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }
}

static void imu_fail_stop(uint32_t step, fsp_err_t err)
{
    s_imu_app.fail_step = step;
    s_imu_app.last_error = err;

    if (s_imu_app.uart_ready)
    {
        printf("imu_test failed: step=%lu err=%d\r\n", (unsigned long) step, (int) err);
    }

    while (1)
    {
        R_BSP_SoftwareDelay(100U, BSP_DELAY_UNITS_MILLISECONDS);
    }
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

    if (s_imu_app.calibration.led_flash_active)
    {
        uint32_t elapsed_us = now_us - s_imu_app.calibration.led_flash_start_us;
        uint32_t pulse_period_us = IMU_LED_FLASH_ON_US + IMU_LED_FLASH_GAP_US;
        uint32_t pattern_window_us = (uint32_t) s_imu_app.calibration.led_flash_pulses * pulse_period_us;

        if (elapsed_us < pattern_window_us)
        {
            led_on = ((elapsed_us % pulse_period_us) < IMU_LED_FLASH_ON_US);
            imu_set_status_led(led_on);
            return;
        }

        s_imu_app.calibration.led_flash_active = false;
    }

    if (s_imu_app.calibration.is_calibrated)
    {
        imu_set_status_led(true);
        return;
    }

    {
        uint32_t wait_pulses = (uint32_t) (s_imu_app.calibration.current_step + 1U);
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

static bool imu_is_button_pressed(void)
{
    bsp_io_level_t pin_level = BSP_IO_LEVEL_HIGH;

    if (FSP_SUCCESS != R_IOPORT_PinRead(&g_ioport_ctrl, IMU_BUTTON_PIN, &pin_level))
    {
        return false;
    }

    return (BSP_IO_LEVEL_LOW == pin_level);
}

static void imu_handle_button_event(uint32_t now_us)
{
    if (s_imu_app.calibration.button_pending)
    {
        s_imu_app.calibration.button_pending = false;

        if (((now_us - s_imu_app.calibration.last_button_time_us) >= IMU_BUTTON_DEBOUNCE_US) &&
            imu_is_button_pressed())
        {
            s_imu_app.calibration.last_button_time_us = now_us;
            s_imu_app.calibration.button_press_start_us = now_us;
            s_imu_app.calibration.button_press_active = true;
            s_imu_app.calibration.button_long_handled = false;
        }
    }

    if (!s_imu_app.calibration.button_press_active)
    {
        return;
    }

    if (imu_is_button_pressed())
    {
        if ((!s_imu_app.calibration.button_long_handled) &&
            ((now_us - s_imu_app.calibration.button_press_start_us) >= IMU_BUTTON_LONG_PRESS_US))
        {
            s_imu_app.calibration.button_long_handled = true;
            s_imu_app.calibration.button_press_active = false;
            imu_calibration_begin(&s_imu_app, now_us);
        }

        return;
    }

    s_imu_app.calibration.button_press_active = false;
    if (!s_imu_app.calibration.button_long_handled)
    {
        imu_calibration_handle_next(&s_imu_app, now_us);
    }
}

static void imu_status_led_wait_hook(void * p_context, uint32_t now_us)
{
    (void) p_context;
    imu_update_status_led(now_us);
}

static float imu_monitor_vector_norm_dps(icm42688Float3_t const * p_value_dps)
{
    if (NULL == p_value_dps)
    {
        return 0.0f;
    }

    return sqrtf((p_value_dps->x * p_value_dps->x) +
                 (p_value_dps->y * p_value_dps->y) +
                 (p_value_dps->z * p_value_dps->z));
}

static float imu_monitor_max_abs_axis_dps(icm42688Float3_t const * p_value_dps)
{
    float max_value = 0.0f;

    if (NULL == p_value_dps)
    {
        return 0.0f;
    }

    max_value = fabsf(p_value_dps->x);
    if (fabsf(p_value_dps->y) > max_value)
    {
        max_value = fabsf(p_value_dps->y);
    }

    if (fabsf(p_value_dps->z) > max_value)
    {
        max_value = fabsf(p_value_dps->z);
    }

    return max_value;
}

static char const * imu_monitor_grade(float residual_norm_dps, float activity_norm_dps)
{
    if ((residual_norm_dps <= 0.05f) && (activity_norm_dps <= 0.12f))
    {
        return "EXCELLENT";
    }

    if ((residual_norm_dps <= 0.15f) && (activity_norm_dps <= 0.30f))
    {
        return "GOOD";
    }

    if ((residual_norm_dps <= 0.40f) && (activity_norm_dps <= 0.80f))
    {
        return "OK";
    }

    if ((residual_norm_dps <= 0.80f) && (activity_norm_dps <= 1.50f))
    {
        return "WARN";
    }

    return "BAD";
}

static void imu_print_zero_drift_metrics(char const * p_label,
                                         imu_runtime_t const * p_imu,
                                         icm42688Float3_t const * p_sum_dps,
                                         icm42688Float3_t const * p_abs_sum_dps,
                                         icm42688Float3_t const * p_effective_bias_dps,
                                         uint32_t processed_count,
                                         uint32_t ready_count,
                                         uint32_t window_us)
{
    icm42688Float3_t avg_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t abs_avg_dps = {0.0f, 0.0f, 0.0f};
    float            residual_norm_dps = 0.0f;
    float            activity_norm_dps = 0.0f;
    float            bias_norm_dps = 0.0f;
    float            max_axis_dps = 0.0f;
    float            temperature_delta_c = 0.0f;
    float            sample_rate_hz = 0.0f;

    if ((NULL == p_label) || (NULL == p_imu) || (NULL == p_sum_dps) || (NULL == p_abs_sum_dps) ||
        (NULL == p_effective_bias_dps) || (0U == processed_count))
    {
        printf("%s_GRADE,NA,", (NULL != p_label) ? p_label : "IMU");
        return;
    }

    avg_dps.x = p_sum_dps->x / (float) processed_count;
    avg_dps.y = p_sum_dps->y / (float) processed_count;
    avg_dps.z = p_sum_dps->z / (float) processed_count;
    abs_avg_dps.x = p_abs_sum_dps->x / (float) processed_count;
    abs_avg_dps.y = p_abs_sum_dps->y / (float) processed_count;
    abs_avg_dps.z = p_abs_sum_dps->z / (float) processed_count;
    residual_norm_dps = imu_monitor_vector_norm_dps(&avg_dps);
    activity_norm_dps = imu_monitor_vector_norm_dps(&abs_avg_dps);
    bias_norm_dps = imu_monitor_vector_norm_dps(p_effective_bias_dps);
    max_axis_dps = imu_monitor_max_abs_axis_dps(&avg_dps);
    temperature_delta_c = p_imu->filtered_temperature_c - p_imu->bias_temperature_c;
    if ((window_us > 0U) && (ready_count > 0U))
    {
        sample_rate_hz = ((float) ready_count * 1000000.0f) / (float) window_us;
    }

    printf("%s_GRADE,%s,", p_label, imu_monitor_grade(residual_norm_dps, activity_norm_dps));
    printf("%s_RATE_HZ,%.1f,%s_TEMP_C,%.2f,%s_DTEMP_C,%.2f,",
           p_label,
           sample_rate_hz,
           p_label,
           p_imu->filtered_temperature_c,
           p_label,
           temperature_delta_c);
    printf("%s_BIAS_NORM_DPS,%.5f,%s_RES_NORM_DPS,%.5f,%s_ACTIVITY_NORM_DPS,%.5f,%s_MAX_AXIS_DPS,%.5f,",
           p_label,
           bias_norm_dps,
           p_label,
           residual_norm_dps,
           p_label,
           activity_norm_dps,
           p_label,
           max_axis_dps);
    printf("%s_AVG_DPS,%.5f,%.5f,%.5f,%s_ABS_DPS,%.5f,%.5f,%.5f,",
           p_label,
           avg_dps.x,
           avg_dps.y,
           avg_dps.z,
           p_label,
           abs_avg_dps.x,
           abs_avg_dps.y,
           abs_avg_dps.z);
    printf("%s_BIAS_DPS,%.5f,%.5f,%.5f,",
           p_label,
           p_effective_bias_dps->x,
           p_effective_bias_dps->y,
           p_effective_bias_dps->z);
}

static void imu_run_zero_drift_monitor(void)
{
    icm42688Float3_t upper_sum_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t lower_sum_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t upper_abs_sum_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t lower_abs_sum_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t upper_effective_bias_dps = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t lower_effective_bias_dps = {0.0f, 0.0f, 0.0f};
    float            upper_peak_dps = 0.0f;
    float            lower_peak_dps = 0.0f;
    uint32_t         upper_processed_count = 0U;
    uint32_t         lower_processed_count = 0U;
    uint32_t         upper_ready_count = 0U;
    uint32_t         lower_ready_count = 0U;
    uint32_t         upper_spike_count = 0U;
    uint32_t         lower_spike_count = 0U;
    uint32_t         last_report_time_us = imu_time_now_us(&s_imu_app.timebase);
    float            upper_temperature_c = s_imu_app.upper_imu.current_temperature_c;
    float            lower_temperature_c = s_imu_app.lower_imu.current_temperature_c;

    printf("ZERO_DRIFT_MONITOR,START,keep_still\r\n");

    while (1)
    {
        icm42688Float3_t upper_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t upper_raw_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t upper_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_raw_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_gyro_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t upper_effective_bias_rad_s = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t lower_effective_bias_rad_s = {0.0f, 0.0f, 0.0f};
        uint32_t         upper_sample_time_us = 0U;
        uint32_t         lower_sample_time_us = 0U;
        uint32_t         upper_sample_ready_count = 0U;
        uint32_t         lower_sample_ready_count = 0U;
        uint32_t         loop_time_us = 0U;
        bool             upper_updated;
        bool             lower_updated;

        upper_updated = imu_try_read_sample(&s_imu_app.upper_imu,
                                            bsp_IcmGetScaledData,
                                            &upper_acc_g,
                                            &upper_raw_gyro_rad_s,
                                            &upper_temperature_c,
                                            &upper_sample_time_us,
                                            &upper_sample_ready_count,
                                            &s_imu_app.timebase);
        lower_updated = imu_try_read_sample(&s_imu_app.lower_imu,
                                            bsp_IcmSciGetScaledData,
                                            &lower_acc_g,
                                            &lower_raw_gyro_rad_s,
                                            &lower_temperature_c,
                                            &lower_sample_time_us,
                                            &lower_sample_ready_count,
                                            &s_imu_app.timebase);

        if (upper_updated)
        {
            float upper_sample_peak_dps;

            imu_apply_temperature_compensation(&s_imu_app.upper_imu,
                                               &upper_acc_g,
                                               &upper_raw_gyro_rad_s,
                                               upper_temperature_c,
                                               &upper_gyro_rad_s,
                                               &upper_effective_bias_rad_s);
            upper_effective_bias_dps.x = upper_effective_bias_rad_s.x * IMU_RAD_TO_DEG;
            upper_effective_bias_dps.y = upper_effective_bias_rad_s.y * IMU_RAD_TO_DEG;
            upper_effective_bias_dps.z = upper_effective_bias_rad_s.z * IMU_RAD_TO_DEG;
            upper_gyro_rad_s.x *= IMU_RAD_TO_DEG;
            upper_gyro_rad_s.y *= IMU_RAD_TO_DEG;
            upper_gyro_rad_s.z *= IMU_RAD_TO_DEG;
            upper_sample_peak_dps = imu_monitor_max_abs_axis_dps(&upper_gyro_rad_s);

            upper_sum_dps.x += upper_gyro_rad_s.x;
            upper_sum_dps.y += upper_gyro_rad_s.y;
            upper_sum_dps.z += upper_gyro_rad_s.z;
            upper_abs_sum_dps.x += fabsf(upper_gyro_rad_s.x);
            upper_abs_sum_dps.y += fabsf(upper_gyro_rad_s.y);
            upper_abs_sum_dps.z += fabsf(upper_gyro_rad_s.z);
            if (upper_sample_peak_dps > upper_peak_dps)
            {
                upper_peak_dps = upper_sample_peak_dps;
            }

            if (upper_sample_peak_dps >= IMU_DEBUG_ZERO_DRIFT_SPIKE_DPS)
            {
                upper_spike_count++;
            }

            upper_processed_count++;
            upper_ready_count += upper_sample_ready_count;
            loop_time_us = upper_sample_time_us;
        }

        if (lower_updated)
        {
            float lower_sample_peak_dps;

            imu_apply_temperature_compensation(&s_imu_app.lower_imu,
                                               &lower_acc_g,
                                               &lower_raw_gyro_rad_s,
                                               lower_temperature_c,
                                               &lower_gyro_rad_s,
                                               &lower_effective_bias_rad_s);
            lower_effective_bias_dps.x = lower_effective_bias_rad_s.x * IMU_RAD_TO_DEG;
            lower_effective_bias_dps.y = lower_effective_bias_rad_s.y * IMU_RAD_TO_DEG;
            lower_effective_bias_dps.z = lower_effective_bias_rad_s.z * IMU_RAD_TO_DEG;
            lower_gyro_rad_s.x *= IMU_RAD_TO_DEG;
            lower_gyro_rad_s.y *= IMU_RAD_TO_DEG;
            lower_gyro_rad_s.z *= IMU_RAD_TO_DEG;
            lower_sample_peak_dps = imu_monitor_max_abs_axis_dps(&lower_gyro_rad_s);

            lower_sum_dps.x += lower_gyro_rad_s.x;
            lower_sum_dps.y += lower_gyro_rad_s.y;
            lower_sum_dps.z += lower_gyro_rad_s.z;
            lower_abs_sum_dps.x += fabsf(lower_gyro_rad_s.x);
            lower_abs_sum_dps.y += fabsf(lower_gyro_rad_s.y);
            lower_abs_sum_dps.z += fabsf(lower_gyro_rad_s.z);
            if (lower_sample_peak_dps > lower_peak_dps)
            {
                lower_peak_dps = lower_sample_peak_dps;
            }

            if (lower_sample_peak_dps >= IMU_DEBUG_ZERO_DRIFT_SPIKE_DPS)
            {
                lower_spike_count++;
            }

            lower_processed_count++;
            lower_ready_count += lower_sample_ready_count;
            if ((!upper_updated) || (lower_sample_time_us > loop_time_us))
            {
                loop_time_us = lower_sample_time_us;
            }
        }

        if (!upper_updated && !lower_updated)
        {
            loop_time_us = imu_time_now_us(&s_imu_app.timebase);
            R_BSP_SoftwareDelay(IMU_IDLE_POLL_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
        }

        imu_update_status_led(loop_time_us);

        if ((loop_time_us - last_report_time_us) >= IMU_DEBUG_ZERO_DRIFT_REPORT_INTERVAL_US)
        {
            uint32_t window_us = loop_time_us - last_report_time_us;

            printf("ZERO_DRIFT_MONITOR,%lu,WINDOW_MS,%.1f,",
                   (unsigned long) loop_time_us,
                   (float) window_us / 1000.0f);

            if (upper_processed_count > 0U)
            {
                imu_print_zero_drift_metrics("UPPER",
                                             &s_imu_app.upper_imu,
                                             &upper_sum_dps,
                                             &upper_abs_sum_dps,
                                             &upper_effective_bias_dps,
                                             upper_processed_count,
                                             upper_ready_count,
                                             window_us);
                printf("UPPER_PEAK_DPS,%.5f,UPPER_SPIKE_COUNT,%lu,",
                       upper_peak_dps,
                       (unsigned long) upper_spike_count);
            }
            else
            {
                printf("UPPER_GRADE,NA,UPPER_RATE_HZ,NA,UPPER_TEMP_C,NA,UPPER_DTEMP_C,NA,UPPER_BIAS_NORM_DPS,NA,UPPER_RES_NORM_DPS,NA,UPPER_ACTIVITY_NORM_DPS,NA,UPPER_MAX_AXIS_DPS,NA,UPPER_AVG_DPS,NA,NA,NA,UPPER_ABS_DPS,NA,NA,NA,UPPER_BIAS_DPS,NA,NA,NA,UPPER_PEAK_DPS,NA,UPPER_SPIKE_COUNT,NA,");
            }

            if (lower_processed_count > 0U)
            {
                imu_print_zero_drift_metrics("LOWER",
                                             &s_imu_app.lower_imu,
                                             &lower_sum_dps,
                                             &lower_abs_sum_dps,
                                             &lower_effective_bias_dps,
                                             lower_processed_count,
                                             lower_ready_count,
                                             window_us);
                printf("LOWER_PEAK_DPS,%.5f,LOWER_SPIKE_COUNT,%lu\r\n",
                       lower_peak_dps,
                       (unsigned long) lower_spike_count);
            }
            else
            {
                printf("LOWER_GRADE,NA,LOWER_RATE_HZ,NA,LOWER_TEMP_C,NA,LOWER_DTEMP_C,NA,LOWER_BIAS_NORM_DPS,NA,LOWER_RES_NORM_DPS,NA,LOWER_ACTIVITY_NORM_DPS,NA,LOWER_MAX_AXIS_DPS,NA,LOWER_AVG_DPS,NA,NA,NA,LOWER_ABS_DPS,NA,NA,NA,LOWER_BIAS_DPS,NA,NA,NA,LOWER_PEAK_DPS,NA,LOWER_SPIKE_COUNT,NA\r\n");
            }

            upper_sum_dps.x = 0.0f;
            upper_sum_dps.y = 0.0f;
            upper_sum_dps.z = 0.0f;
            lower_sum_dps.x = 0.0f;
            lower_sum_dps.y = 0.0f;
            lower_sum_dps.z = 0.0f;
            upper_abs_sum_dps.x = 0.0f;
            upper_abs_sum_dps.y = 0.0f;
            upper_abs_sum_dps.z = 0.0f;
            lower_abs_sum_dps.x = 0.0f;
            lower_abs_sum_dps.y = 0.0f;
            lower_abs_sum_dps.z = 0.0f;
            upper_peak_dps = 0.0f;
            lower_peak_dps = 0.0f;
            upper_processed_count = 0U;
            lower_processed_count = 0U;
            upper_ready_count = 0U;
            lower_ready_count = 0U;
            upper_spike_count = 0U;
            lower_spike_count = 0U;
            last_report_time_us = loop_time_us;
        }
    }
}
