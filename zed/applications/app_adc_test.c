#include "app_adc_test.h"
#include "drv_adc0.h"
#include <math.h>
#include <stdio.h>

#define ENVELOPE_BUFFER_SIZE  16

/* 用一个很小的环形缓冲区对整流后的信号做滑动平均，得到更稳定的包络。 */
static uint16_t s_adc_buffer[1] = {0};
static float    s_envelope_buffer[ENVELOPE_BUFFER_SIZE] = {0.0f};
static int      s_envelope_index = 0;
static float    s_envelope_sum = 0.0f;

static float get_envelope(float sample);

void app_test(void)
{
    /* 先完成 ADC 驱动初始化，然后在死循环里持续输出观测数据，便于串口调试。 */
    adcdrvinit();

    while (1)
    {
        float filtered_value;
        float envelope_value;

        ADCDrvRead(s_adc_buffer, 1);
        filtered_value = Filter((float) s_adc_buffer[0]);
        envelope_value = get_envelope(fabsf(filtered_value));
        printf("Filtered: %.2f, %.2f\r\n", filtered_value, envelope_value);
    }
}

static float get_envelope(float sample)
{
    /* 这里实现的是最简单的包络检测：对绝对值后的采样做滑动平均。 */
    s_envelope_sum -= s_envelope_buffer[s_envelope_index];
    s_envelope_sum += sample;
    s_envelope_buffer[s_envelope_index] = sample;

    s_envelope_index = (s_envelope_index + 1) % ENVELOPE_BUFFER_SIZE;

    return s_envelope_sum / ENVELOPE_BUFFER_SIZE;
}
