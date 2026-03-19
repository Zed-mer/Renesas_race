#include "imu_protocol.h"
#include "drv_uart.h"
#include "emg_runtime.h"
#include "imu_calibration.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * EMG 调参帧比原来的 POSE / CAL 更长，因此这里把临时格式化缓冲区放大，
 * 避免 EMGDBG 在数字位数稍长时被截断。
 */
#define IMU_PROTOCOL_FRAME_BUFFER_SIZE  192U

static void        imu_ascii_to_upper(char * p_text);
static char const * imu_cal_step_name(imu_cal_step_t step);
static char const * imu_cal_result_name(imu_cal_result_t result);
#if IMU_EMG_ONLY_TEST && emg_dbg
static bool         imu_protocol_is_emg_command(char const * p_line);
static void         imu_protocol_handle_emg_command(imu_app_context_t * p_ctx, char * p_line);
static void         imu_protocol_send_emg_config_dump(imu_app_context_t * p_ctx);
static void         imu_protocol_send_emg_config_value(imu_app_context_t * p_ctx, char const * p_name, float value);
static void         imu_protocol_send_emg_config_ok(imu_app_context_t * p_ctx, char const * p_name, float value);
static void         imu_protocol_send_emg_config_error(imu_app_context_t * p_ctx, char const * p_name, char const * p_reason);
#endif

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

void imu_protocol_send_emg_debug_frame(imu_app_context_t * p_ctx)
{
    emg_debug_snapshot_t snapshot = {0};

    /*
     * EMGDBG 是专门给调参上位机吃的结构化报文。
     * 固件侧把算法内部关键状态一次性打平发出，
     * 这样前端不需要猜测阈值和内部状态，自然也更容易调参。
     */
    emg_runtime_get_debug_snapshot(&snapshot);
    imu_protocol_send_textf(p_ctx,
                            "EMGDBG,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u\r\n",
                            (unsigned int) snapshot.raw,
                            snapshot.filtered,
                            snapshot.envelope,
                            snapshot.rest,
                            snapshot.peak,
                            snapshot.th_on,
                            snapshot.th_off,
                            (unsigned int) snapshot.grip,
                            snapshot.active ? 1U : 0U);
}

void imu_protocol_handle_uart_commands(imu_app_context_t * p_ctx, uint32_t now_us)
{
    char line[IMU_UART_LINE_MAX_LEN] = {0};

    while (drv_uart_read_line(line, sizeof(line)))
    {
        if ('\0' == line[0])
        {
            continue;
        }

        imu_ascii_to_upper(line);

#if IMU_EMG_ONLY_TEST && emg_dbg
        if (imu_protocol_is_emg_command(line))
        {
            imu_protocol_handle_emg_command(p_ctx, line);
            continue;
        }
#endif

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

/* 只有在肌电调参模式下，才需要识别 EMGCFG 命令前缀。 */
#if IMU_EMG_ONLY_TEST && emg_dbg
static bool imu_protocol_is_emg_command(char const * p_line)
{
    if (NULL == p_line)
    {
        return false;
    }

    return (0 == strncmp(p_line, "EMGCFG,", 7U));
}
#endif

#if IMU_EMG_ONLY_TEST && emg_dbg
static void imu_protocol_handle_emg_command(imu_app_context_t * p_ctx, char * p_line)
{
    char * p_group;
    char * p_action;
    char * p_name;
    char * p_value_text;
    char * p_parse_end = NULL;
    float  value = 0.0f;

    p_group = strtok(p_line, ",");
    p_action = strtok(NULL, ",");
    (void) p_group;

    if (NULL == p_action)
    {
        imu_protocol_send_emg_config_error(p_ctx, "ACTION", "MISSING");
        return;
    }

    if (0 == strcmp(p_action, "GET"))
    {
        imu_protocol_send_emg_config_dump(p_ctx);
        return;
    }

    if (0 == strcmp(p_action, "RESET"))
    {
        emg_runtime_reset_params_to_defaults();
        imu_protocol_send_text(p_ctx, "EMGCFG,OK,RESET,DEFAULTS\r\n");
        imu_protocol_send_emg_config_dump(p_ctx);
        return;
    }

    if (0 != strcmp(p_action, "SET"))
    {
        imu_protocol_send_emg_config_error(p_ctx, "ACTION", "UNKNOWN");
        return;
    }

    p_name = strtok(NULL, ",");
    p_value_text = strtok(NULL, ",");

    if ((NULL == p_name) || (NULL == p_value_text))
    {
        imu_protocol_send_emg_config_error(p_ctx, "SET", "ARGS");
        return;
    }

    value = strtof(p_value_text, &p_parse_end);
    if ((NULL == p_parse_end) || ('\0' != *p_parse_end))
    {
        imu_protocol_send_emg_config_error(p_ctx, p_name, "VALUE");
        return;
    }

    if (FSP_SUCCESS != emg_runtime_set_param(p_name, value))
    {
        imu_protocol_send_emg_config_error(p_ctx, p_name, "RANGE");
        return;
    }

    imu_protocol_send_emg_config_ok(p_ctx, p_name, value);
}

static void imu_protocol_send_emg_config_dump(imu_app_context_t * p_ctx)
{
    emg_tune_params_t params = {0};

    emg_runtime_get_params(&params);
    imu_protocol_send_text(p_ctx, "EMGCFG,BEGIN\r\n");
    /*
     * 当前版本按用户要求把调参入口收敛成“包络窗口大小”一个量。
     * 这样上位机、默认宏和老工程的使用习惯都能保持一致。
     */
    imu_protocol_send_emg_config_value(p_ctx,
                                       "ENVELOPE_WINDOW_SIZE",
                                       (float) params.envelope_window_size);
    imu_protocol_send_text(p_ctx, "EMGCFG,END\r\n");
}

static void imu_protocol_send_emg_config_value(imu_app_context_t * p_ctx, char const * p_name, float value)
{
    imu_protocol_send_textf(p_ctx,
                            "EMGCFG,VALUE,%s,%.6f\r\n",
                            p_name,
                            value);
}

static void imu_protocol_send_emg_config_ok(imu_app_context_t * p_ctx, char const * p_name, float value)
{
    imu_protocol_send_textf(p_ctx,
                            "EMGCFG,OK,%s,%.6f\r\n",
                            p_name,
                            value);
}

static void imu_protocol_send_emg_config_error(imu_app_context_t * p_ctx, char const * p_name, char const * p_reason)
{
    imu_protocol_send_textf(p_ctx,
                            "EMGCFG,ERR,%s,%s\r\n",
                            (NULL != p_name) ? p_name : "UNKNOWN",
                            (NULL != p_reason) ? p_reason : "ERR");
}
#endif
