#ifndef __ICM42688_H__
#define __ICM42688_H__

#include "hal_data.h" // 包含 FSP 基础头文件
#include <stdbool.h>
/* --- 用户硬件定义：请根据实际接线修改 --- */
#define ICM42688_CS_PIN      BSP_IO_PORT_02_PIN_05  // 假设 CS 接在 P103，请按需修改
#define ICM_SPI_INSTANCE     g_spi0                  // 对应 FSP 中的 SPI 实例名

/* --- SCI用户硬件定义：请根据实际接线修改 --- */
#define ICM42688_SCI_CS_PIN      BSP_IO_PORT_02_PIN_10  // 假设 CS 接在 P103，请按需修改
#define ICM_SCI_SPI_INSTANCE     g_spi1                  // 对应 FSP 中的 SPI 实例名

/* --- ICM42688 寄存器完整映射定义 --- */

// ==========================================
// User Bank 0 (默认 Bank) [cite: 3, 5, 7]
// ==========================================

// --- 1. 系统与设备控制寄存器 ---
#define ICM42688_DEVICE_CONFIG      0x11  // 设备配置寄存器 (软复位, SPI模式) [cite: 7]
#define ICM42688_DRIVE_CONFIG       0x13  // 驱动配置 (SPI/I2C转换速率) [cite: 7]
#define ICM42688_SIGNAL_PATH_RESET  0x4B  // 信号路径复位/FIFO刷新 [cite: 7]
#define ICM42688_INTF_CONFIG0       0x4C  // 接口配置0 (传感器数据端序等) [cite: 7]
#define ICM42688_INTF_CONFIG1       0x4D  // 接口配置1 (RTC时钟模式等) [cite: 7]
#define ICM42688_PWR_MGMT0          0x4E  // 电源管理0 (温度开关, Accel/Gyro模式) [cite: 7]
#define ICM42688_WHO_AM_I           0x75  // 设备ID寄存器 (默认值通常为0x47) [cite: 7]
#define ICM42688_REG_BANK_SEL       0x76  // 寄存器Bank选择 (用于切换Bank 0-4) [cite: 7]

// --- 2. 传感器量程与滤波器配置 ---
#define ICM42688_GYRO_CONFIG0       0x4F  // 陀螺仪配置0 (量程FS_SEL, 输出速率ODR) [cite: 7]
#define ICM42688_ACCEL_CONFIG0      0x50  // 加速度计配置0 (量程FS_SEL, 输出速率ODR) [cite: 7]
#define ICM42688_GYRO_CONFIG1       0x51  // 陀螺仪配置1 (温度滤波带宽等) [cite: 7]
#define ICM42688_GYRO_ACCEL_CONFIG0 0x52  // 陀螺仪/加速度计配置0 (UI滤波带宽) [cite: 7]
#define ICM42688_ACCEL_CONFIG1      0x53  // 加速度计配置1 (UI滤波阶数) [cite: 7]

// --- 3. 传感器数据输出寄存器 ---
#define ICM42688_TEMP_DATA1         0x1D  // 温度数据 高8位 [cite: 7]
#define ICM42688_TEMP_DATA0         0x1E  // 温度数据 低8位 [cite: 7]
#define ICM42688_ACCEL_DATA_X1      0x1F  // 加速度计 X轴 高8位 [cite: 7]
#define ICM42688_ACCEL_DATA_X0      0x20  // 加速度计 X轴 低8位 [cite: 7]
#define ICM42688_ACCEL_DATA_Y1      0x21  // 加速度计 Y轴 高8位 [cite: 7]
#define ICM42688_ACCEL_DATA_Y0      0x22  // 加速度计 Y轴 低8位 [cite: 7]
#define ICM42688_ACCEL_DATA_Z1      0x23  // 加速度计 Z轴 高8位 [cite: 7]
#define ICM42688_ACCEL_DATA_Z0      0x24  // 加速度计 Z轴 低8位 [cite: 7]
#define ICM42688_GYRO_DATA_X1       0x25  // 陀螺仪 X轴 高8位 [cite: 7]
#define ICM42688_GYRO_DATA_X0       0x26  // 陀螺仪 X轴 低8位 [cite: 7]
#define ICM42688_GYRO_DATA_Y1       0x27  // 陀螺仪 Y轴 高8位 [cite: 7]
#define ICM42688_GYRO_DATA_Y0       0x28  // 陀螺仪 Y轴 低8位 [cite: 7]
#define ICM42688_GYRO_DATA_Z1       0x29  // 陀螺仪 Z轴 高8位 [cite: 7]
#define ICM42688_GYRO_DATA_Z0       0x2A  // 陀螺仪 Z轴 低8位 [cite: 7]

