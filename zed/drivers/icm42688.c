#include "icm42688.h"
#include <stdbool.h>

/* 角速度从度每秒转换为弧度每秒的比例系数。 */
#define DEG_TO_RAD                    (0.01745329251994329577f)
/* 轮询等待 SPI/SCI_SPI 传输完成时使用的软件超时计数，避免硬件异常时永久卡死。 */
#define ICM42688_SPI_TIMEOUT_LOOPS    (100000U)
/* ICM42688 SPI 读寄存器时，地址最高位需要置 1。 */
#define ICM42688_SPI_READ_MASK        (0x80U)
/* SPI 读数据时发送的占位字节。 */
#define ICM42688_SPI_DUMMY_BYTE       (0xFFU)

/* 
 * 将“一颗 IMU 所在的总线资源”抽象成统一上下文。
 * 这样硬件 SPI(g_spi0) 和 SCI_SPI(g_spi1) 都可以走同一套寄存器访问代码，
 * 只需要替换实例、片选脚和完成标志即可。
 */
typedef struct st_icm42688_bus
{
    spi_instance_t const * p_spi;               /* FSP 中的 SPI 实例。 */
    bsp_io_port_pin_t      cs_pin;              /* 当前 IMU 对应的片选引脚。 */
    volatile bool        * p_transfer_complete; /* 当前总线传输完成标志，由回调置位。 */
} icm42688_bus_t;

/* 两路 SPI 各自维护独立的“传输完成”标志，防止双 IMU 之间互相干扰。 */
static volatile bool g_spi0_txc_flag = false;
static volatile bool g_spi1_txc_flag = false;
/* 当前量程配置对应的灵敏度系数。
 * g_acc_sensitivity: mg/LSB
 * g_gyro_sensitivity: dps/LSB
 */
static float g_acc_sensitivity = 0.0f;
static float g_gyro_sensitivity = 0.0f;

/* 上臂 IMU：挂在硬件 SPI0 上。 */
static icm42688_bus_t const g_upper_imu_bus =
{
    .p_spi                = &ICM_SPI_INSTANCE,
    .cs_pin               = ICM42688_CS_PIN,
    .p_transfer_complete  = &g_spi0_txc_flag,
};

/* 手腕 IMU：挂在 SCI_SPI(g_spi1) 上。 */
static icm42688_bus_t const g_lower_imu_bus =
{
    .p_spi                = &ICM_SCI_SPI_INSTANCE,
    .cs_pin               = ICM42688_SCI_CS_PIN,
    .p_transfer_complete  = &g_spi1_txc_flag,
};

static fsp_err_t icm42688_transfer_byte(icm42688_bus_t const * p_bus, uint8_t tx_data, uint8_t * p_rx_data);
static fsp_err_t icm42688_write_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t val);
static fsp_err_t icm42688_read_reg(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_reg_val);
static fsp_err_t icm42688_read_regs(icm42688_bus_t const * p_bus, uint8_t reg, uint8_t * p_buf, uint16_t len);
static fsp_err_t icm42688_init_bus(icm42688_bus_t const * p_bus);
static fsp_err_t icm42688_get_raw_data(icm42688_bus_t const * p_bus,
                                       icm42688RawData_t   * p_acc_data,
                                       icm42688RawData_t   * p_gyro_data);
static void      icm42688_scale_data(icm42688RawData_t const * p_raw_acc,
                                     icm42688RawData_t const * p_raw_gyro,
                                     icm42688Float3_t       * p_acc_data,
                                     icm42688Float3_t       * p_gyro_data);

/* 硬件 SPI0 传输完成回调。
 * FSP 驱动在一帧 8bit 收发结束后进入这里，我们只做最轻量的置位操作。
 */
void spi0_callback(spi_callback_args_t * p_args)
{
    if ((NULL != p_args) && (SPI_EVENT_TRANSFER_COMPLETE == p_args->event))
    {
        g_spi0_txc_flag = true;
    }
}

/* SCI_SPI 传输完成回调。
 * 逻辑与 SPI0 一致，只是对应另一条总线。
 */
void sci_spi_callback(spi_callback_args_t * p_args)
{
    if ((NULL != p_args) && (SPI_EVENT_TRANSFER_COMPLETE == p_args->event))
    {
        g_spi1_txc_flag = true;
    }
}

/* 
 * 在指定总线上完成 1 个字节的 SPI 全双工收发。
 * ICM42688 的寄存器访问是“先发地址，再发/收数据”，
 * 所以更底层的实现统一为逐字节 writeRead。
 */
