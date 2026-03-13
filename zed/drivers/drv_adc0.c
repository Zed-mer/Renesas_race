/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "drv_adc0.h"
#include <stdbool.h>

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

static volatile bool adc_sample_cplt = false;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
void adcdrvinit(void)
{
    fsp_err_t err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    assert(FSP_SUCCESS == err);
    err = g_adc0.p_api->open(g_adc0.p_ctrl, g_adc0.p_cfg);
    assert(FSP_SUCCESS == err);
    /* 配置ADC指令的通道完成初始化 */
    err = g_adc0.p_api->scanCfg(g_adc0.p_ctrl, g_adc0.p_channel_cfg);
    assert(FSP_SUCCESS == err);
    /* 打开ELC设备完成初始化 */
    err = g_elc.p_api->open(g_elc.p_ctrl, g_elc.p_cfg);
    assert(FSP_SUCCESS == err);
    /* 使能ELC的连接功能 */
    err = g_elc.p_api->enable(g_elc.p_ctrl);
    assert(FSP_SUCCESS == err);
    /* 打开DMA设备完成初始化 */
    err = g_transfer0.p_api->open(g_transfer0.p_ctrl, g_transfer0.p_cfg);
    assert(FSP_SUCCESS == err);
    /* 打开定时器设备完成初始化 */
    err = g_timer0.p_api->open(g_timer0.p_ctrl, g_timer0.p_cfg);
    assert(FSP_SUCCESS == err);
    err = g_timer0.p_api->start(g_timer0.p_ctrl);
    assert(FSP_SUCCESS == err);
    /* 使能ADC的转换功能 */
    err = g_adc0.p_api->scanStart(g_adc0.p_ctrl);
    assert(FSP_SUCCESS == err);
}


void adc0_dma_callback(dmac_callback_args_t * p_args)
{
    adc_sample_cplt = true;
}
static void ADCWaitConvCplt(void)
{
    while(!adc_sample_cplt);
    adc_sample_cplt = false;
}

void ADCDrvRead(unsigned short *buffer, unsigned short num)
{
        /* 每次采样前将DMAC的目的地址和传输个数重置 */
        g_transfer0.p_cfg->p_info->p_dest = buffer;
        g_transfer0.p_cfg->p_info->length = num;
        fsp_err_t err = g_transfer0.p_api->reconfigure(g_transfer0.p_ctrl, g_transfer0.p_cfg->p_info);
        assert(FSP_SUCCESS == err);
        /* 开启定时器触发ADC采样 */
        err = g_timer0.p_api->start(g_timer0.p_ctrl);
        assert(FSP_SUCCESS == err);
        ADCWaitConvCplt();
        /* 采样结束后关闭定时器 */
        err = g_timer0.p_api->stop(g_timer0.p_ctrl);
        assert(FSP_SUCCESS == err);
}
/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
float Filter(float input) {
  float output = input;
  {
    static float z1, z2; // filter section state
    float x = output - (-0.55195385 * z1) - (0.60461714 * z2);
    output = 0.00223489 * x + (0.00446978 * z1) + (0.00223489 * z2);
    z2 = z1;
    z1 = x;
  }

  {
    static float z1, z2; // filter section state
    float x = output - (-0.86036562 * z1) - (0.63511954 * z2);
    output = 1.00000000 * x + (2.00000000 * z1) + (1.00000000 * z2);
    z2 = z1;
    z1 = x;
  }

  {
    static float z1, z2; // filter section state
    float x = output - (-0.37367240 * z1) - (0.81248708 * z2);
    output = 1.00000000 * x + (-2.00000000 * z1) + (1.00000000 * z2);
    z2 = z1;
    z1 = x;
  }

  {
    static float z1, z2; // filter section state
    float x = output - (-1.15601175 * z1) - (0.84761589 * z2);
    output = 1.00000000 * x + (-2.00000000 * z1) + (1.00000000 * z2);
    z2 = z1;
    z1 = x;
  }

  return output;
}

