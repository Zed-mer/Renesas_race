#include "gt911_touch.h"
#include <string.h>

#define GT911_DIAG_MAGIC             (0x47543931U)
#define GT911_ADDRESS_PRIMARY        (0x14U)
#define GT911_ADDRESS_FALLBACK       (0x5DU)
#define GT911_REG_COMMAND            (0x8040U)
#define GT911_REG_CONFIG_RESOLUTION  (0x8048U)
#define GT911_REG_PRODUCT_ID         (0x8140U)
#define GT911_REG_FIRMWARE_VERSION   (0x8144U)
#define GT911_REG_TOUCH_STATUS       (0x814EU)
#define GT911_TOUCH_READY            (0x80U)
#define GT911_TOUCH_COUNT_MASK       (0x0FU)
#define GT911_POINT_BYTES            (8U)
#define GT911_TOUCH_RESET            (BSP_IO_PORT_00_PIN_13)
#define GT911_TOUCH_INTERRUPT        (BSP_IO_PORT_04_PIN_12)
#define GT911_I2C_EVENT_PENDING      (UINT32_MAX)
#define GT911_I2C_WAIT_STEPS         (2000U)
#define GT911_I2C_WAIT_STEP_US       (10U)

volatile gt911_diag_t g_gt911_diag;

static volatile uint32_t g_touch_i2c_event = GT911_I2C_EVENT_PENDING;

static fsp_err_t gt911_wait_for_event(i2c_master_event_t expected)
{
    for (uint32_t step = 0U; step < GT911_I2C_WAIT_STEPS; step++)
    {
        const uint32_t event = g_touch_i2c_event;
        if ((uint32_t) expected == event)
        {
            return FSP_SUCCESS;
        }
        if ((uint32_t) I2C_MASTER_EVENT_ABORTED == event)
        {
            return FSP_ERR_ABORTED;
        }
        R_BSP_SoftwareDelay(GT911_I2C_WAIT_STEP_US, BSP_DELAY_UNITS_MICROSECONDS);
    }

    (void) R_IIC_MASTER_Abort(&g_touch_i2c_ctrl);
    return FSP_ERR_TIMEOUT;
}

static fsp_err_t gt911_transfer_write(uint8_t * p_data, uint32_t length)
{
    g_touch_i2c_event = GT911_I2C_EVENT_PENDING;
    fsp_err_t err = R_IIC_MASTER_Write(&g_touch_i2c_ctrl, p_data, length, false);
    if (FSP_SUCCESS == err)
    {
        err = gt911_wait_for_event(I2C_MASTER_EVENT_TX_COMPLETE);
    }

    g_gt911_diag.i2c_transfers++;
    if (FSP_SUCCESS != err)
    {
        g_gt911_diag.i2c_errors++;
        g_gt911_diag.last_error = (uint32_t) err;
    }
    return err;
}

static fsp_err_t gt911_transfer_read(uint8_t * p_data, uint32_t length)
{
    g_touch_i2c_event = GT911_I2C_EVENT_PENDING;
    fsp_err_t err = R_IIC_MASTER_Read(&g_touch_i2c_ctrl, p_data, length, false);
    if (FSP_SUCCESS == err)
    {
        err = gt911_wait_for_event(I2C_MASTER_EVENT_RX_COMPLETE);
    }

    g_gt911_diag.i2c_transfers++;
    if (FSP_SUCCESS != err)
    {
        g_gt911_diag.i2c_errors++;
        g_gt911_diag.last_error = (uint32_t) err;
    }
    return err;
}

static fsp_err_t gt911_read(uint16_t reg, uint8_t * p_data, uint32_t length)
{
    uint8_t address[2] = {(uint8_t) (reg >> 8U), (uint8_t) reg};
    fsp_err_t err = gt911_transfer_write(address, sizeof(address));
    if (FSP_SUCCESS == err)
    {
        err = gt911_transfer_read(p_data, length);
    }
    return err;
}

static fsp_err_t gt911_write_u8(uint16_t reg, uint8_t value)
{
    uint8_t data[3] = {(uint8_t) (reg >> 8U), (uint8_t) reg, value};
    return gt911_transfer_write(data, sizeof(data));
}