// --- 4. 中断控制与状态寄存器 ---
#define ICM42688_INT_CONFIG         0x14  // 中断配置 (INT1/INT2模式, 极性, 驱动电路) [cite: 7]
#define ICM42688_INT_STATUS         0x2D  // 中断状态 (数据准备就绪, FIFO满等) [cite: 7]
#define ICM42688_INT_STATUS2        0x37  // 中断状态2 (唤醒动作 WOM 中断等) [cite: 7]
#define ICM42688_INT_STATUS3        0x38  // 中断状态3 (计步、倾斜、唤醒、睡眠检测等) [cite: 7]
#define ICM42688_INT_CONFIG0        0x63  // 中断配置0 (清除方式配置) [cite: 7]
#define ICM42688_INT_CONFIG1        0x64  // 中断配置1 (脉冲宽度设定) [cite: 7]
#define ICM42688_INT_SOURCE0        0x65  // 中断源配置0 (INT1映射: FSYNC, UI DRDY, FIFO) [cite: 7]
#define ICM42688_INT_SOURCE1        0x66  // 中断源配置1 (INT1映射: WOM, SMD) [cite: 7]
#define ICM42688_INT_SOURCE3        0x68  // 中断源配置3 (INT2映射配置A) [cite: 7]
#define ICM42688_INT_SOURCE4        0x69  // 中断源配置4 (INT2映射配置B) [cite: 7]

// --- 5. FIFO 与时间戳寄存器 ---
#define ICM42688_FIFO_CONFIG        0x16  // FIFO配置 (旁路/流模式) [cite: 7]
#define ICM42688_FIFO_COUNTH        0x2E  // FIFO计数 高8位 [cite: 7]
#define ICM42688_FIFO_COUNTL        0x2F  // FIFO计数 低8位 [cite: 7]
#define ICM42688_FIFO_DATA          0x30  // FIFO数据读写口 [cite: 7]
#define ICM42688_FIFO_CONFIG1       0x5F  // FIFO配置1 (包含哪些传感器数据) [cite: 7]
#define ICM42688_FIFO_CONFIG2       0x60  // FIFO配置2 (水位阈值) [cite: 7]
#define ICM42688_FIFO_CONFIG3       0x61  // FIFO配置3 (水位阈值扩展) [cite: 7]
#define ICM42688_FIFO_LOST_PKT0     0x6C  // FIFO丢失包计数低字节 [cite: 7]
#define ICM42688_FIFO_LOST_PKT1     0x6D  // FIFO丢失包计数高字节 [cite: 7]
#define ICM42688_TMST_FSYNCH        0x2B  // 时间戳同步数据 高8位 [cite: 7]
#define ICM42688_TMST_FSYNCL        0x2C  // 时间戳同步数据 低8位 [cite: 7]
#define ICM42688_TMST_CONFIG        0x54  // 时间戳配置 (增量、启用等) [cite: 7]
#define ICM42688_FSYNC_CONFIG       0x62  // FSYNC 同步配置 [cite: 7]

// --- 6. APEX 高级计步与动作识别 ---
#define ICM42688_APEX_DATA0         0x31  // 计步器步数 低8位 [cite: 7]
#define ICM42688_APEX_DATA1         0x32  // 计步器步数 高8位 [cite: 7]
#define ICM42688_APEX_DATA2         0x33  // 步行节奏 (Cadence) [cite: 7]
#define ICM42688_APEX_DATA3         0x34  // DMP状态机 (活动分类) [cite: 7]
#define ICM42688_APEX_DATA4         0x35  // 敲击动作状态 [cite: 7]
#define ICM42688_APEX_DATA5         0x36  // 双击时序 [cite: 7]
#define ICM42688_APEX_CONFIG0       0x56  // APEX配置0 (使能计步、倾斜、敲击等) [cite: 7]
#define ICM42688_SMD_CONFIG         0x57  // SMD (显著运动检测) 与 WOM 模式配置 [cite: 7]

