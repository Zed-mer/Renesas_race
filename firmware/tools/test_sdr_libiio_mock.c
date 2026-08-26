/* Offline libiio 0.25-shaped mock for sdr_adapter_libiio.c. */

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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
    TEST_IIO_BUFFER_CANCEL,
    TEST_IIO_COUNTER_COUNT
};

struct iio_context
{
    uint32_t tag;
};

struct iio_device
{
    const char *name;
};

struct iio_channel
{
    const char *name;
    bool output;
    bool enabled;
};

struct iio_buffer
{
    size_t samples;
    uint8_t *bytes;
};

static struct iio_context g_context = { 0xC07E5701U };
static struct iio_device g_phy = { "ad9361-phy" };
static struct iio_device g_rx = { "cf-ad9361-lpc" };
static struct iio_channel g_phy_rx = { "voltage0", false, false };
static struct iio_channel g_rx_lo = { "altvoltage0", true, false };
static struct iio_channel g_rx_i = { "voltage0", false, false };
static struct iio_channel g_rx_q = { "voltage1", false, false };
static uint32_t g_counters[TEST_IIO_COUNTER_COUNT];
static long long g_sample_rate;
static long long g_bandwidth;
static long long g_center;
static int g_poll_pipe[2] = { -1, -1 };
static uint32_t g_eagain_once_consumed;