static fsp_err_t gt911_hardware_reset(uint8_t address)
{
    const bsp_io_level_t address_level =
        (GT911_ADDRESS_PRIMARY == address) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;
    uint32_t interrupt_cfg = (uint32_t) IOPORT_CFG_DRIVE_MID |
                             (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT;
    interrupt_cfg |= (BSP_IO_LEVEL_HIGH == address_level) ?
                     (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH : (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW;

    fsp_err_t err = R_IOPORT_PinCfg(g_ioport.p_ctrl,
                                    GT911_TOUCH_RESET,
                                    (uint32_t) IOPORT_CFG_DRIVE_MID |
                                    (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                                    (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS == err)
    {
        err = R_IOPORT_PinCfg(g_ioport.p_ctrl, GT911_TOUCH_INTERRUPT, interrupt_cfg);
    }
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay(200U, BSP_DELAY_UNITS_MICROSECONDS);
    err = R_IOPORT_PinWrite(g_ioport.p_ctrl, GT911_TOUCH_RESET, BSP_IO_LEVEL_HIGH);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

    (void) R_IOPORT_PinWrite(g_ioport.p_ctrl, GT911_TOUCH_INTERRUPT, BSP_IO_LEVEL_LOW);
    R_BSP_SoftwareDelay(50U, BSP_DELAY_UNITS_MILLISECONDS);
    err = R_IOPORT_PinCfg(g_ioport.p_ctrl,
                          GT911_TOUCH_INTERRUPT,
                          (uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT |
                          (uint32_t) IOPORT_CFG_PULLUP_ENABLE);
    if (FSP_SUCCESS == err)
    {
        g_gt911_diag.resets++;
    }
    return err;
}

static fsp_err_t gt911_probe_address(uint8_t address, uint8_t product_id[4])
{
    fsp_err_t err = gt911_hardware_reset(address);
    if (FSP_SUCCESS == err)
    {
        err = R_IIC_MASTER_SlaveAddressSet(&g_touch_i2c_ctrl,
                                           address,
                                           I2C_MASTER_ADDR_MODE_7BIT);
    }
    if (FSP_SUCCESS == err)
    {
        err = gt911_read(GT911_REG_PRODUCT_ID, product_id, 4U);
    }
    if ((FSP_SUCCESS == err) && ('9' != product_id[0]))
    {
        err = FSP_ERR_NOT_FOUND;
    }
    if (FSP_SUCCESS == err)
    {
        g_gt911_diag.i2c_address = address;
    }
    return err;
}

fsp_err_t gt911_touch_init(void)
{
    memset((void *) &g_gt911_diag, 0, sizeof(g_gt911_diag));
    g_gt911_diag.magic = GT911_DIAG_MAGIC;

    fsp_err_t err = R_IIC_MASTER_Open(&g_touch_i2c_ctrl, &g_touch_i2c_cfg);
    if (FSP_SUCCESS != err)
    {
        g_gt911_diag.init_error = (uint32_t) err;
        g_gt911_diag.last_error = (uint32_t) err;
        return err;
    }

    uint8_t product_id[4] = {0U};
    err = gt911_probe_address(GT911_ADDRESS_PRIMARY, product_id);
    if (FSP_SUCCESS != err)
    {
        (void) R_IIC_MASTER_Abort(&g_touch_i2c_ctrl);
        memset(product_id, 0, sizeof(product_id));
        err = gt911_probe_address(GT911_ADDRESS_FALLBACK, product_id);
    }
    if (FSP_SUCCESS != err)
    {
        g_gt911_diag.init_error = (uint32_t) err;
        g_gt911_diag.last_error = (uint32_t) err;
        return err;
    }

    g_gt911_diag.product_id = ((uint32_t) product_id[0]) |
                              ((uint32_t) product_id[1] << 8U) |
                              ((uint32_t) product_id[2] << 16U) |
                              ((uint32_t) product_id[3] << 24U);

    uint8_t firmware[2] = {0U};
    err = gt911_read(GT911_REG_FIRMWARE_VERSION, firmware, sizeof(firmware));
    if (FSP_SUCCESS == err)
    {
        g_gt911_diag.firmware_version = (uint32_t) firmware[0] | ((uint32_t) firmware[1] << 8U);
    }

    uint8_t resolution[4] = {0U};
    if (FSP_SUCCESS == err)
    {
        err = gt911_read(GT911_REG_CONFIG_RESOLUTION, resolution, sizeof(resolution));
    }
    if (FSP_SUCCESS == err)
    {
        g_gt911_diag.config_x_max = (uint32_t) resolution[0] | ((uint32_t) resolution[1] << 8U);
        g_gt911_diag.config_y_max = (uint32_t) resolution[2] | ((uint32_t) resolution[3] << 8U);
        err = gt911_write_u8(GT911_REG_COMMAND, 0U);
    }

    g_gt911_diag.init_error = (uint32_t) err;
    g_gt911_diag.last_error = (uint32_t) err;
    g_gt911_diag.initialized = (FSP_SUCCESS == err) ? 1U : 0U;
    return err;
}

fsp_err_t gt911_touch_poll(gt911_sample_t * p_sample)
{
    if (NULL == p_sample)
    {
        return FSP_ERR_ASSERTION;
    }
    memset(p_sample, 0, sizeof(*p_sample));
    if (0U == g_gt911_diag.initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    uint8_t status_payload[1U] = {0U};
    g_gt911_diag.polls++;
    fsp_err_t err = gt911_read(GT911_REG_TOUCH_STATUS, status_payload, sizeof(status_payload));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    const uint8_t status = status_payload[0];
    g_gt911_diag.last_status = status;
    if (0U == (status & GT911_TOUCH_READY))
    {
        return FSP_SUCCESS;
    }

    const uint8_t count = status & GT911_TOUCH_COUNT_MASK;
    g_gt911_diag.ready_frames++;
    p_sample->updated = true;
    p_sample->count = count;
    if (count > GT911_MAX_TOUCHES)
    {
        p_sample->count = 0U;
        err = FSP_ERR_INVALID_DATA;
    }
    else
    {
        uint8_t point_payload[GT911_MAX_TOUCHES * GT911_POINT_BYTES] = {0U};
        if (count > 0U)
        {
            err = gt911_read((uint16_t) (GT911_REG_TOUCH_STATUS + 1U),
                             point_payload,
                             (uint32_t) count * GT911_POINT_BYTES);
        }

        if (FSP_SUCCESS != err)
        {
            g_gt911_diag.last_error = (uint32_t) err;
            return err;
        }

        for (uint32_t index = 0U; index < count; index++)
        {
            const uint32_t offset = index * GT911_POINT_BYTES;
            p_sample->points[index].track_id = point_payload[offset];
            p_sample->points[index].x = (uint16_t) point_payload[offset + 1U] |
                                        (uint16_t) ((uint16_t) point_payload[offset + 2U] << 8U);
            p_sample->points[index].y = (uint16_t) point_payload[offset + 3U] |
                                        (uint16_t) ((uint16_t) point_payload[offset + 4U] << 8U);
            p_sample->points[index].size = (uint16_t) point_payload[offset + 5U] |
                                           (uint16_t) ((uint16_t) point_payload[offset + 6U] << 8U);
        }
    }

    const fsp_err_t clear_err = gt911_write_u8(GT911_REG_TOUCH_STATUS, 0U);
    if (FSP_SUCCESS == err)
    {
        err = clear_err;
    }

    g_gt911_diag.last_touch_count = p_sample->count;
    if (p_sample->count > 0U)
    {
        g_gt911_diag.touch_frames++;
        g_gt911_diag.last_track_id = p_sample->points[0].track_id;
        g_gt911_diag.last_x = p_sample->points[0].x;
        g_gt911_diag.last_y = p_sample->points[0].y;
        g_gt911_diag.last_size = p_sample->points[0].size;
    }
    g_gt911_diag.last_error = (uint32_t) err;
    return err;
}

void touch_i2c_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL != p_args)
    {
        g_touch_i2c_event = (uint32_t) p_args->event;
        g_gt911_diag.last_i2c_event = (uint32_t) p_args->event;
    }
}