// --- 7. 自检寄存器 ---
#define ICM42688_SELF_TEST_CONFIG   0x70  // 传感器自检控制使能位 [cite: 7]


// ==========================================
// User Bank 1 (陀螺仪静态配置与自检) [cite: 14, 15]
// ==========================================
#define ICM42688_SENSOR_CONFIG0     0x03  // 传感器配置0 (关闭指定轴等) [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC2 0x0B // 陀螺仪抗混叠滤波器配置 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC3 0x0C // 陀螺仪配置静态参数3 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC4 0x0D // 陀螺仪配置静态参数4 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC5 0x0E // 陀螺仪配置静态参数5 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC6 0x0F // 陀螺仪陷波滤波器配置6 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC7 0x10 // 陀螺仪陷波滤波器配置7 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC8 0x11 // 陀螺仪陷波滤波器配置8 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC9 0x12 // 陀螺仪陷波滤波器配置9 [cite: 15]
#define ICM42688_GYRO_CONFIG_STATIC10 0x13 // 陀螺仪陷波滤波器带宽选择 [cite: 15]
#define ICM42688_XG_ST_DATA         0x5F  // 陀螺仪 X轴自检数据 [cite: 15]
#define ICM42688_YG_ST_DATA         0x60  // 陀螺仪 Y轴自检数据 [cite: 15]
#define ICM42688_ZG_ST_DATA         0x61  // 陀螺仪 Z轴自检数据 [cite: 15]
#define ICM42688_TMSTVAL0           0x62  // 时间戳当前值 7:0 [cite: 15]
#define ICM42688_TMSTVAL1           0x63  // 时间戳当前值 15:8 [cite: 15]
#define ICM42688_TMSTVAL2           0x64  // 时间戳当前值 19:16 [cite: 15]
#define ICM42688_INTF_CONFIG4       0x7A  // 接口配置4 (I3C / SPI_AP 4线模式) [cite: 15]
#define ICM42688_INTF_CONFIG5       0x7B  // 接口配置5 [cite: 15]
#define ICM42688_INTF_CONFIG6       0x7C  // 接口配置6 (I3C DDR/SDR 使能) [cite: 15]


// ==========================================
// User Bank 2 (加速度计静态配置与自检) [cite: 20, 22]
// ==========================================
#define ICM42688_ACCEL_CONFIG_STATIC2 0x03 // 加速度计抗混叠滤波器配置2 [cite: 22]
#define ICM42688_ACCEL_CONFIG_STATIC3 0x04 // 加速度计抗混叠滤波器配置3 [cite: 22]
#define ICM42688_ACCEL_CONFIG_STATIC4 0x05 // 加速度计抗混叠滤波器配置4 [cite: 22]
#define ICM42688_XA_ST_DATA         0x3B // 加速度计 X轴自检数据 [cite: 22]
#define ICM42688_YA_ST_DATA         0x3C // 加速度计 Y轴自检数据 [cite: 22]
#define ICM42688_ZA_ST_DATA         0x3D // 加速度计 Z轴自检数据 [cite: 22]


// ==========================================
// User Bank 3 (时钟相关) [cite: 23, 24]
// ==========================================
#define ICM42688_CLKDIV             0x2A  // 时钟分频器 [cite: 24]


