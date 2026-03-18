#include "app_adc_test.h"
#include "drv_adc0.h"
#include <math.h>
#include <stdio.h>

#define ENVELOPE_BUFFER_SIZE  16

static uint16_t s_adc_buffer[1] = {0};
static float    s_envelope_buffer[ENVELOPE_BUFFER_SIZE] = {0.0f};
static int      s_envelope_index = 0;
static float    s_envelope_sum = 0.0f;

static float get_envelope(float sample);

void app_test(void)
{
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
    s_envelope_sum -= s_envelope_buffer[s_envelope_index];
    s_envelope_sum += sample;
    s_envelope_buffer[s_envelope_index] = sample;

    s_envelope_index = (s_envelope_index + 1) % ENVELOPE_BUFFER_SIZE;

    return s_envelope_sum / ENVELOPE_BUFFER_SIZE;
}
