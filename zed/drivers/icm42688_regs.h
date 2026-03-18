#ifndef ICM42688_REGS_H
#define ICM42688_REGS_H

#define ICM42688_DEVICE_CONFIG         0x11
#define ICM42688_INT_CONFIG            0x14
#define ICM42688_FIFO_CONFIG           0x16
#define ICM42688_TEMP_DATA1            0x1D
#define ICM42688_TEMP_DATA0            0x1E
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
/*
 * ICM-42688-P 数据手册里，ACCEL_ODR / GYRO_ODR 的 0x0F 都表示 500Hz。
 * 这里统一拉到 500Hz，让中断、原始采样和姿态更新都按 2ms 周期工作。
 */
#define AODR_500Hz                     0x0F
#define GODR_500Hz                     0x0F

#endif