static fsp_err_t icm42688_transfer_byte(icm42688_bus_t const * p_bus, uint8_t tx_data, uint8_t * p_rx_data)
{
    fsp_err_t err;
    uint8_t   rx_data = 0U;
    uint32_t  timeout = ICM42688_SPI_TIMEOUT_LOOPS;

    /* 启动传输前先清除完成标志，防止误用上一次的结果。 */
    *(p_bus->p_transfer_complete) = false;
    err = p_bus->p_spi->p_api->writeRead(p_bus->p_spi->p_ctrl, &tx_data, &rx_data, 1U, SPI_BIT_WIDTH_8_BITS);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 等待底层中断回调置位，说明当前字节的时钟和数据都已完成。 */
    while ((!*(p_bus->p_transfer_complete)) && (timeout > 0U))
    {
        timeout--;
    }

    /* 超时通常意味着总线异常、实例未正确打开或中断未正常工作。 */
    if (0U == timeout)
    {
        return FSP_ERR_TIMEOUT;
    }

    /* 如果调用方关心返回值，就把收到的字节带回去。 */
    if (NULL != p_rx_data)
    {
        *p_rx_data = rx_data;
    }

    return FSP_SUCCESS;
}

/* 写单个寄存器：
 * 1. 拉低 CS
 * 2. 发送寄存器地址
 * 3. 发送写入值
 * 4. 拉高 CS
 */
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

/* 读单个寄存器：
 * 1. 拉低 CS
 * 2. 发送“读地址”(地址 | 0x80)
 * 3. 再发一个 dummy 字节，从 MISO 上收回寄存器值
 * 4. 拉高 CS
 */
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

/* 连续读取多个寄存器。
 * ICM42688 支持 burst read，因此先发起始地址，之后连续时钟出 len 个字节。
 */
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

/* 
 * 初始化指定总线上的一颗 ICM42688。
 * 这里既适用于上臂 IMU，也适用于手腕 IMU。
 *
 * 初始化流程：
 * 1. 打开对应 SPI 实例
 * 2. 切回 Bank0
 * 3. 软件复位
 * 4. 读取 WHO_AM_I 验证通信
 * 5. 配置 Data Ready 中断映射
 * 6. 配置加速度/陀螺仪量程和 ODR
 * 7. 配置灵敏度换算系数
 * 8. 打开加速度和陀螺仪低噪声模式
 */
