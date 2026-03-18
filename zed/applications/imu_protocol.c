#include "imu_protocol.h"
#include "drv_uart.h"
#include "imu_calibration.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 文本串口协议层：
 * 用简单的文本帧完成标定提示、状态查询和姿态输出。
 */

/* 这是一个很轻量的串口文本协议，用于标定提示、状态查询和姿态帧输出。 */
static void        imu_ascii_to_upper(char * p_text);
static char const * imu_cal_step_name(imu_cal_step_t step);
static char const * imu_cal_result_name(imu_cal_result_t result);

void imu_protocol_send_text(imu_app_context_t * p_ctx, char const * p_text)
{
    /* 上层所有串口输出最终都会走到这里，统一做发送和发送完成等待。 */
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
        imu_protocol_send_text(p_ctx, frame);
    }
}

void imu_protocol_send_cal_step(imu_app_context_t * p_ctx)
{
    /* 告诉上位机或串口助手，当前应该采集哪一个标定姿态。 */
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
    /* 用紧凑的 CSV 文本格式输出当前舵机目标角，方便上位机直接解析。 */
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
        imu_protocol_send_text(p_ctx, frame);
    }
}

void imu_protocol_handle_uart_commands(imu_app_context_t * p_ctx, uint32_t now_us)
{
    /* 命令集合刻意保持很小，方便直接用串口助手手工输入调试。 */
    char line[IMU_UART_LINE_MAX_LEN] = {0};

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
    }
}

static void imu_ascii_to_upper(char * p_text)
{
    /* 命令大小写不敏感，减少手工输入时因为大小写导致的调试摩擦。 */
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
