#include "jd9165_panel.h"
#include "display_bringup.h"
#include "r_mipi_dsi.h"
#include <string.h>

#define JD9165_MAX_PARAMETERS       (14U)
#define JD9165_COMMAND_TIMEOUT_MS   (1000U)
#define JD9165_SHUTDOWN_COMMAND_TIMEOUT_MS (100U)
#define JD9165_DISPLAY_OFF          (0x28U)
#define JD9165_SLEEP_IN             (0x10U)
#define JD9165_DISPLAY_OFF_WAIT_MS  (40U)
#define JD9165_READ_DSI_ERRORS      (0x05U)
#define JD9165_READ_POWER_MODE      (0x0AU)
#define JD9165_TWO_LANE_1024_X_600  (0x11U)
#define JD9165_TX_ERROR_MASK        ((uint32_t) MIPI_DSI_SEQUENCE_STATUS_DESCRIPTOR_ABORT | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_SIZE_ERROR | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_TX_INTERNAL_BUS_ERROR | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_RX_FATAL_ERROR | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_RX_FAIL | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_RX_PACKET_DATA_FAIL | \
                                     (uint32_t) MIPI_DSI_SEQUENCE_STATUS_RX_ACK_AND_ERROR)
#define JD9165_RX_ERROR_MASK        ((uint32_t) MIPI_DSI_RECEIVE_STATUS_LP_RX_HOST_TIMEOUT | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_BTA_ACK_TIMEOUT | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_MALFORM_ERROR | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_ECC_MULTI | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_UNEXPECTED_PACKET | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_WORD_COUNT | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_CRC | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_INTERNAL_BUS | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_BUFFER_OVERFLOW | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_TIMEOUT | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_NO_RESPONSE | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_PACKET_SIZE | \
                                     (uint32_t) MIPI_DSI_RECEIVE_STATUS_ACK_AND_ERROR)

typedef struct st_jd9165_command
{
    uint8_t command;
    uint8_t parameters[JD9165_MAX_PARAMETERS];
    uint8_t parameter_count;
    uint8_t delay_ms;
} jd9165_command_t;

