#include "icm42688.h"
#include <stdbool.h>

static volatile bool g_spi_txc_flag = false;
static float accSensitivity = 0.0f;
static float gyroSensitivity = 0.0f;

#define DEG_TO_RAD (0.01745329251994329577f)

void spi0_callback(spi_callback_args_t * p_args)
{
    if (SPI_EVENT_TRANSFER_COMPLETE == p_args->event)
    {
        g_spi_txc_flag = true;
    }
}

static uint8_t spi_read_write_byte(uint8_t tx_data)
{
    uint8_t rx_data = 0;
    uint32_t timeout = 100000;

    g_spi_txc_flag = false;
    R_SPI_WriteRead(ICM_SPI_INSTANCE.p_ctrl, &tx_data, &rx_data, 1, SPI_BIT_WIDTH_8_BITS);

    while ((!g_spi_txc_flag) && timeout--)
    {
    }

    return rx_data;
}

#define CS_LOW()  R_IOPORT_PinWrite(&g_ioport_ctrl, ICM42688_CS_PIN, BSP_IO_LEVEL_LOW)
#define CS_HIGH() R_IOPORT_PinWrite(&g_ioport_ctrl, ICM42688_CS_PIN, BSP_IO_LEVEL_HIGH)

static void icm42688_write_reg(uint8_t reg, uint8_t val)
{
    CS_LOW();
    spi_read_write_byte(reg);
    spi_read_write_byte(val);
    CS_HIGH();
}

static uint8_t icm42688_read_reg(uint8_t reg)
{
    uint8_t reg_val;

    CS_LOW();
    spi_read_write_byte((uint8_t) (reg | 0x80U));
    reg_val = spi_read_write_byte(0xFFU);
    CS_HIGH();

    return reg_val;
}

static void icm42688_read_regs(uint8_t reg, uint8_t * buf, uint16_t len)
{
    CS_LOW();
    spi_read_write_byte((uint8_t) (reg | 0x80U));

    while (len--)
    {
        *buf++ = spi_read_write_byte(0xFFU);
    }

    CS_HIGH();
}

fsp_err_t bsp_Icm42688Init(void)
{
    fsp_err_t err;
    uint8_t who_am_i = 0;

    err = R_SPI_Open(ICM_SPI_INSTANCE.p_ctrl, ICM_SPI_INSTANCE.p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    icm42688_write_reg(ICM42688_REG_BANK_SEL, 0x00U);
    icm42688_write_reg(ICM42688_DEVICE_CONFIG, 0x01U);
    R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);

    who_am_i = icm42688_read_reg(ICM42688_WHO_AM_I);
    if (who_am_i != ICM42688_ID)
    {
        return FSP_ERR_NOT_FOUND;
    }

    icm42688_write_reg(ICM42688_INT_CONFIG, 0x30U);
    icm42688_write_reg(ICM42688_INT_SOURCE0, 0x08U);
    icm42688_write_reg(ICM42688_ACCEL_CONFIG0, (uint8_t) ((AFS_4G << 5) | AODR_100Hz));
    icm42688_write_reg(ICM42688_GYRO_CONFIG0, (uint8_t) ((GFS_1000DPS << 5) | GODR_100Hz));

    accSensitivity = 4000.0f / 32768.0f;
    gyroSensitivity = 1000.0f / 32768.0f;

    icm42688_write_reg(ICM42688_PWR_MGMT0, 0x2FU);
    R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

void bsp_IcmGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData)
{
    uint8_t buf[12];

    icm42688_read_regs(ICM42688_ACCEL_DATA_X1, buf, 12);

    if (accData)
    {
        accData->x = (int16_t) ((buf[0] << 8) | buf[1]);
        accData->y = (int16_t) ((buf[2] << 8) | buf[3]);
        accData->z = (int16_t) ((buf[4] << 8) | buf[5]);
    }

    if (GyroData)
    {
        GyroData->x = (int16_t) ((buf[6] << 8) | buf[7]);
        GyroData->y = (int16_t) ((buf[8] << 8) | buf[9]);
        GyroData->z = (int16_t) ((buf[10] << 8) | buf[11]);
    }
}

void bsp_IcmGetScaledData(icm42688Float3_t *accData, icm42688Float3_t *gyroData)
{
    icm42688RawData_t raw_acc = {0};
    icm42688RawData_t raw_gyro = {0};

    bsp_IcmGetRawData(&raw_acc, &raw_gyro);

    if (accData)
    {
        accData->x = ((float) raw_acc.x) * accSensitivity / 1000.0f;
        accData->y = ((float) raw_acc.y) * accSensitivity / 1000.0f;
        accData->z = ((float) raw_acc.z) * accSensitivity / 1000.0f;
    }

    if (gyroData)
    {
        gyroData->x = ((float) raw_gyro.x) * gyroSensitivity * DEG_TO_RAD;
        gyroData->y = ((float) raw_gyro.y) * gyroSensitivity * DEG_TO_RAD;
        gyroData->z = ((float) raw_gyro.z) * gyroSensitivity * DEG_TO_RAD;
    }
}
