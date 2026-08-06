#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "sdr_adapter.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    TEST_IIO_CONTEXT_CREATE = 0,
    TEST_IIO_CONTEXT_DESTROY,
    TEST_IIO_ATTR_WRITE,
    TEST_IIO_BUFFER_CREATE,
    TEST_IIO_BUFFER_DESTROY,
    TEST_IIO_BUFFER_REFILL,
    TEST_IIO_TIMEOUT_SET,
    TEST_IIO_BLOCKING_MODE_SET,
    TEST_IIO_BUFFER_CANCEL
};

typedef uint32_t (*test_get_counter_fn)(uint32_t counter);
typedef long long (*test_get_value_fn)(void);

static int load_function(void *module, const char *name,
                         void *function, size_t function_size)
{
    void *symbol = dlsym(module, name);
    if ((symbol == NULL) || (function_size != sizeof(symbol)))
    {
        return 0;
    }
    memcpy(function, &symbol, function_size);
    return 1;
}

static int16_t get_le16(const uint8_t *input)
{
    uint16_t bits = (uint16_t)((uint16_t)input[0] |
                               ((uint16_t)input[1] << 8U));
    return (int16_t)bits;
}

int main(int argc, char **argv)
{
    ra8p1_sdr_adapter_get_api_fn get_api = NULL;
    test_get_counter_fn get_counter = NULL;
    test_get_value_fn get_sample_rate = NULL;
    test_get_value_fn get_bandwidth = NULL;
    test_get_value_fn get_center = NULL;
    ra8p1_sdr_adapter_api_t api;
    ra8p1_sdr_adapter_config_t config;
    ra8p1_sdr_iq2_sample_t compatibility[8];
    uint8_t rx1[8U * 4U];
    void *plugin;
    void *mock;
    void *context = NULL;
    uint32_t index;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <adapter.so> <mock-libiio.so>\n", argv[0]);
        return 2;
    }
    if (setenv("RA8P1_LIBIIO_PATH", argv[2], 1) != 0)
    {
        return 3;
    }
    mock = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if ((mock == NULL) || (plugin == NULL))
    {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 4;
    }
    if (!load_function(plugin, RA8P1_SDR_ADAPTER_GET_API_SYMBOL,
                       &get_api, sizeof(get_api)) ||
        !load_function(mock, "test_iio_get_counter",
                       &get_counter, sizeof(get_counter)) ||
        !load_function(mock, "test_iio_get_sample_rate",
                       &get_sample_rate, sizeof(get_sample_rate)) ||
        !load_function(mock, "test_iio_get_bandwidth",
                       &get_bandwidth, sizeof(get_bandwidth)) ||
        !load_function(mock, "test_iio_get_center",
                       &get_center, sizeof(get_center)))
    {
        return 5;
    }
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    if ((get_api(RA8P1_SDR_ADAPTER_ABI_VERSION, &api) != 0) ||
        (api.struct_size != sizeof(api)) ||
        (api.capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2) ||
        (api.sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        (api.rx1_capture_le == NULL))
    {
        return 6;
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t)sizeof(config);
    config.abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    config.initial_center_frequency_hz = 2420000000ULL;
    config.sample_rate_hz = 60000000U;
    config.bandwidth_hz = 56000000U;
    config.rx_channels = 2U;
    if ((api.open(&context, &config) != 0) || (context == NULL) ||
        (get_counter(TEST_IIO_CONTEXT_CREATE) != 1U) ||
        (get_counter(TEST_IIO_BUFFER_CREATE) != 0U) ||
        (get_counter(TEST_IIO_ATTR_WRITE) != 3U) ||
        (get_sample_rate() != 60000000LL) ||
        (get_bandwidth() != 56000000LL) ||
        (get_center() != 2420000000LL))
    {
        return 7;
    }
    if ((api.set_rx(context, 2420000000ULL, 60000000U, 56000000U) != 0) ||
        (get_counter(TEST_IIO_ATTR_WRITE) != 3U))
    {
        return 8;
    }
    if (api.rx1_capture_le(context, rx1, 8U, 500U) != 0)
    {
        return 9;
    }
    for (index = 0U; index < 8U; index++)
    {
        if ((get_le16(rx1 + index * 4U) !=
             (int16_t)(-1000 - (int32_t)index)) ||
            (get_le16(rx1 + index * 4U + 2U) !=
             (int16_t)(1000 + (int32_t)index)))
        {
            return 10;
        }
    }
    if ((api.rx1_capture_le(context, rx1, 8U, 500U) != 0) ||
        (get_le16(rx1) != -1100) || (get_le16(rx1 + 2U) != 1100) ||
        (get_counter(TEST_IIO_BUFFER_CREATE) != 1U) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 2U) ||
        (get_counter(TEST_IIO_BLOCKING_MODE_SET) != 1U))
    {
        return 11;
    }
    if ((api.set_rx(context, 2464000000ULL, 60000000U, 56000000U) != 0) ||
        (get_center() != 2464000000LL) ||
        (get_counter(TEST_IIO_ATTR_WRITE) != 4U) ||
        (api.set_rx(context, 2500000000ULL, 60000000U, 56000000U) == 0))
    {
        return 12;
    }
    if ((api.rx_capture(context, compatibility, 8U, 500U) != 0) ||
        (compatibility[0].rx1_i != -1200) ||
        (compatibility[0].rx1_q != 1200) ||
        (compatibility[0].rx2_i != 0) ||
        (compatibility[0].rx2_q != 0) ||
        (get_counter(TEST_IIO_BUFFER_CREATE) != 1U) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 3U) ||
        (get_counter(TEST_IIO_TIMEOUT_SET) != 3U))
    {
        return 13;
    }
    if ((setenv("RA8P1_TEST_IIO_EAGAIN_ONCE", "1", 1) != 0) ||
        (api.rx1_capture_le(context, rx1, 8U, 500U) != 0) ||
        (unsetenv("RA8P1_TEST_IIO_EAGAIN_ONCE") != 0) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 5U))
    {
        return 14;
    }
    if ((setenv("RA8P1_TEST_IIO_EAGAIN_ALWAYS", "1", 1) != 0) ||
        (api.rx1_capture_le(context, rx1, 8U, 5U) == 0) ||
        (unsetenv("RA8P1_TEST_IIO_EAGAIN_ALWAYS") != 0) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 6U) ||
        (get_counter(TEST_IIO_BUFFER_DESTROY) != 1U) ||
        (get_counter(TEST_IIO_BUFFER_CANCEL) != 1U))
    {
        return 15;
    }
    if ((api.rx1_capture_le(context, rx1, 8U, 500U) != 0) ||
        (get_counter(TEST_IIO_BUFFER_CREATE) != 2U) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 7U) ||
        (get_counter(TEST_IIO_BLOCKING_MODE_SET) != 2U))
    {
        return 16;
    }
    if ((api.close(context) != 0) ||
        (get_counter(TEST_IIO_CONTEXT_DESTROY) != 1U) ||
        (get_counter(TEST_IIO_BUFFER_DESTROY) != 2U) ||
        (get_counter(TEST_IIO_BUFFER_CANCEL) != 2U))
    {
        return 17;
    }
    context = NULL;
    if ((setenv("RA8P1_LIBIIO_BUFFER_MODE", "recreate", 1) != 0) ||
        (api.open(&context, &config) != 0) || (context == NULL) ||
        (api.rx1_capture_le(context, rx1, 8U, 500U) != 0) ||
        (api.rx1_capture_le(context, rx1, 8U, 500U) != 0) ||
        (get_counter(TEST_IIO_BUFFER_CREATE) != 4U) ||
        (get_counter(TEST_IIO_BUFFER_DESTROY) != 3U) ||
        (get_counter(TEST_IIO_BUFFER_REFILL) != 9U) ||
        (get_counter(TEST_IIO_BLOCKING_MODE_SET) != 2U))
    {
        return 18;
    }
    if ((api.close(context) != 0) ||
        (get_counter(TEST_IIO_CONTEXT_DESTROY) != 2U) ||
        (get_counter(TEST_IIO_BUFFER_DESTROY) != 4U) ||
        (get_counter(TEST_IIO_BUFFER_CANCEL) != 3U))
    {
        return 19;
    }
    (void)dlclose(plugin);
    (void)dlclose(mock);
    puts("persistent libiio adapter mock test passed");
    return 0;
}
