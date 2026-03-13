/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "app.h"
#include "drv_uart.h"
#include "drv_adc0.h"
#include "icm42688.h"
#include <stdio.h>
#include "hal_data.h"
#include <math.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/


/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
uint16_t g_adc_buffer[1]={0};
volatile uint16_t g_adc_flag = 0;
/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
#define ENVELOPE_BUFFER_SIZE 16

static float envelope_buffer[ENVELOPE_BUFFER_SIZE];
static int envelope_index = 0;
static float envelope_sum = 0;

static float get_envelope(float sample)
{
    envelope_sum -= envelope_buffer[envelope_index];
    envelope_sum += sample;
    envelope_buffer[envelope_index] = sample;

    envelope_index = (envelope_index + 1) % ENVELOPE_BUFFER_SIZE;

    return envelope_sum / ENVELOPE_BUFFER_SIZE;
}
void app_test(void)
{
    adcdrvinit();
    while(1)
    {
        ADCDrvRead(g_adc_buffer,1);
        float filtered_value = Filter((float)g_adc_buffer[0]);  // 使用滤波器处理数据
        // 包络检测
        float envelope_value = get_envelope(fabsf(filtered_value));  // 使用绝对值并进行包络提取
//        printf("%d\r\n",g_adc_buffer[0]);
        printf("Filtered: %.2f, %.2f\r\n", filtered_value, envelope_value);  // 输出结果
    };
}

static icm42688RawData_t s_accval;
static icm42688RawData_t s_gyroval;

static Quaternion_t g_quat_big = {1.0f, 0.0f, 0.0f, 0.0f};
static Quaternion_t g_quat_small = {1.0f, 0.0f, 0.0f, 0.0f};
static volatile bool s_imu_data_ready = false;
//
///* 外部中断回调 */
void icu8_callback(external_irq_callback_args_t *p_args)
{
    if (8 == p_args->channel) // 对应 IRQ8
    {
        // 1. 通知主循环打印
        s_imu_data_ready = true;
    }
}
void imu_test(void)
{
//    fsp_err_t err;
    fsp_err_t err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    // 1. 初始化传感器
    err = bsp_Icm42688Init();
    if (FSP_SUCCESS != err) {
        printf("IMU Init Error: %d\r\n", err);
        return;
    }
    printf("IMU Init Success!\r\n");

    // 2. 修正后的打开中断逻辑 (关键！使用 FSP 实例成员指针)
    err = R_ICU_ExternalIrqOpen(g_external_irq8.p_ctrl, g_external_irq8.p_cfg);
    if (FSP_SUCCESS != err) printf("IRQ Open Failed\r\n");

    R_ICU_ExternalIrqEnable(g_external_irq8.p_ctrl);

    printf("Waiting for IMU data...\r\n");

    while (1)
    {

        if (s_imu_data_ready)
        {
            s_imu_data_ready = false;
            //读取原始数据
            bsp_IcmGetRawData(&s_accval, &s_gyroval);
            // 在主循环打印，避免中断卡死
            printf("Acc: %d, %d, %d | Gyro: %d, %d, %d\r\n",
                    s_accval.x, s_accval.y, s_accval.z,
                    s_gyroval.x, s_gyroval.y, s_gyroval.z);
        }
    }
}

