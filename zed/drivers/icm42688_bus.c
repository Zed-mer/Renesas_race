#include "icm42688_bus.h"
#include "icm42688_regs.h"

#define ICM42688_SPI_TIMEOUT_LOOPS  100000U
#define ICM42688_SPI_READ_MASK      0x80U
#define ICM42688_SPI_DUMMY_BYTE     0xFFU

static fsp_err_t icm42688_transfer_byte(icm42688_bus_t const * p_bus, uint8_t tx_data, uint8_t * p_rx_data);
static fsp_err_t icm42688_write_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t val);
static fsp_err_t icm42688_read_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_reg_val);
static fsp_err_t icm42688_read_regs(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_buf, uint16_t len);

fsp_err_t icm42688_bus_init(icm42688_bus_t const * p_bus)
{
    fsp_err_t err;
    uint8_t   who_am_i = 0U;

    err = p_bus->p_spi->p_api->open(p_bus->p_spi->p_ctrl, p_bus->p_spi->p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_REG_BANK_SEL, 0x00U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_DEVICE_CONFIG, 0x01U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

    err = icm42688_read_reg(p_bus, ICM42688_WHO_AM_I, &who_am_i);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (ICM42688_ID != who_am_i)
    {
        return FSP_ERR_NOT_FOUND;
    }

    err = icm42688_write_reg(p_bus, ICM42688_INT_CONFIG, 0x03U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_INT_CONFIG1, 0x00U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_INT_SOURCE0, 0x08U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_ACCEL_CONFIG0, (uint8_t) ((AFS_4G << 5) | AODR_500Hz));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_GYRO_CONFIG0, (uint8_t) ((GFS_1000DPS << 5) | GODR_500Hz));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = icm42688_write_reg(p_bus, ICM42688_PWR_MGMT0, 0x0FU);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

fsp_err_t icm42688_bus_get_raw_data(icm42688_bus_t const * p_bus,
                                    int16_t * p_temp_raw,
                                    icm42688RawData_t * p_acc_data,
                                    icm42688RawData_t * p_gyro_data)
{
    fsp_err_t err;
    uint8_t   buf[14] = {0};

    err = icm42688_read_regs(p_bus, ICM42688_TEMP_DATA1, buf, sizeof(buf));
    if (FSP_SUCCESS != err)
    {
        if (NULL != p_temp_raw)
        {
            *p_temp_raw = 0;
        }

        if (NULL != p_acc_data)
        {
            p_acc_data->x = 0;
            p_acc_data->y = 0;
            p_acc_data->z = 0;
        }

        if (NULL != p_gyro_data)
        {
            p_gyro_data->x = 0;
            p_gyro_data->y = 0;
            p_gyro_data->z = 0;
        }

        return err;
    }

    if (NULL != p_temp_raw)
    {
        *p_temp_raw = (int16_t) ((buf[0] << 8) | buf[1]);
    }

    if (NULL != p_acc_data)
    {
        p_acc_data->x = (int16_t) ((buf[2] << 8) | buf[3]);
        p_acc_data->y = (int16_t) ((buf[4] << 8) | buf[5]);
        p_acc_data->z = (int16_t) ((buf[6] << 8) | buf[7]);
    }

    if (NULL != p_gyro_data)
    {
        p_gyro_data->x = (int16_t) ((buf[8] << 8) | buf[9]);
        p_gyro_data->y = (int16_t) ((buf[10] << 8) | buf[11]);
        p_gyro_data->z = (int16_t) ((buf[12] << 8) | buf[13]);
    }

    return FSP_SUCCESS;
}

static fsp_err_t icm42688_transfer_byte(icm42688_bus_t const * p_bus, uint8_t tx_data, uint8_t * p_rx_data)
{
    fsp_err_t err;
    uint8_t   rx_data = 0U;
    uint32_t  timeout = ICM42688_SPI_TIMEOUT_LOOPS;

    *(p_bus->p_transfer_complete) = false;
    err = p_bus->p_spi->p_api->writeRead(p_bus->p_spi->p_ctrl, &tx_data, &rx_data, 1U, SPI_BIT_WIDTH_8_BITS);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    while ((!*(p_bus->p_transfer_complete)) && (timeout > 0U))
    {
        timeout--;
    }

    if (0U == timeout)
    {
        return FSP_ERR_TIMEOUT;
    }

    if (NULL != p_rx_data)
    {
        *p_rx_data = rx_data;
    }

    return FSP_SUCCESS;
}

static fsp_err_t icm42688_write_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t val)
{
    fsp_err_t err;

    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_LOW);
    err = icm42688_transfer_byte(p_bus, reg, NULL);
    if (FSP_SUCCESS == err)
    {
        err = icm42688_transfer_byte(p_bus, val, NULL);
    }
    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_HIGH);

    return err;
}

static fsp_err_t icm42688_read_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_reg_val)
{
    fsp_err_t err;
    uint8_t   reg_val = 0U;

    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_LOW);
    err = icm42688_transfer_byte(p_bus, (uint8_t) (reg | ICM42688_SPI_READ_MASK), NULL);
    if (FSP_SUCCESS == err)
    {
        err = icm42688_transfer_byte(p_bus, ICM42688_SPI_DUMMY_BYTE, &reg_val);
    }
    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_HIGH);

    if ((FSP_SUCCESS == err) && (NULL != p_reg_val))
    {
        *p_reg_val = reg_val;
    }

    return err;
}

static fsp_err_t icm42688_read_regs(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_buf, uint16_t len)
{
    fsp_err_t err;

    if ((NULL == p_buf) || (0U == len))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_LOW);
    err = icm42688_transfer_byte(p_bus, (uint8_t) (reg | ICM42688_SPI_READ_MASK), NULL);

    while ((FSP_SUCCESS == err) && (len > 0U))
    {
        err = icm42688_transfer_byte(p_bus, ICM42688_SPI_DUMMY_BYTE, p_buf);
        p_buf++;
        len--;
    }

    R_IOPORT_PinWrite(&g_ioport_ctrl, p_bus->cs_pin, BSP_IO_LEVEL_HIGH);

    return err;
}
