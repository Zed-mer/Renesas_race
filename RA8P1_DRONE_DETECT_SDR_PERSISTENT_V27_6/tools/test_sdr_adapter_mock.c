/* Test-only RAW_2R2T adapter.  It never claims or accesses SDR hardware. */

#include "sdr_adapter.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define TEST_SDR_EXPORT __declspec(dllexport)
#else
#define TEST_SDR_EXPORT __attribute__((visibility("default")))
#endif

typedef struct st_test_sdr_context
{
    uint32_t opened;
    uint32_t sample_cursor;
    uint64_t center_hz;
} test_sdr_context_t;

static test_sdr_context_t g_test_context;

static void put_le16(uint8_t *buffer, int16_t value)
{
    uint16_t bits = (uint16_t) value;
    buffer[0] = (uint8_t) bits;
    buffer[1] = (uint8_t) (bits >> 8U);
}

static int32_t test_open(void **context, const ra8p1_sdr_adapter_config_t *config)
{
    if ((context == NULL) || (config == NULL) || (config->sample_rate_hz != 60000000U) ||
        (config->bandwidth_hz != 56000000U) || (config->rx_channels != 2U))
    {
        return -1;
    }
    memset(&g_test_context, 0, sizeof(g_test_context));
    g_test_context.opened = 1U;
    g_test_context.center_hz = config->initial_center_frequency_hz;
    *context = &g_test_context;
    return 0;
}

static int32_t test_set_rx(void *context, uint64_t lo_hz,
                           uint32_t sample_rate_hz, uint32_t bandwidth_hz)
{
    test_sdr_context_t *test = (test_sdr_context_t *) context;
    if ((test != &g_test_context) || (test->opened == 0U) ||
        (sample_rate_hz != 60000000U) || (bandwidth_hz != 56000000U))
    {
        return -1;
    }
    test->center_hz = lo_hz;
    test->sample_cursor = 0U;
    return 0;
}

static int32_t test_capture(void *context, void *buffer,
                            uint32_t sample_count, uint32_t timeout_ms)
{
    test_sdr_context_t *test = (test_sdr_context_t *) context;
    uint8_t *raw = (uint8_t *) buffer;
    uint32_t n;
    if ((test != &g_test_context) || (test->opened == 0U) ||
        (buffer == NULL) || (sample_count == 0U) || (timeout_ms == 0U))
    {
        return -1;
    }
    for (n = 0U; n < sample_count; n++)
    {
        uint32_t sample = test->sample_cursor + n;
        uint8_t *record = raw + (size_t) n * 8U;
        put_le16(record + 0U, (int16_t) (1000 + (sample & 0x3FFU))); /* Q1 */
        put_le16(record + 2U, (int16_t) (-1000 - (int32_t) (sample & 0x3FFU))); /* I1 */
        put_le16(record + 4U, (int16_t) (2000 + (sample & 0x3FFU))); /* Q2 */
        put_le16(record + 6U, (int16_t) (-2000 - (int32_t) (sample & 0x3FFU))); /* I2 */
    }
    test->sample_cursor += sample_count;
    return 0;
}

static int32_t test_close(void *context)
{
    test_sdr_context_t *test = (test_sdr_context_t *) context;
    if ((test != &g_test_context) || (test->opened == 0U))
    {
        return -1;
    }
    memset(test, 0, sizeof(*test));
    return 0;
}

TEST_SDR_EXPORT int32_t ra8p1_sdr_adapter_get_api_v1(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api)
{
    if ((requested_abi != RA8P1_SDR_ADAPTER_ABI_VERSION) || (api == NULL) ||
        (api->struct_size < sizeof(*api)))
    {
        return -1;
    }
    memset(api, 0, sizeof(*api));
    api->struct_size = (uint32_t) sizeof(*api);
    api->abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    api->capture_format = RA8P1_SDR_CAPTURE_RAW_2R2T_LE;
    api->sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api->name = "TEST ONLY raw 2R2T adapter";
    api->open = test_open;
    api->set_rx = test_set_rx;
    api->rx_capture = test_capture;
    api->close = test_close;
    return 0;
}
