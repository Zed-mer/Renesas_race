#ifndef ICM42688_H
#define ICM42688_H

#include "hal_data.h"
#include <stdint.h>

typedef struct st_icm42688_raw_data
{
    int16_t x;
    int16_t y;
    int16_t z;
} icm42688RawData_t;

typedef struct st_icm42688_float3
{
    float x;
    float y;
    float z;
} icm42688Float3_t;

typedef struct st_quaternion
{
    float q0;
    float q1;
    float q2;
    float q3;
} Quaternion_t;

fsp_err_t bsp_Icm42688Init(void);
fsp_err_t bsp_Icm42688SciInit(void);
void      bsp_IcmGetRawData(icm42688RawData_t * p_acc_data, icm42688RawData_t * p_gyro_data, int16_t * p_temp_raw);
void      bsp_IcmSciGetRawData(icm42688RawData_t * p_acc_data, icm42688RawData_t * p_gyro_data, int16_t * p_temp_raw);
void      bsp_IcmGetScaledData(icm42688Float3_t * p_acc_data, icm42688Float3_t * p_gyro_data, float * p_temp_c);
void      bsp_IcmSciGetScaledData(icm42688Float3_t * p_acc_data, icm42688Float3_t * p_gyro_data, float * p_temp_c);
void      spi0_callback(spi_callback_args_t * p_args);
void      sci_spi_callback(spi_callback_args_t * p_args);

#endif
