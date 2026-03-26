#include "imu_protocol.h"
#include "app_arm_link.h"
#include "drv_uart.h"
#include "imu_calibration.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* One shared scratch buffer is enough for the short CAL and POSE frames. */
#define IMU_PROTOCOL_FRAME_BUFFER_SIZE  192U

static void        imu_ascii_to_upper(char * p_text);
static char const * imu_cal_step_name(imu_cal_step_t step);
static char const * imu_cal_result_name(imu_cal_result_t result);
static void        imu_protocol_handle_gripcfg(imu_app_context_t * p_ctx, char * p_args);

void imu_protocol_send_text(imu_app_context_t * p_ctx, char const * p_text)
{
    size_t text_len;

    if ((NULL == p_ctx) || (NULL == p_text) || !p_ctx->uart_ready)
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

void imu_protocol_send_textf(imu_app_context_t * p_ctx, char const * p_format, ...)
{
    char    frame[IMU_PROTOCOL_FRAME_BUFFER_SIZE] = {0};
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
        imu_protocol_send_text(p_ctx, frame);
    }
}

void imu_protocol_send_cal_step(imu_app_context_t * p_ctx)
{
    imu_protocol_send_textf(p_ctx,
                            "CAL,STEP,%u,%s\r\n",
                            (unsigned int) (p_ctx->calibration.current_step + 1U),
                            imu_cal_step_name(p_ctx->calibration.current_step));
}

void imu_protocol_send_cal_ok(imu_app_context_t * p_ctx, imu_cal_step_t step)
{
    imu_protocol_send_textf(p_ctx,
                            "CAL,OK,%u,%s\r\n",
                            (unsigned int) (step + 1U),
                            imu_cal_step_name(step));
}

void imu_protocol_send_cal_error(imu_app_context_t * p_ctx, imu_cal_result_t result, imu_cal_step_t step)
{
    imu_protocol_send_textf(p_ctx,
                            "CAL,ERR,%s,%s\r\n",
                            imu_cal_result_name(result),
                            imu_cal_step_name(step));
}

void imu_protocol_send_cal_done(imu_app_context_t * p_ctx)
{
    imu_protocol_send_text(p_ctx, "CAL,DONE\r\n");
}

void imu_protocol_send_cal_state(imu_app_context_t * p_ctx)
{
    if (p_ctx->calibration.is_calibrated)
    {
        imu_protocol_send_text(p_ctx, "CAL,STATE,5,DONE,1\r\n");
    }
    else
    {
        imu_protocol_send_textf(p_ctx,
                                "CAL,STATE,%u,%s,0\r\n",
                                (unsigned int) (p_ctx->calibration.current_step + 1U),
                                imu_cal_step_name(p_ctx->calibration.current_step));
    }
}

void imu_protocol_send_pose_frame(imu_app_context_t * p_ctx, imu_servo_pose_t const * p_pose)
{
    char frame[48] = {0};
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
        imu_protocol_send_text(p_ctx, frame);
    }
}

void imu_protocol_handle_uart_commands(imu_app_context_t * p_ctx, uint32_t now_us)
{
    char line[IMU_UART_LINE_MAX_LEN] = {0};

    if (NULL == p_ctx)
    {
        return;
    }

    while (drv_uart_read_line(line, sizeof(line)))
    {
        if ('\0' == line[0])
        {
            continue;
        }

        imu_ascii_to_upper(line);

        if ((0 == strcmp(line, "CAL,START")) || (0 == strcmp(line, "CAL,RESET")))
        {
            imu_calibration_begin(p_ctx, now_us);
        }
        else if (0 == strcmp(line, "CAL,NEXT"))
        {
            imu_calibration_handle_next(p_ctx, now_us);
        }
        else if (0 == strcmp(line, "CAL,STATUS"))
        {
            imu_protocol_send_cal_state(p_ctx);
        }
        else if (0 == strncmp(line, "GRIPCFG,", 8))
        {
            imu_protocol_handle_gripcfg(p_ctx, &line[8]);
        }
    }
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

static void imu_protocol_handle_gripcfg(imu_app_context_t * p_ctx, char * p_args)
{
    arm_emg_servo0_cfg_t cfg;
    char *               value_text;
    char *               end_ptr;
    float                value;

    if ((NULL == p_ctx) || (NULL == p_args) || ('\0' == *p_args))
    {
        return;
    }

    if (0 == strcmp(p_args, "STATUS"))
    {
        arm_get_emg_servo0_config(&cfg);
        imu_protocol_send_textf(p_ctx,
                                "GRIPCFG,STATUS,OPEN,%.2f,CLOSE,%.2f,ENV_ALPHA,%.3f,GRIP_ALPHA,%.3f,"
                                "DEADBAND,%.2f,HOLD,%.2f,SPEED,%.2f,ENV,%.2f,GRIP,%u\r\n",
                                (double) cfg.envelope_open,
                                (double) cfg.envelope_close,
                                (double) cfg.envelope_alpha,
                                (double) cfg.grip_alpha,
                                (double) cfg.grip_deadband_percent,
                                (double) cfg.grip_hold_percent,
                                (double) cfg.servo_speed_step,
                                (double) arm_get_emg_filtered_envelope(),
                                (unsigned int) arm_get_emg_grip_percent());
        return;
    }

    if (0 == strcmp(p_args, "DEFAULTS"))
    {
        arm_reset_emg_servo0_config();
        imu_protocol_send_text(p_ctx, "GRIPCFG,OK,DEFAULTS\r\n");
        return;
    }

    value_text = strchr(p_args, ',');
    if (NULL == value_text)
    {
        imu_protocol_send_text(p_ctx, "GRIPCFG,ERR,FORMAT\r\n");
        return;
    }

    *value_text++ = '\0';
    value = strtof(value_text, &end_ptr);
    if ((end_ptr == value_text) || ('\0' != *end_ptr))
    {
        imu_protocol_send_text(p_ctx, "GRIPCFG,ERR,VALUE\r\n");
        return;
    }

    if (FSP_SUCCESS == arm_set_emg_servo0_param(p_args, value))
    {
        imu_protocol_send_textf(p_ctx, "GRIPCFG,OK,%s,%.3f\r\n", p_args, (double) value);
    }
    else
    {
        imu_protocol_send_textf(p_ctx, "GRIPCFG,ERR,%s,%.3f\r\n", p_args, (double) value);
    }
}
