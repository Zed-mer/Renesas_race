#include "app.h"
#include "drv_uart.h"
#include "hal_data.h"
#include "icm42688.h"
#include "imu_app_context.h"
#include "imu_calibration.h"
#include "imu_protocol.h"
#include "imu_runtime.h"
#include <stdio.h>

static imu_app_context_t s_imu_app = {0};

static void imu_fail_stop(uint32_t step, fsp_err_t err);
static void imu_set_status_led(bool led_on);
static void imu_update_status_led(uint32_t now_us);
static bool imu_is_button_pressed(void);
static void imu_handle_button_event(uint32_t now_us);
static void imu_status_led_wait_hook(void * p_context, uint32_t now_us);

void icu8_callback(external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (8 == p_args->channel))
    {
        s_imu_app.upper_imu.data_ready = true;
    }
}

void icu9_callback(external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (9 == p_args->channel))
    {
        s_imu_app.lower_imu.data_ready = true;
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
    s_imu_app.upper_imu.data_ready = false;
    s_imu_app.lower_imu.data_ready = false;
    s_imu_app.last_telemetry_time_us = 0U;

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
        bool             upper_updated;
        bool             lower_updated;

        upper_updated = imu_try_read_sample(&s_imu_app.upper_imu,
                                            bsp_IcmGetScaledData,
                                            &upper_acc_g,
                                            &upper_gyro_rad_s,
                                            &upper_sample_time_us,
                                            &s_imu_app.timebase);
        lower_updated = imu_try_read_sample(&s_imu_app.lower_imu,
                                            bsp_IcmSciGetScaledData,
                                            &lower_acc_g,
                                            &lower_gyro_rad_s,
                                            &lower_sample_time_us,
                                            &s_imu_app.timebase);

        if (upper_updated)
        {
            upper_gyro_rad_s.x -= s_imu_app.upper_imu.gyro_bias.x;
            upper_gyro_rad_s.y -= s_imu_app.upper_imu.gyro_bias.y;
            upper_gyro_rad_s.z -= s_imu_app.upper_imu.gyro_bias.z;
            imu_mahony_update(&s_imu_app.upper_imu,
                              &upper_acc_g,
                              &upper_gyro_rad_s,
                              imu_calc_dt_sec(&s_imu_app.upper_imu, upper_sample_time_us));
            frame_sample_time_us = upper_sample_time_us;
        }

        if (lower_updated)
        {
            lower_gyro_rad_s.x -= s_imu_app.lower_imu.gyro_bias.x;
            lower_gyro_rad_s.y -= s_imu_app.lower_imu.gyro_bias.y;
            lower_gyro_rad_s.z -= s_imu_app.lower_imu.gyro_bias.z;
            imu_mahony_update(&s_imu_app.lower_imu,
                              &lower_acc_g,
                              &lower_gyro_rad_s,
                              imu_calc_dt_sec(&s_imu_app.lower_imu, lower_sample_time_us));
            if ((!upper_updated) || (lower_sample_time_us > frame_sample_time_us))
            {
                frame_sample_time_us = lower_sample_time_us;
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
                s_imu_app.last_telemetry_time_us = loop_time_us;
            }
        }
        else
        {
            R_BSP_SoftwareDelay(IMU_IDLE_POLL_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
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