static int ensure_poll_pipe(void)
{
    if (g_poll_pipe[0] >= 0)
    {
        return 0;
    }
    if (pipe(g_poll_pipe) != 0)
    {
        return -1;
    }
    (void)fcntl(g_poll_pipe[0], F_SETFL,
                fcntl(g_poll_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(g_poll_pipe[1], F_SETFL,
                fcntl(g_poll_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

static void drain_poll_pipe(void)
{
    uint8_t byte;
    if (g_poll_pipe[0] < 0)
    {
        return;
    }
    while (read(g_poll_pipe[0], &byte, sizeof(byte)) > 0)
    {
    }
}

static void put_le16(uint8_t *output, int16_t value)
{
    uint16_t bits = (uint16_t)value;
    output[0] = (uint8_t)bits;
    output[1] = (uint8_t)(bits >> 8U);
}

struct iio_context *iio_create_local_context(void)
{
    g_counters[TEST_IIO_CONTEXT_CREATE]++;
    return &g_context;
}

void iio_context_destroy(struct iio_context *context)
{
    if (context == &g_context)
    {
        g_counters[TEST_IIO_CONTEXT_DESTROY]++;
    }
}

int iio_context_set_timeout(struct iio_context *context,
                            unsigned int timeout_ms)
{
    if ((context != &g_context) || (timeout_ms == 0U))
    {
        return -1;
    }
    g_counters[TEST_IIO_TIMEOUT_SET]++;
    return 0;
}

struct iio_device *iio_context_find_device(
    const struct iio_context *context, const char *name)
{
    if ((context != &g_context) || (name == NULL))
    {
        return NULL;
    }
    if (strcmp(name, g_phy.name) == 0)
    {
        return &g_phy;
    }
    if (strcmp(name, g_rx.name) == 0)
    {
        return &g_rx;
    }
    return NULL;
}

struct iio_channel *iio_device_find_channel(
    const struct iio_device *device, const char *name, bool output)
{
    if ((device == NULL) || (name == NULL))
    {
        return NULL;
    }
    if (device == &g_phy)
    {
        if (!output && (strcmp(name, "voltage0") == 0))
        {
            return &g_phy_rx;
        }
        if (output && (strcmp(name, "altvoltage0") == 0))
        {
            return &g_rx_lo;
        }
    }
    if ((device == &g_rx) && !output)
    {
        if (strcmp(name, "voltage0") == 0)
        {
            return &g_rx_i;
        }
        if (strcmp(name, "voltage1") == 0)
        {
            return &g_rx_q;
        }
    }
    return NULL;
}

int iio_channel_attr_write_longlong(
    const struct iio_channel *channel, const char *attribute, long long value)
{
    if ((channel == NULL) || (attribute == NULL))
    {
        return -1;
    }
    if ((channel == &g_phy_rx) &&
        (strcmp(attribute, "sampling_frequency") == 0))
    {
        g_sample_rate = value;
    }
    else if ((channel == &g_phy_rx) &&
             (strcmp(attribute, "rf_bandwidth") == 0))
    {
        g_bandwidth = value;
    }
    else if ((channel == &g_rx_lo) &&
             (strcmp(attribute, "frequency") == 0))
    {
        g_center = value;
    }
    else
    {
        return -2;
    }
    g_counters[TEST_IIO_ATTR_WRITE]++;
    return 0;
}

void iio_channel_enable(struct iio_channel *channel)
{
    if (channel != NULL)
    {
        channel->enabled = true;
    }
}

void iio_channel_disable(struct iio_channel *channel)
{
    if (channel != NULL)
    {
        channel->enabled = false;
    }
}

struct iio_buffer *iio_device_create_buffer(
    const struct iio_device *device, size_t samples_count, bool cyclic)
{
    struct iio_buffer *buffer;
    if ((device != &g_rx) || (samples_count == 0U) || cyclic ||
        !g_rx_i.enabled || !g_rx_q.enabled)
    {
        return NULL;
    }
    buffer = (struct iio_buffer *)calloc(1U, sizeof(*buffer));
    if (buffer == NULL)
    {
        return NULL;
    }
    buffer->bytes = (uint8_t *)malloc(samples_count * 4U);
    if (buffer->bytes == NULL)
    {
        free(buffer);
        return NULL;
    }
    buffer->samples = samples_count;
    g_counters[TEST_IIO_BUFFER_CREATE]++;
    return buffer;
}

void iio_buffer_destroy(struct iio_buffer *buffer)
{
    if (buffer != NULL)
    {
        free(buffer->bytes);
        free(buffer);
        g_counters[TEST_IIO_BUFFER_DESTROY]++;
    }
}

ssize_t iio_buffer_refill(struct iio_buffer *buffer)
{
    size_t index;
    uint32_t refill;
    const char *eagain_once;
    const char *eagain_always;
    if (buffer == NULL)
    {
        return -1;
    }
    refill = g_counters[TEST_IIO_BUFFER_REFILL]++;
    eagain_once = getenv("RA8P1_TEST_IIO_EAGAIN_ONCE");
    eagain_always = getenv("RA8P1_TEST_IIO_EAGAIN_ALWAYS");
    if ((eagain_always != NULL) && (eagain_always[0] == '1'))
    {
        drain_poll_pipe();
        return -EAGAIN;
    }
    if ((eagain_once != NULL) && (eagain_once[0] == '1') &&
        (g_eagain_once_consumed == 0U))
    {
        uint8_t byte = 1U;
        g_eagain_once_consumed = 1U;
        if ((ensure_poll_pipe() != 0) ||
            (write(g_poll_pipe[1], &byte, sizeof(byte)) != sizeof(byte)))
        {
            return -EIO;
        }
        return -EAGAIN;
    }
    drain_poll_pipe();
    for (index = 0U; index < buffer->samples; index++)
    {
        int16_t i_value = (int16_t)(-1000 - (int32_t)index -
                                    (int32_t)(refill * 100U));
        int16_t q_value = (int16_t)(1000 + (int32_t)index +
                                    (int32_t)(refill * 100U));
        put_le16(buffer->bytes + index * 4U, i_value);
        put_le16(buffer->bytes + index * 4U + 2U, q_value);
    }
    return (ssize_t)(buffer->samples * 4U);
}

int iio_buffer_get_poll_fd(const struct iio_buffer *buffer)
{
    return ((buffer != NULL) && (ensure_poll_pipe() == 0)) ?
        g_poll_pipe[0] : -1;
}

int iio_buffer_set_blocking_mode(struct iio_buffer *buffer, bool blocking)
{
    if ((buffer == NULL) || blocking)
    {
        return -1;
    }
    g_counters[TEST_IIO_BLOCKING_MODE_SET]++;
    return 0;
}

void iio_buffer_cancel(struct iio_buffer *buffer)
{
    if (buffer != NULL)
    {
        drain_poll_pipe();
        g_counters[TEST_IIO_BUFFER_CANCEL]++;
    }
}

void *iio_buffer_first(const struct iio_buffer *buffer,
                       const struct iio_channel *channel)
{
    if ((buffer == NULL) || (channel == NULL))
    {
        return NULL;
    }
    if (channel == &g_rx_i)
    {
        return buffer->bytes;
    }
    if (channel == &g_rx_q)
    {
        return buffer->bytes + 2U;
    }
    return NULL;
}

ptrdiff_t iio_buffer_step(const struct iio_buffer *buffer)
{
    return buffer != NULL ? 4 : 0;
}

void *iio_buffer_end(const struct iio_buffer *buffer)
{
    return buffer != NULL ? buffer->bytes + buffer->samples * 4U : NULL;
}

uint32_t test_iio_get_counter(uint32_t counter)
{
    return counter < TEST_IIO_COUNTER_COUNT ? g_counters[counter] : UINT32_MAX;
}

long long test_iio_get_sample_rate(void)
{
    return g_sample_rate;
}

long long test_iio_get_bandwidth(void)
{
    return g_bandwidth;
}

long long test_iio_get_center(void)
{
    return g_center;
}
