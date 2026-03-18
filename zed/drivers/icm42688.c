#include "icm42688.h"
#include "icm42688_bus.h"
#include <stdbool.h>

#define DEG_TO_RAD  0.01745329251994329577f

static volatile bool g_spi0_txc_flag = false;
static volatile bool g_spi1_txc_flag = false;
static float         g_acc_sensitivity = 0.0f;
static float         g_gyro_sensitivity = 0.0f;

static icm42688_bus_t const g_upper_imu_bus =
{
    .p_spi = &g_spi0,
    .cs_pin = BSP_IO_PORT_02_PIN_05,
    .p_transfer_complete = &g_spi0_txc_flag,
};

static icm42688_bus_t const g_lower_imu_bus =
{
    .p_spi = &g_spi1,
    .cs_pin = BSP_IO_PORT_02_PIN_10,
    .p_transfer_complete = &g_spi1_txc_flag,
};

static void icm42688_scale_data(icm42688RawData_t const * p_raw_acc,
                                icm42688RawData_t const * p_raw_gyro,
                                icm42688Float3_t * p_acc_data,
                                icm42688Float3_t * p_gyro_data);

void spi0_callback(spi_callback_args_t * p_args)
{
    if ((NULL != p_args) && (SPI_EVENT_TRANSFER_COMPLETE == p_args->event))
    {
        g_spi0_txc_flag = true;
    }
}

void sci_spi_callback(spi_callback_args_t * p_args)
{
    if ((NULL != p_args) && (SPI_EVENT_TRANSFER_COMPLETE == p_args->event))
    {
        g_spi1_txc_flag = true;
    }
}

fsp_err_t bsp_Icm42688Init(void)
{
    fsp_err_t err = icm42688_bus_init(&g_upper_imu_bus);

    if (FSP_SUCCESS == err)
    {
        g_acc_sensitivity = 4000.0f / 32768.0f;
        g_gyro_sensitivity = 1000.0f / 32768.0f;
    }

    return err;
}

fsp_err_t bsp_Icm42688SciInit(void)
{
    fsp_err_t err = icm42688_bus_init(&g_lower_imu_bus);

    if (FSP_SUCCESS == err)
    {
        g_acc_sensitivity = 4000.0f / 32768.0f;
        g_gyro_sensitivity = 1000.0f / 32768.0f;
    }

    return err;
}

void bsp_IcmGetRawData(icm42688RawData_t * p_acc_data, icm42688RawData_t * p_gyro_data)
{
    (void) icm42688_bus_get_raw_data(&g_upper_imu_bus, p_acc_data, p_gyro_data);
}

void bsp_IcmSciGetRawData(icm42688RawData_t * p_acc_data, icm42688RawData_t * p_gyro_data)
{
    (void) icm42688_bus_get_raw_data(&g_lower_imu_bus, p_acc_data, p_gyro_data);
}

void bsp_IcmGetScaledData(icm42688Float3_t * p_acc_data, icm42688Float3_t * p_gyro_data)
{
    icm42688RawData_t raw_acc = {0};
    icm42688RawData_t raw_gyro = {0};

    (void) icm42688_bus_get_raw_data(&g_upper_imu_bus, &raw_acc, &raw_gyro);
    icm42688_scale_data(&raw_acc, &raw_gyro, p_acc_data, p_gyro_data);
}

void bsp_IcmSciGetScaledData(icm42688Float3_t * p_acc_data, icm42688Float3_t * p_gyro_data)
{
    icm42688RawData_t raw_acc = {0};
    icm42688RawData_t raw_gyro = {0};

    (void) icm42688_bus_get_raw_data(&g_lower_imu_bus, &raw_acc, &raw_gyro);
    icm42688_scale_data(&raw_acc, &raw_gyro, p_acc_data, p_gyro_data);
}

static void icm42688_scale_data(icm42688RawData_t const * p_raw_acc,
                                icm42688RawData_t const * p_raw_gyro,
                                icm42688Float3_t * p_acc_data,
                                icm42688Float3_t * p_gyro_data)
{
    if ((NULL != p_acc_data) && (NULL != p_raw_acc))
    {
        p_acc_data->x = ((float) p_raw_acc->x) * g_acc_sensitivity / 1000.0f;
        p_acc_data->y = ((float) p_raw_acc->y) * g_acc_sensitivity / 1000.0f;
        p_acc_data->z = ((float) p_raw_acc->z) * g_acc_sensitivity / 1000.0f;
    }

    if ((NULL != p_gyro_data) && (NULL != p_raw_gyro))
    {
        p_gyro_data->x = ((float) p_raw_gyro->x) * g_gyro_sensitivity * DEG_TO_RAD;
        p_gyro_data->y = ((float) p_raw_gyro->y) * g_gyro_sensitivity * DEG_TO_RAD;
        p_gyro_data->z = ((float) p_raw_gyro->z) * g_gyro_sensitivity * DEG_TO_RAD;
    }
}