/* Supplier W700BH018I-30Z/JD9165BA sequence, preserved byte-for-byte. */
static const jd9165_command_t g_jd9165_init[] =
{
    {0x30, {0x00}, 1, 0},
    {0xF7, {0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, {0x01}, 1, 0},
    {0x04, {0x0C}, 1, 0},
    {0x05, {0x08}, 1, 0},
    {0x0B, {JD9165_TWO_LANE_1024_X_600}, 1, 0},
    {0x20, {0x04}, 1, 0},
    {0x1F, {0x00}, 1, 0},
    {0x23, {0x38}, 1, 0},
    {0x28, {0x18}, 1, 0},
    {0x29, {0x29}, 1, 0},
    {0x2A, {0x01}, 1, 0},
    {0x2B, {0x29}, 1, 0},
    {0x2C, {0x01}, 1, 0},
    {0x30, {0x02}, 1, 0},
    {0x00, {0x05}, 1, 0},
    {0x01, {0x22}, 1, 0},
    {0x02, {0x08}, 1, 0},
    {0x03, {0x12}, 1, 0},
    {0x04, {0x16}, 1, 0},
    {0x05, {0x64}, 1, 0},
    {0x06, {0x00}, 1, 0},
    {0x07, {0x00}, 1, 0},
    {0x08, {0x78}, 1, 0},
    {0x09, {0x00}, 1, 0},
    {0x0A, {0x04}, 1, 0},
    {0x0B, {0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09}, 11, 0},
    {0x0C, {0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, {0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08}, 11, 0},
    {0x0E, {0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, {0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F}, 11, 0},
    {0x10, {0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, {0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E}, 11, 0},
    {0x12, {0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x13, {0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x14, {0x00, 0x00, 0x41, 0x41}, 4, 0},
    {0x15, {0x00, 0x00, 0x00, 0x00}, 4, 0},
    {0x17, {0x00}, 1, 0},
    {0x18, {0x85}, 1, 0},
    {0x19, {0x06, 0x09}, 2, 0},
    {0x1A, {0x05, 0x08}, 2, 0},
    {0x1B, {0x0A, 0x04}, 2, 0},
    {0x26, {0x00}, 1, 0},
    {0x27, {0x00}, 1, 0},
    {0x30, {0x06}, 1, 0},
    {0x12, {0x3F, 0x26, 0x27, 0x35, 0x2D, 0x34, 0x3F, 0x3F, 0x3F, 0x35, 0x2A, 0x20, 0x16, 0x08}, 14, 0},
    {0x13, {0x3F, 0x26, 0x28, 0x35, 0x27, 0x29, 0x29, 0x2F, 0x35, 0x2F, 0x26, 0x20, 0x16, 0x08}, 14, 0},
    {0x30, {0x0A}, 1, 0},
    {0x02, {0x4F}, 1, 0},
    {0x0B, {0x40}, 1, 0},
    {0x30, {0x0D}, 1, 0},
    {0x0D, {0x04}, 1, 0},
    {0x10, {0x0C}, 1, 0},
    {0x11, {0x0C}, 1, 0},
    {0x12, {0x0C}, 1, 0},
    {0x13, {0x0C}, 1, 0},
    {0x30, {0x00}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x11, {0x00}, 0, 120},
    {0x29, {0x00}, 0, 20},
};

static uint8_t g_dsi_tx_buffer[JD9165_MAX_PARAMETERS + 1U]
    BSP_ALIGN_VARIABLE(32) BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".ram_noinit_nocache");
static uint8_t g_dsi_rx_buffer[4]
    BSP_ALIGN_VARIABLE(32) BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".ram_noinit_nocache");
static volatile bool g_command_done;
static volatile bool g_receive_done;
static volatile bool g_read_failed;
static volatile bool g_read_in_progress;
static volatile uint8_t g_read_value;

static fsp_err_t jd9165_send_with_flags(uint8_t command,
                                         const uint8_t * p_parameters,
                                         uint8_t parameter_count,
                                         mipi_dsi_cmd_flag_t flags,
                                         uint32_t timeout_ms)
{
    mipi_dsi_cmd_t message = {0};

    g_dsi_tx_buffer[0] = command;
    if (parameter_count > 0U)
    {
        memcpy(&g_dsi_tx_buffer[1], p_parameters, parameter_count);
    }

    message.channel = 0;
    message.flags = flags;
    message.tx_len = (uint16_t) parameter_count + 1U;
    message.p_tx_buffer = g_dsi_tx_buffer;
    if (0U == parameter_count)
    {
        message.cmd_id = (mipi_cmd_id_t) MIPI_DSI_CMD_ID_DCS_SHORT_WRITE_0_PARAM;
    }
    else if (1U == parameter_count)
    {
        message.cmd_id = (mipi_cmd_id_t) MIPI_DSI_CMD_ID_DCS_SHORT_WRITE_1_PARAM;
    }
    else
    {
        message.cmd_id = (mipi_cmd_id_t) MIPI_DSI_CMD_ID_DCS_LONG_WRITE;
    }

    display_startup_diag_note_first_dsi_command(command);
    g_command_done = false;
    fsp_err_t err = R_MIPI_DSI_Command(&g_mipi_dsi0_ctrl, &message);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    for (uint32_t elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms++)
    {
        if (g_command_done)
        {
            g_display_diag.commands_sent++;
            return FSP_SUCCESS;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return FSP_ERR_TIMEOUT;
}

static fsp_err_t jd9165_send(uint8_t command,
                             const uint8_t * p_parameters,
                             uint8_t parameter_count)
{
    return jd9165_send_with_flags(command, p_parameters, parameter_count,
                                   MIPI_DSI_CMD_FLAG_LOW_POWER,
                                   JD9165_COMMAND_TIMEOUT_MS);
}

fsp_err_t jd9165_panel_shutdown_commands(bool high_speed)
{
    const mipi_dsi_cmd_flag_t flags = high_speed ?
        MIPI_DSI_CMD_FLAG_NONE : MIPI_DSI_CMD_FLAG_LOW_POWER;
    fsp_err_t err = jd9165_send_with_flags(
        JD9165_DISPLAY_OFF, NULL, 0U, flags,
        JD9165_SHUTDOWN_COMMAND_TIMEOUT_MS);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    R_BSP_SoftwareDelay(JD9165_DISPLAY_OFF_WAIT_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);
    return jd9165_send_with_flags(JD9165_SLEEP_IN, NULL, 0U, flags,
                                   JD9165_SHUTDOWN_COMMAND_TIMEOUT_MS);
}

fsp_err_t jd9165_panel_configure(void)
{
    for (uint32_t i = 0; i < (sizeof(g_jd9165_init) / sizeof(g_jd9165_init[0])); i++)
    {
        g_display_diag.last_command_index = i;
        fsp_err_t err = jd9165_send(g_jd9165_init[i].command,
                                    g_jd9165_init[i].parameters,
                                    g_jd9165_init[i].parameter_count);
        if (FSP_SUCCESS != err)
        {
            return err;
        }
        if (g_jd9165_init[i].delay_ms > 0U)
        {
            R_BSP_SoftwareDelay(g_jd9165_init[i].delay_ms, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    return FSP_SUCCESS;
}

static fsp_err_t jd9165_read(uint8_t command, uint8_t * p_value)
{
    mipi_dsi_cmd_t message = {0};
    mipi_dsi_status_t status = {0};

    g_dsi_tx_buffer[0] = command;
    memset(g_dsi_rx_buffer, 0xA5, sizeof(g_dsi_rx_buffer));

    message.channel = 0;
    message.cmd_id = (mipi_cmd_id_t) MIPI_DSI_CMD_ID_DCS_READ;
    message.flags = (mipi_dsi_cmd_flag_t) (MIPI_DSI_CMD_FLAG_LOW_POWER | MIPI_DSI_CMD_FLAG_BTA_READ);
    message.tx_len = 1U;
    message.p_tx_buffer = g_dsi_tx_buffer;
    message.p_rx_buffer = g_dsi_rx_buffer;

    g_display_diag.panel_read_tx_status = 0U;
    g_display_diag.panel_receive_status = 0U;
    g_display_diag.panel_read_result = 0U;
    g_display_diag.panel_read_buffer = 0U;
    g_display_diag.panel_receive_events = 0U;
    g_command_done = false;
    g_receive_done = false;
    g_read_failed = false;
    g_read_in_progress = true;
    g_read_value = 0U;

    fsp_err_t err = R_MIPI_DSI_Command(&g_mipi_dsi0_ctrl, &message);
    if (FSP_SUCCESS == err)
    {
        err = FSP_ERR_TIMEOUT;
        for (uint32_t elapsed_ms = 0; elapsed_ms < JD9165_COMMAND_TIMEOUT_MS; elapsed_ms++)
        {
            if (g_read_failed)
            {
                err = FSP_ERR_ABORTED;
                break;
            }
            if (g_command_done && g_receive_done)
            {
                err = FSP_SUCCESS;
                break;
            }
            R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    g_read_in_progress = false;
    g_display_diag.panel_read_buffer =
        ((uint32_t) g_dsi_rx_buffer[0]) |
        ((uint32_t) g_dsi_rx_buffer[1] << 8U) |
        ((uint32_t) g_dsi_rx_buffer[2] << 16U) |
        ((uint32_t) g_dsi_rx_buffer[3] << 24U);

    if (FSP_SUCCESS == R_MIPI_DSI_StatusGet(&g_mipi_dsi0_ctrl, &status))
    {
        g_display_diag.dsi_link_status = (uint32_t) status.link_status;
        g_display_diag.dsi_ack_latest = status.ack_err_latest.bits;
        g_display_diag.dsi_ack_accumulated = status.ack_err_accumulated.bits;
    }

    if ((FSP_SUCCESS == err) && (NULL != p_value))
    {
        *p_value = g_read_value;
    }

    return err;
}

fsp_err_t jd9165_panel_disable_bist(void)
{
    static const uint8_t page_1 = 0x01U;
    static const uint8_t register_control = 0x09U;
    static const uint8_t normal_video_mode = 0x00U;
    static const uint8_t page_0 = 0x00U;

    fsp_err_t err = jd9165_send(0x30U, &page_1, 1U);
    if (FSP_SUCCESS == err)
    {
        /* Select register control so the BIST_EN pin cannot select the test generator. */
        err = jd9165_send(0x05U, &register_control, 1U);
    }
    if (FSP_SUCCESS == err)
    {
        err = jd9165_send(0x06U, &normal_video_mode, 1U);
    }
    if (FSP_SUCCESS == err)
    {
        err = jd9165_send(0x30U, &page_0, 1U);
    }

    return err;
}

fsp_err_t jd9165_panel_read_power_mode(void)
{
    uint8_t power_mode = 0U;
    g_display_diag.panel_power_mode = 0U;
    fsp_err_t err = jd9165_read(JD9165_READ_POWER_MODE, &power_mode);
    if (FSP_SUCCESS == err)
    {
        g_display_diag.panel_power_mode = power_mode;
    }

    return err;
}

fsp_err_t jd9165_panel_read_dsi_error_count(uint8_t * p_error_count)
{
    return jd9165_read(JD9165_READ_DSI_ERRORS, p_error_count);
}

fsp_err_t jd9165_panel_read_lane_config(uint8_t * p_lane_config, uint8_t * p_lane_control)
{
    static const uint8_t page_1 = 0x01U;
    static const uint8_t page_0 = 0x00U;

    fsp_err_t err = jd9165_send(0x30U, &page_1, 1U);
    if (FSP_SUCCESS == err)
    {
        err = jd9165_read(0x0BU, p_lane_config);
    }
    if (FSP_SUCCESS == err)
    {
        err = jd9165_read(0x20U, p_lane_control);
    }

    fsp_err_t page_err = jd9165_send(0x30U, &page_0, 1U);
    if (FSP_SUCCESS == err)
    {
        err = page_err;
    }

    return err;
}

fsp_err_t jd9165_panel_enable_bist(void)
{
    static const uint8_t page_1 = 0x01U;
    static const uint8_t bist_source_register = 0x09U;
    static const uint8_t bist_enabled = 0x40U;
    static const uint8_t page_0 = 0x00U;

    fsp_err_t err = jd9165_send(0x30U, &page_1, 1U);
    if (FSP_SUCCESS == err)
    {
        /* Preserve the supplier's R05 bit 3 while selecting register-controlled BIST. */
        err = jd9165_send(0x05U, &bist_source_register, 1U);
    }
    if (FSP_SUCCESS == err)
    {
        err = jd9165_send(0x06U, &bist_enabled, 1U);
    }
    if (FSP_SUCCESS == err)
    {
        err = jd9165_send(0x30U, &page_0, 1U);
    }

    return err;
}

void mipi_dsi_callback(mipi_dsi_callback_args_t * p_args)
{
    switch (p_args->event)
    {
        case MIPI_DSI_EVENT_SEQUENCE_0:
            if (g_read_in_progress)
            {
                g_display_diag.panel_read_tx_status |= (uint32_t) p_args->tx_status;
            }
            else
            {
                g_display_diag.last_tx_status = (uint32_t) p_args->tx_status;
            }
            if (0U != ((uint32_t) p_args->tx_status & (uint32_t) MIPI_DSI_SEQUENCE_STATUS_DESCRIPTORS_FINISHED))
            {
                if (!g_read_in_progress)
                {
                    g_display_diag.command_callbacks++;
                }
                g_command_done = true;
            }
            if (g_read_in_progress && (0U != ((uint32_t) p_args->tx_status & JD9165_TX_ERROR_MASK)))
            {
                g_read_failed = true;
            }
            break;

        case MIPI_DSI_EVENT_RECEIVE:
            g_display_diag.panel_receive_events++;
            g_display_diag.panel_receive_status |= (uint32_t) p_args->rx_status;
            if (NULL != p_args->p_result)
            {
                mipi_dsi_receive_result_t const * p_result = p_args->p_result;
                g_display_diag.panel_read_result =
                    ((uint32_t) p_result->data[0]) |
                    ((uint32_t) p_result->data[1] << 8U) |
                    ((uint32_t) p_result->cmd_id << 16U) |
                    ((uint32_t) p_result->virtual_channel_id << 22U) |
                    ((uint32_t) p_result->long_packet << 24U) |
                    ((uint32_t) p_result->rx_success << 25U) |
                    ((uint32_t) p_result->timeout << 26U) |
                    ((uint32_t) p_result->rx_fail << 27U) |
                    ((uint32_t) p_result->rx_data_fail << 28U) |
                    ((uint32_t) p_result->rx_correctable_error << 29U) |
                    ((uint32_t) p_result->rx_ack_err << 30U) |
                    ((uint32_t) p_result->info_overwrite << 31U);
                if (0U != p_result->rx_success)
                {
                    g_read_value = p_result->data[0];
                }
                if ((0U != p_result->timeout) || (0U != p_result->rx_fail) ||
                    (0U != p_result->rx_data_fail) || (0U != p_result->rx_ack_err))
                {
                    g_read_failed = true;
                }
            }
            if (0U != ((uint32_t) p_args->rx_status & JD9165_RX_ERROR_MASK))
            {
                g_read_failed = true;
                g_receive_done = true;
            }
            if (0U != ((uint32_t) p_args->rx_status & (uint32_t) MIPI_DSI_RECEIVE_STATUS_RESPONSE_PACKET))
            {
                g_receive_done = true;
            }
            break;

        case MIPI_DSI_EVENT_VIDEO:
            g_display_diag.video_status |= (uint32_t) p_args->video_status;
            break;

        case MIPI_DSI_EVENT_FATAL:
            g_display_diag.fatal_status |= (uint32_t) p_args->fatal_status;
            if (g_read_in_progress)
            {
                g_read_failed = true;
                g_receive_done = true;
            }
            break;

        case MIPI_DSI_EVENT_PHY:
            g_display_diag.phy_status |= (uint32_t) p_args->phy_status;
            break;

        default:
            break;
    }
}
