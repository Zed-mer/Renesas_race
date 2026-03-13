#include "icm42688.h"
#include <stdio.h>
#include <stdbool.h>

// 状态标志
static volatile bool g_spi_txc_flag = false;
static float accSensitivity  = 0.0f;
static float gyroSensitivity = 0.0f;

/* --- 内部底层函数 --- */

// 修复警告：添加函数声明或保持 static

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
    g_spi_txc_flag = false;

    /* 修复警告：ICM_SPI_INSTANCE.p_ctrl 已经是地址，不要加 & */
    R_SPI_WriteRead(ICM_SPI_INSTANCE.p_ctrl, &tx_data, &rx_data, 1, SPI_BIT_WIDTH_8_BITS);

    uint32_t timeout = 100000;
    while (!g_spi_txc_flag && timeout--);
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
    spi_read_write_byte(reg | 0x80);
    reg_val = spi_read_write_byte(0xFF);
    CS_HIGH();
    return reg_val;
}

static void icm42688_read_regs(uint8_t reg, uint8_t* buf, uint16_t len)
{
    CS_LOW();
    spi_read_write_byte(reg | 0x80);
    while(len--)
    {
        *buf++ = spi_read_write_byte(0xFF);
    }
    CS_HIGH();
}


//寄存器配置封装
// 定义一些常用的配置宏，方便调用
#define ACCEL_MODE_OFF      0x00
#define ACCEL_MODE_LP       0x02 // Low Power Mode
#define ACCEL_MODE_LN       0x03 // Low Noise Mode

#define GYRO_MODE_OFF       0x00
#define GYRO_MODE_STBY      0x01 // Standby Mode
#define GYRO_MODE_LN        0x03 // Low Noise Mode

///**
// * @brief 切换寄存器 Bank
// * @param bank 目标Bank号 (0~4)
// */
//static void icm42688_set_bank(uint8_t bank)
//{
//    // Register bank selection 000: Bank 0 到 100: Bank 4 [cite: 496]
//    icm42688_write_reg(ICM42688_REG_BANK_SEL, bank & 0x07);
//}

///**
// * @brief 软复位传感器
// */
//static void icm42688_soft_reset(void)
//{
//    icm42688_set_bank(0);
//    // 写入1使能软件复位，需要等待1ms生效 [cite: 11]
//    icm42688_write_reg(ICM42688_DEVICE_CONFIG, 0x01);
//
//    // 注意：这里请替换为你工程里的毫秒延时函数
//    R_BSP_SoftwareDelay(2, BSP_DELAY_UNITS_MILLISECONDS);
//}
//
///**
// * @brief 检查传感器ID
// * @return 1: 成功; 0: 失败
// */
//static uint8_t icm42688_check_id(void)
//{
//    icm42688_set_bank(0);
//    // 默认设备 ID 应为 0x47 [cite: 489]
//    uint8_t id = icm42688_read_reg(ICM42688_WHO_AM_I);
//    return (id == 0x47) ? 1 : 0;
//}
///**
// * @brief 配置传感器电源模式
// * @param gyro_mode 陀螺仪模式 (0:Off, 1:Standby, 3:Low Noise)
// * @param accel_mode 加速度计模式 (0:Off, 2:Low Power, 3:Low Noise)
// * @param temp_dis 是否禁用温度传感器 (0:启用, 1:禁用)
// */
//static void icm42688_set_pwr_mgmt(uint8_t gyro_mode, uint8_t accel_mode, uint8_t temp_dis)
//{
//    icm42688_set_bank(0);
//    // TEMP_DIS[5], GYRO_MODE[3:2], ACCEL_MODE[1:0] [cite: 296]
//    uint8_t pwr_val = ((temp_dis & 0x01) << 5) | ((gyro_mode & 0x03) << 2) | (accel_mode & 0x03);
//    icm42688_write_reg(ICM42688_PWR_MGMT0, pwr_val);
//
//    // 状态转换需要等待至少 200us 后才能写入其他寄存器 [cite: 296]
//    // 为了稳妥，这里延时1ms
//    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
//}
//
///**
// * @brief 配置陀螺仪量程和采样率
// * @param fs_sel 量程选择 (例: 0=±2000dps, 1=±1000dps, 2=±500dps ...)
// * @param odr 采样率 (例: 6=1kHz(默认), 15=500Hz, 7=200Hz ...)
// */
//static void icm42688_set_gyro_config(uint8_t fs_sel, uint8_t odr)
//{
//    icm42688_set_bank(0);
//    // GYRO_FS_SEL[7:5], GYRO_ODR[3:0] [cite: 305]
//    uint8_t cfg_val = ((fs_sel & 0x07) << 5) | (odr & 0x0F);
//    icm42688_write_reg(ICM42688_GYRO_CONFIG0, cfg_val);
//}
//
///**
// * @brief 配置加速度计量程和采样率
// * @param fs_sel 量程选择 (例: 0=±16g(默认), 1=±8g, 2=±4g, 3=±2g)
// * @param odr 采样率 (例: 6=1kHz(默认), 15=500Hz, 7=200Hz ...)
// */
//static void icm42688_set_accel_config(uint8_t fs_sel, uint8_t odr)
//{
//    icm42688_set_bank(0);
//    // ACCEL_FS_SEL[7:5], ACCEL_ODR[3:0] [cite: 314]
//    uint8_t cfg_val = ((fs_sel & 0x07) << 5) | (odr & 0x0F);
//    icm42688_write_reg(ICM42688_ACCEL_CONFIG0, cfg_val);
//}
///**
// * @brief 一次性读取所有传感器原始数据 (14字节)
// * @param accel 存储加速度数据的数组 (长度3)
// * @param gyro 存储陀螺仪数据的数组 (长度3)
// * @param temp 存储温度数据的变量指针
// */
//static void icm42688_read_raw_data(int16_t* accel, int16_t* gyro, int16_t* temp)
//{
//    uint8_t buf[14];
//
//    // 从 TEMP_DATA1 (0x1D) 开始连续读取 14 个字节 [cite: 39, 44]
//    // 顺序为：Temp(2) -> Accel X(2) -> Y(2) -> Z(2) -> Gyro X(2) -> Y(2) -> Z(2)
//    icm42688_read_regs(ICM42688_TEMP_DATA1, buf, 14);
//
//    // 拼接 16-bit 数据 (高位在前，低位在后)
//    if(temp != NULL) {
//        *temp = (int16_t)((buf[0] << 8) | buf[1]);
//    }
//
//    if(accel != NULL) {
//        accel[0] = (int16_t)((buf[2] << 8) | buf[3]);
//        accel[1] = (int16_t)((buf[4] << 8) | buf[5]);
//        accel[2] = (int16_t)((buf[6] << 8) | buf[7]);
//    }
//
//    if(gyro != NULL) {
//        gyro[0] = (int16_t)((buf[8] << 8) | buf[9]);
//        gyro[1] = (int16_t)((buf[10] << 8) | buf[11]);
//        gyro[2] = (int16_t)((buf[12] << 8) | buf[13]);
//    }
//}