static fsp_err_t icm42688_init_bus(icm42688_bus_t const * p_bus)
{
    fsp_err_t err;
    uint8_t   who_am_i = 0U;

    /* 打开 FSP SPI 实例。若已打开则复用现有配置。 */
    err = p_bus->p_spi->p_api->open(p_bus->p_spi->p_ctrl, p_bus->p_spi->p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        return err;
    }

    /* 显式切到 Bank0，避免上电后寄存器页不确定。 */
    err = icm42688_write_reg(p_bus, ICM42688_REG_BANK_SEL, 0x00U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 软件复位芯片。 */
    err = icm42688_write_reg(p_bus, ICM42688_DEVICE_CONFIG, 0x01U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 给芯片一点时间完成内部重启。 */
    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

    /* 通过 WHO_AM_I 判断 SPI 通信是否正常，以及芯片型号是否正确。 */
    err = icm42688_read_reg(p_bus, ICM42688_WHO_AM_I, &who_am_i);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* ICM42688 正常应返回 0x47。 */
    if (ICM42688_ID != who_am_i)
    {
        return FSP_ERR_NOT_FOUND;
    }

    /* 配置中断输出极性/驱动方式。 */
    err = icm42688_write_reg(p_bus, ICM42688_INT_CONFIG, 0x30U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 将 UI Data Ready 映射到 INT1。 */
    err = icm42688_write_reg(p_bus, ICM42688_INT_SOURCE0, 0x08U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 加速度计配置：量程 ±4g，输出速率 100Hz。 */
    err = icm42688_write_reg(p_bus, ICM42688_ACCEL_CONFIG0, (uint8_t) ((AFS_4G << 5) | AODR_100Hz));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 陀螺仪配置：量程 ±1000dps，输出速率 100Hz。 */
    err = icm42688_write_reg(p_bus, ICM42688_GYRO_CONFIG0, (uint8_t) ((GFS_1000DPS << 5) | GODR_100Hz));
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 保存当前量程下的缩放系数，后续把原始值转换成工程单位。 */
    g_acc_sensitivity = 4000.0f / 32768.0f;
    g_gyro_sensitivity = 1000.0f / 32768.0f;

    /* 打开温度、加速度、陀螺仪，并切换到低噪声模式。 */
    err = icm42688_write_reg(p_bus, ICM42688_PWR_MGMT0, 0x2FU);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 等待 MEMS 和数字滤波链路稳定。 */
    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

/* 读取一帧原始 6 轴数据。
 * 从 ACCEL_DATA_X1 开始连续读 12 字节，顺序为：
 * AccX/AccY/AccZ/GyroX/GyroY/GyroZ
 */
static fsp_err_t icm42688_get_raw_data(icm42688_bus_t const * p_bus,
                                       icm42688RawData_t   * p_acc_data,
                                       icm42688RawData_t   * p_gyro_data)
{
    fsp_err_t err;
    uint8_t   buf[12] = {0};

    /* 连续突发读取 12 字节，效率高于逐寄存器读。 */
    err = icm42688_read_regs(p_bus, ICM42688_ACCEL_DATA_X1, buf, sizeof(buf));
    if (FSP_SUCCESS != err)
    {
        /* 一旦通信失败，主动清零输出，避免上层误用旧数据。 */
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

    if (NULL != p_acc_data)
    {
        /* 高字节在前，组合成有符号 16bit 原始加速度。 */
        p_acc_data->x = (int16_t) ((buf[0] << 8) | buf[1]);
        p_acc_data->y = (int16_t) ((buf[2] << 8) | buf[3]);
        p_acc_data->z = (int16_t) ((buf[4] << 8) | buf[5]);
    }

    if (NULL != p_gyro_data)
    {
        /* 同样按高字节在前，组合成有符号 16bit 原始角速度。 */
        p_gyro_data->x = (int16_t) ((buf[6] << 8) | buf[7]);
        p_gyro_data->y = (int16_t) ((buf[8] << 8) | buf[9]);
        p_gyro_data->z = (int16_t) ((buf[10] << 8) | buf[11]);
    }

    return FSP_SUCCESS;
}

/* 将原始 ADC 码值转换成上层更容易直接使用的物理量：
 * - 加速度：g
 * - 角速度：rad/s
 */
static void icm42688_scale_data(icm42688RawData_t const * p_raw_acc,
                                icm42688RawData_t const * p_raw_gyro,
                                icm42688Float3_t       * p_acc_data,
                                icm42688Float3_t       * p_gyro_data)
{
    if ((NULL != p_acc_data) && (NULL != p_raw_acc))
    {
        /* 先把 mg/LSB 换算成 mg，再除以 1000 得到 g。 */
        p_acc_data->x = ((float) p_raw_acc->x) * g_acc_sensitivity / 1000.0f;
        p_acc_data->y = ((float) p_raw_acc->y) * g_acc_sensitivity / 1000.0f;
        p_acc_data->z = ((float) p_raw_acc->z) * g_acc_sensitivity / 1000.0f;
    }

    if ((NULL != p_gyro_data) && (NULL != p_raw_gyro))
    {
        /* 先得到 dps，再乘以 DEG_TO_RAD 变成 rad/s。 */
        p_gyro_data->x = ((float) p_raw_gyro->x) * g_gyro_sensitivity * DEG_TO_RAD;
        p_gyro_data->y = ((float) p_raw_gyro->y) * g_gyro_sensitivity * DEG_TO_RAD;
        p_gyro_data->z = ((float) p_raw_gyro->z) * g_gyro_sensitivity * DEG_TO_RAD;
    }
}

/* 上臂 IMU 初始化入口。 */
fsp_err_t bsp_Icm42688Init(void)
{
    return icm42688_init_bus(&g_upper_imu_bus);
}

/* 手腕 IMU 初始化入口，底层使用 SCI_SPI。 */
fsp_err_t bsp_Icm42688SciInit(void)
{
    return icm42688_init_bus(&g_lower_imu_bus);
}

/* 读取上臂 IMU 的原始值。 */
void bsp_IcmGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData)
{
    (void) icm42688_get_raw_data(&g_upper_imu_bus, accData, GyroData);
}

/* 读取手腕 IMU 的原始值。 */
void bsp_IcmSciGetRawData(icm42688RawData_t *accData, icm42688RawData_t *GyroData)
{
    (void) icm42688_get_raw_data(&g_lower_imu_bus, accData, GyroData);
}

/* 读取上臂 IMU 的缩放后数据。 */
void bsp_IcmGetScaledData(icm42688Float3_t *accData, icm42688Float3_t *gyroData)
{
    icm42688RawData_t raw_acc = {0};
    icm42688RawData_t raw_gyro = {0};

    (void) icm42688_get_raw_data(&g_upper_imu_bus, &raw_acc, &raw_gyro);
    icm42688_scale_data(&raw_acc, &raw_gyro, accData, gyroData);
}

/* 读取手腕 IMU 的缩放后数据。 */
void bsp_IcmSciGetScaledData(icm42688Float3_t *accData, icm42688Float3_t *gyroData)
{
    icm42688RawData_t raw_acc = {0};
    icm42688RawData_t raw_gyro = {0};

    (void) icm42688_get_raw_data(&g_lower_imu_bus, &raw_acc, &raw_gyro);
    icm42688_scale_data(&raw_acc, &raw_gyro, accData, gyroData);
}
