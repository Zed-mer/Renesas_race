#ifndef ICM42688_BUS_H
#define ICM42688_BUS_H

#include "hal_data.h"
#include "icm42688.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct st_icm42688_bus
{
    spi_instance_t const * p_spi;
    bsp_io_port_pin_t      cs_pin;
    volatile bool        * p_transfer_complete;
} icm42688_bus_t;

fsp_err_t icm42688_bus_init(icm42688_bus_t const * p_bus);
fsp_err_t icm42688_bus_get_raw_data(icm42688_bus_t const * p_bus,
                                    int16_t * p_temp_raw,
                                    icm42688RawData_t * p_acc_data,
                                    icm42688RawData_t * p_gyro_data);

#endif
