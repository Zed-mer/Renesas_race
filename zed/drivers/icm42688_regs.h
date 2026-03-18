#ifndef ICM42688_REGS_H
#define ICM42688_REGS_H

#define ICM42688_DEVICE_CONFIG         0x11
#define ICM42688_INT_CONFIG            0x14
#define ICM42688_FIFO_CONFIG           0x16
#define ICM42688_ACCEL_DATA_X1         0x1F
#define ICM42688_GYRO_DATA_Z0          0x2A
#define ICM42688_PWR_MGMT0             0x4E
#define ICM42688_GYRO_CONFIG0          0x4F
#define ICM42688_ACCEL_CONFIG0         0x50
#define ICM42688_INT_CONFIG1           0x64
#define ICM42688_INT_SOURCE0           0x65
#define ICM42688_WHO_AM_I              0x75
#define ICM42688_REG_BANK_SEL          0x76

#define ICM42688_ID                    0x47

#define AFS_4G                         0x02
#define GFS_1000DPS                    0x01
#define AODR_100Hz                     0x08
#define GODR_100Hz                     0x08

#endif