// ==========================================
// User Bank 4 (APEX 阈值设定与校准偏移量) [cite: 25, 26]
// ==========================================
#define ICM42688_APEX_CONFIG1       0x40  // APEX DMP配置1 (低功耗阈值/定时器) [cite: 26]
#define ICM42688_APEX_CONFIG2       0x41  // APEX DMP配置2 (计步振幅阈值) [cite: 26]
#define ICM42688_APEX_CONFIG3       0x42  // APEX DMP配置3 (计步检测阈值) [cite: 26]
#define ICM42688_APEX_CONFIG4       0x43  // APEX DMP配置4 (倾斜与休眠等待时间) [cite: 26]
#define ICM42688_APEX_CONFIG5       0x44  // APEX DMP配置5 (挂载矩阵) [cite: 26]
#define ICM42688_APEX_CONFIG6       0x45  // APEX DMP配置6 (睡眠手势延迟) [cite: 26]
#define ICM42688_APEX_CONFIG7       0x46  // APEX DMP配置7 (敲击检测参数) [cite: 26]
#define ICM42688_APEX_CONFIG8       0x47  // APEX DMP配置8 (敲击时间窗口) [cite: 26]
#define ICM42688_APEX_CONFIG9       0x48  // APEX DMP配置9 (灵敏度模式) [cite: 26]
#define ICM42688_ACCEL_WOM_X_THR    0x4A  // 加速度计X轴唤醒 (WOM) 阈值 [cite: 26]
#define ICM42688_ACCEL_WOM_Y_THR    0x4B  // 加速度计Y轴唤醒 (WOM) 阈值 [cite: 26]
#define ICM42688_ACCEL_WOM_Z_THR    0x4C  // 加速度计Z轴唤醒 (WOM) 阈值 [cite: 26]
#define ICM42688_INT_SOURCE6        0x4D  // 中断源配置6 (INT1映射: APEX中断) [cite: 26]
#define ICM42688_INT_SOURCE7        0x4E  // 中断源配置7 (INT2映射: APEX中断) [cite: 26]
#define ICM42688_INT_SOURCE8        0x4F  // 中断源配置8 (IBI使能) [cite: 26]
#define ICM42688_INT_SOURCE9        0x50  // 中断源配置9 (IBI WOM/SMD使能) [cite: 26]
#define ICM42688_INT_SOURCE10       0x51  // 中断源配置10 (IBI 计步/倾斜使能) [cite: 26]
#define ICM42688_OFFSET_USER0       0x77  // 用户偏置校准0 (陀螺仪X低8位) [cite: 26]
#define ICM42688_OFFSET_USER1       0x78  // 用户偏置校准1 (陀螺仪X/Y高位) [cite: 26]
#define ICM42688_OFFSET_USER2       0x79  // 用户偏置校准2 (陀螺仪Y低8位) [cite: 26]
#define ICM42688_OFFSET_USER3       0x7A  // 用户偏置校准3 (陀螺仪Z低8位) [cite: 26]
#define ICM42688_OFFSET_USER4       0x7B  // 用户偏置校准4 (陀螺仪Z/加速度X高位) [cite: 26]
#define ICM42688_OFFSET_USER5       0x7C  // 用户偏置校准5 (加速度X低8位) [cite: 26]
#define ICM42688_OFFSET_USER6       0x7D  // 用户偏置校准6 (加速度Y低8位) [cite: 26]
#define ICM42688_OFFSET_USER7       0x7E  // 用户偏置校准7 (加速度Y/Z高位) [cite: 26]
#define ICM42688_OFFSET_USER8       0x7F  // 用户偏置校准8 (加速度Z低8位) [cite: 26]

// 量程与速率宏 (保留原有定义)
#define AFS_4G      0x02
#define GFS_1000DPS 0x01
#define AODR_100Hz  0x08
#define GODR_100Hz  0x08

/* --- 数据结构 --- */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} icm42688RawData_t;

typedef struct
{
    float x;
    float y;
    float z;
} icm42688Float3_t;

typedef struct
{
    float q0;
    float q1;
    float q2;
    float q3;
} Quaternion_t;





#define ICM42688_ID              0x47
/* --- 函数原型 --- */
fsp_err_t bsp_Icm42688Init(void);
fsp_err_t bsp_Icm42688SciInit(void);
void bsp_IcmGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData);
void bsp_IcmSciGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData);
void bsp_IcmGetScaledData(icm42688Float3_t *accData, icm42688Float3_t *gyroData);
void bsp_IcmSciGetScaledData(icm42688Float3_t *accData, icm42688Float3_t *gyroData);
void bsp_IcmGetTemperature(float* pTemp);
uint32_t imu_read_timestamp();
void bsp_IcmGetScaledDataWithTimestamp(icm42688Float3_t *accData, icm42688Float3_t *gyroData, uint32_t *timestamp);
#endif