/* --- 应用层接口 --- */

fsp_err_t bsp_Icm42688Init(void)
{
    fsp_err_t err;
    uint8_t who_am_i = 0;

    /* 修复警告：同理，去掉 p_ctrl 和 p_cfg 前面的 & */
    // 打开 SPI 外设，建立与单片机的通信
    err = R_SPI_Open(ICM_SPI_INSTANCE.p_ctrl, ICM_SPI_INSTANCE.p_cfg);
    if (FSP_SUCCESS != err) return err;

    // 切换到 Bank 0 以访问基本配置寄存器 [cite: 496]
    icm42688_write_reg(ICM42688_REG_BANK_SEL, 0);

    // 触发软件复位 (向 DEVICE_CONFIG 写入 0x01) [cite: 7, 11]
    icm42688_write_reg(ICM42688_DEVICE_CONFIG, 0x01);

    // 延时等待复位完成。数据手册要求复位后至少等待 1ms [cite: 11]，这里延时 10ms 非常安全
    R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);

    // 读取设备 ID，验证 SPI 通信是否正常以及设备是否正确响应 [cite: 488]
    who_am_i = icm42688_read_reg(ICM42688_WHO_AM_I);

    /* 修复错误：FSP 库没有 FSP_ERR_SENSOR_MISMATCH，改为通用错误 */
    // ICM-42688-P 的默认设备 ID 应为 0x47 [cite: 489]
    if (who_am_i != ICM42688_ID) return FSP_ERR_NOT_FOUND;;

    // 配置中断引脚 (INT_CONFIG 寄存器) [cite: 27]
    // 写入 0x30 (二进制 0011 0000) 将 INT2 引脚配置为：锁存模式 (Latched mode) 和推挽输出 (Push pull)
    icm42688_write_reg(ICM42688_INT_CONFIG, 0x30);
    // 功能：将“数据准备就绪 (Data Ready)”中断信号路由/绑定到物理引脚 INT1
    icm42688_write_reg(ICM42688_INT_SOURCE0, 0x08);
    // 配置加速度计：量程 ±4g，输出数据速率 (ODR) 100Hz [cite: 310, 314]
    icm42688_write_reg(ICM42688_ACCEL_CONFIG0, (AFS_4G << 5) | AODR_100Hz);

    // 配置陀螺仪：量程 ±1000dps，输出数据速率 (ODR) 100Hz [cite: 301, 305]
    icm42688_write_reg(ICM42688_GYRO_CONFIG0, (GFS_1000DPS << 5) | GODR_100Hz);

    // 根据配置的量程，计算转换灵敏度比例因子 (用于后续将 16-bit 原始数据转为实际物理单位)
    accSensitivity = 4000.0f / 32768.0f;
    gyroSensitivity = 1000.0f / 32768.0f;

    // 电源管理配置 (PWR_MGMT0 寄存器) [cite: 292]
    // 写入 0x0F (二进制 0010 1111) 代表以下组合配置：
    // 关闭温度传感器 [cite: 296]
    // 将陀螺仪置于低噪模式 (Low Noise Mode) [cite: 296]
    // 将加速度计置于低噪模式 (Low Noise Mode) [cite: 296]
    icm42688_write_reg(ICM42688_PWR_MGMT0, 0x2F);

    // 传感器从 OFF 状态转换到其他模式时，需要等待至少 200µs 才能写入其他寄存器 [cite: 296]，这里延时 10ms 确保状态稳定
    R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

void bsp_IcmGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData)
{
    uint8_t buf[12];
    icm42688_read_regs(ICM42688_ACCEL_DATA_X1, buf, 12);

    /* 修复警告：添加明确的强转 (int16_t)，避免 float 转换警告 */
    accData->x = (int16_t)((float)((int16_t)((buf[0] << 8) | buf[1])) * accSensitivity);
    accData->y = (int16_t)((float)((int16_t)((buf[2] << 8) | buf[3])) * accSensitivity);
    accData->z = (int16_t)((float)((int16_t)((buf[4] << 8) | buf[5])) * accSensitivity);

    GyroData->x = (int16_t)((float)((int16_t)((buf[6] << 8) | buf[7])) * gyroSensitivity);
    GyroData->y = (int16_t)((float)((int16_t)((buf[8] << 8) | buf[9])) * gyroSensitivity);
    GyroData->z = (int16_t)((float)((int16_t)((buf[10] << 8) | buf[11])) * gyroSensitivity);
}
