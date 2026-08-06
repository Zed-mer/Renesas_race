/*
 * Persistent local-libiio adapter for the Pluto/Zynq SDR root filesystem.
 *
 * This plugin is loaded by sdr_iq_udp_stream --sdr-lib.  It deliberately
 * uses dlopen/dlsym and a minimal libiio 0.25 ABI declaration because the
 * target image contains /usr/lib/libiio.so.0 but no development headers.
 * No rootfs, network, boot, or firmware setting is modified.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sdr_adapter.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "../shared/analysis_contract.h"

#if defined(_WIN32)
#error "sdr_adapter_libiio.c is a POSIX/Pluto plugin"
#endif

#define RA8P1_LIBIIO_DEFAULT_SO       "libiio.so.0"
#define RA8P1_LIBIIO_PATH_ENV         "RA8P1_LIBIIO_PATH"
#define RA8P1_LIBIIO_BUFFER_MODE_ENV  "RA8P1_LIBIIO_BUFFER_MODE"
#define RA8P1_LIBIIO_PHY_DEVICE       "ad9361-phy"
#define RA8P1_LIBIIO_RX_DEVICE        "cf-ad9361-lpc"
#define RA8P1_LIBIIO_PHY_RX_CHANNEL   "voltage0"
#define RA8P1_LIBIIO_RX_I_CHANNEL     "voltage0"
#define RA8P1_LIBIIO_RX_Q_CHANNEL     "voltage1"
#define RA8P1_LIBIIO_RX_LO_CHANNEL    "altvoltage0"

#define RA8P1_LIBIIO_EINVAL           (-2101)
#define RA8P1_LIBIIO_EDLOPEN          (-2102)
#define RA8P1_LIBIIO_ESYMBOL          (-2103)
#define RA8P1_LIBIIO_ECONTEXT         (-2104)
#define RA8P1_LIBIIO_EDEVICE          (-2105)
#define RA8P1_LIBIIO_ECHANNEL         (-2106)
#define RA8P1_LIBIIO_EBUFFER          (-2107)
#define RA8P1_LIBIIO_ELAYOUT          (-2108)
#define RA8P1_LIBIIO_ETIMEOUT         (-2109)

typedef enum e_ra8p1_libiio_buffer_mode
{
    RA8P1_LIBIIO_BUFFER_AUTO = 0,
    RA8P1_LIBIIO_BUFFER_PERSISTENT,
    RA8P1_LIBIIO_BUFFER_RECREATE
} ra8p1_libiio_buffer_mode_t;

/* Opaque libiio 0.25 types. */
struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;

typedef struct st_ra8p1_libiio_api
{
    struct iio_context *(*create_local_context)(void);
    void (*context_destroy)(struct iio_context *context);
    int (*context_set_timeout)(struct iio_context *context,
                               unsigned int timeout_ms);
    struct iio_device *(*context_find_device)(const struct iio_context *context,
                                              const char *name);
    struct iio_channel *(*device_find_channel)(const struct iio_device *device,
                                               const char *name, bool output);
    const char *(*channel_find_attr)(const struct iio_channel *channel,
                                     const char *name);
    int (*channel_attr_write_longlong)(const struct iio_channel *channel,
                                       const char *attribute, long long value);
    void (*channel_enable)(struct iio_channel *channel);
    void (*channel_disable)(struct iio_channel *channel);
    struct iio_buffer *(*device_create_buffer)(const struct iio_device *device,
                                               size_t samples_count,
                                               bool cyclic);
    void (*buffer_destroy)(struct iio_buffer *buffer);
    ssize_t (*buffer_refill)(struct iio_buffer *buffer);
    void *(*buffer_first)(const struct iio_buffer *buffer,
                          const struct iio_channel *channel);
    ptrdiff_t (*buffer_step)(const struct iio_buffer *buffer);
    void *(*buffer_end)(const struct iio_buffer *buffer);
    int (*buffer_get_poll_fd)(const struct iio_buffer *buffer);
    int (*buffer_set_blocking_mode)(struct iio_buffer *buffer, bool blocking);
    void (*buffer_cancel)(struct iio_buffer *buffer);
} ra8p1_libiio_api_t;

typedef struct st_ra8p1_libiio_context
{
    void *module;
    ra8p1_libiio_api_t io;
    struct iio_context *iio_context;
    struct iio_device *phy_device;
    struct iio_device *rx_device;
    struct iio_channel *phy_rx_channel;
    struct iio_channel *rx_lo_channel;
    struct iio_channel *rx_i_channel;
    struct iio_channel *rx_q_channel;
    struct iio_buffer *rx_buffer;
    uint64_t center_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t buffer_samples;
    int buffer_poll_fd;
    ra8p1_libiio_buffer_mode_t requested_buffer_mode;
    uint32_t poll_buffer_enabled;
    uint32_t recreate_each_capture;
    uint32_t opened;
    ra8p1_sdr_adapter_status_t status;
} ra8p1_libiio_context_t;

static ra8p1_libiio_context_t g_libiio_context;

#if defined(__GNUC__)
#define RA8P1_SDR_ADAPTER_EXPORT __attribute__((visibility("default")))
#else
#define RA8P1_SDR_ADAPTER_EXPORT
#endif

static int ra8p1_libiio_load_symbol(void *module, const char *name,
                                    void *destination, size_t destination_size,
                                    bool required)
{
    void *symbol;
    const char *error;

    (void) dlerror();
    symbol = dlsym(module, name);
    error = dlerror();
    if ((error != NULL) || (symbol == NULL))
    {
        if (required)
        {
            return RA8P1_LIBIIO_ESYMBOL;
        }
        memset(destination, 0, destination_size);
        return 0;
    }
    if (destination_size != sizeof(symbol))
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    memcpy(destination, &symbol, destination_size);
    return 0;
}

#define RA8P1_LIBIIO_LOAD_REQUIRED(context, member, symbol_name)                 \
    do                                                                           \
    {                                                                            \
        int load_status = ra8p1_libiio_load_symbol(                              \
            (context)->module, (symbol_name), &(context)->io.member,             \
            sizeof((context)->io.member), true);                                 \
        if (load_status != 0)                                                     \
        {                                                                        \
            return load_status;                                                  \
        }                                                                        \
    } while (0)

static int32_t ra8p1_libiio_load(ra8p1_libiio_context_t *context)
{
    const char *path = getenv(RA8P1_LIBIIO_PATH_ENV);

    if ((path == NULL) || (path[0] == '\0'))
    {
        path = RA8P1_LIBIIO_DEFAULT_SO;
    }
    context->module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (context->module == NULL)
    {
        return RA8P1_LIBIIO_EDLOPEN;
    }

    RA8P1_LIBIIO_LOAD_REQUIRED(context, create_local_context,
                               "iio_create_local_context");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, context_destroy,
                               "iio_context_destroy");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, context_find_device,
                               "iio_context_find_device");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, device_find_channel,
                               "iio_device_find_channel");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, channel_attr_write_longlong,
                               "iio_channel_attr_write_longlong");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, channel_enable,
                               "iio_channel_enable");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, channel_disable,
                               "iio_channel_disable");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, device_create_buffer,
                               "iio_device_create_buffer");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, buffer_destroy,
                               "iio_buffer_destroy");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, buffer_refill,
                               "iio_buffer_refill");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, buffer_first,
                               "iio_buffer_first");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, buffer_step,
                               "iio_buffer_step");
    RA8P1_LIBIIO_LOAD_REQUIRED(context, buffer_end,
                               "iio_buffer_end");

    if (ra8p1_libiio_load_symbol(
            context->module, "iio_channel_find_attr",
            &context->io.channel_find_attr,
            sizeof(context->io.channel_find_attr), false) != 0)
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    if (ra8p1_libiio_load_symbol(
        context->module, "iio_context_set_timeout",
        &context->io.context_set_timeout,
        sizeof(context->io.context_set_timeout), false) != 0)
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    if (ra8p1_libiio_load_symbol(
        context->module, "iio_buffer_get_poll_fd",
        &context->io.buffer_get_poll_fd,
        sizeof(context->io.buffer_get_poll_fd), false) != 0)
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    if (ra8p1_libiio_load_symbol(
        context->module, "iio_buffer_set_blocking_mode",
        &context->io.buffer_set_blocking_mode,
        sizeof(context->io.buffer_set_blocking_mode), false) != 0)
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    return ra8p1_libiio_load_symbol(
        context->module, "iio_buffer_cancel",
        &context->io.buffer_cancel,
        sizeof(context->io.buffer_cancel), false);
}

static int32_t ra8p1_libiio_parse_buffer_mode(
    ra8p1_libiio_context_t *context)
{
    const char *mode = getenv(RA8P1_LIBIIO_BUFFER_MODE_ENV);

    context->requested_buffer_mode = RA8P1_LIBIIO_BUFFER_AUTO;
    if ((mode == NULL) || (mode[0] == '\0') || (strcmp(mode, "auto") == 0))
    {
        return 0;
    }
    if (strcmp(mode, "persistent") == 0)
    {
        context->requested_buffer_mode = RA8P1_LIBIIO_BUFFER_PERSISTENT;
        return 0;
    }
    if (strcmp(mode, "recreate") == 0)
    {
        context->requested_buffer_mode = RA8P1_LIBIIO_BUFFER_RECREATE;
        return 0;
    }
    return RA8P1_LIBIIO_EINVAL;
}

static void ra8p1_libiio_destroy_buffer(ra8p1_libiio_context_t *context,
                                         bool cancel)
{
    if (context->rx_buffer != NULL)
    {
        if (cancel && (context->io.buffer_cancel != NULL))
        {
            context->io.buffer_cancel(context->rx_buffer);
        }
        context->io.buffer_destroy(context->rx_buffer);
    }
    context->rx_buffer = NULL;
    context->buffer_samples = 0U;
    context->buffer_poll_fd = -1;
    context->poll_buffer_enabled = 0U;
}

static void ra8p1_libiio_release(ra8p1_libiio_context_t *context)
{
    if ((context->rx_buffer != NULL) && (context->io.buffer_destroy != NULL))
    {
        ra8p1_libiio_destroy_buffer(context, true);
    }
    if (context->io.channel_disable != NULL)
    {
        if (context->rx_i_channel != NULL)
        {
            context->io.channel_disable(context->rx_i_channel);
        }
        if (context->rx_q_channel != NULL)
        {
            context->io.channel_disable(context->rx_q_channel);
        }
    }
    if ((context->iio_context != NULL) &&
        (context->io.context_destroy != NULL))
    {
        context->io.context_destroy(context->iio_context);
    }
    context->iio_context = NULL;
    if (context->module != NULL)
    {
        (void) dlclose(context->module);
    }
    memset(context, 0, sizeof(*context));
}

static int ra8p1_libiio_center_index(uint64_t center_hz, uint32_t *result)
{
    static const uint64_t centers[] = {
        2420000000ULL, 2464000000ULL, 5760000000ULL, 5816000000ULL
    };
    uint32_t index;

    for (index = 0U; index <
         (uint32_t)(sizeof(centers) / sizeof(centers[0])); index++)
    {
        if (center_hz == centers[index])
        {
            if (result != NULL)
            {
                *result = index;
            }
            return 1;
        }
    }
    return 0;
}

static uint64_t ra8p1_libiio_monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0ULL;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int32_t ra8p1_libiio_prepare_fastlock(
    ra8p1_libiio_context_t *context, uint64_t restore_center_hz)
{
    static const uint64_t centers[] = {
        2420000000ULL, 2464000000ULL, 5760000000ULL, 5816000000ULL
    };
    uint32_t restore_index = 0U;
    uint32_t index;
    int status;

    context->status.fastlock_profiles = 0U;
    if ((context->io.channel_find_attr == NULL) ||
        (context->io.channel_find_attr(
             context->rx_lo_channel, "fastlock_store") == NULL) ||
        (context->io.channel_find_attr(
             context->rx_lo_channel, "fastlock_recall") == NULL))
    {
        context->status.flags |=
            RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK;
        context->status.fallback_count++;
        return 0;
    }
    context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_SUPPORTED;
    (void)ra8p1_libiio_center_index(restore_center_hz, &restore_index);
    for (index = 0U; index <
         (uint32_t)(sizeof(centers) / sizeof(centers[0])); index++)
    {
        status = context->io.channel_attr_write_longlong(
            context->rx_lo_channel, "frequency", (long long)centers[index]);
        if (status < 0)
        {
            goto fallback;
        }
        status = context->io.channel_attr_write_longlong(
            context->rx_lo_channel, "fastlock_store", (long long)index);
        if (status < 0)
        {
            goto fallback;
        }
        context->status.fastlock_profiles++;
    }
    status = context->io.channel_attr_write_longlong(
        context->rx_lo_channel, "fastlock_recall", (long long)restore_index);
    if (status < 0)
    {
        goto fallback;
    }
    context->center_hz = restore_center_hz;
    context->status.center_frequency_hz = restore_center_hz;
    context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY;
    return 0;

fallback:
    context->status.flags &= ~RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY;
    context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK;
    context->status.fastlock_profiles = 0U;
    context->status.fallback_count++;
    status = context->io.channel_attr_write_longlong(
        context->rx_lo_channel, "frequency", (long long)restore_center_hz);
    if (status >= 0)
    {
        context->center_hz = restore_center_hz;
        context->status.center_frequency_hz = restore_center_hz;
        return 0;
    }
    return status;
}

static int32_t ra8p1_libiio_set_rx(void *opaque, uint64_t center_hz,
                                   uint32_t sample_rate_hz,
                                   uint32_t bandwidth_hz)
{
    ra8p1_libiio_context_t *context =
        (ra8p1_libiio_context_t *) opaque;
    uint32_t center_index = 0U;
    int status;

    if ((context != &g_libiio_context) || (context->opened == 0U) ||
        !ra8p1_libiio_center_index(center_hz, &center_index) ||
        (sample_rate_hz != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    context->status.flags &=
        ~RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK;
    context->status.tune_start_ns = ra8p1_libiio_monotonic_ns();
    context->status.tune_count++;
    if (context->sample_rate_hz != sample_rate_hz)
    {
        status = context->io.channel_attr_write_longlong(
            context->phy_rx_channel, "sampling_frequency",
            (long long) sample_rate_hz);
        if (status < 0)
        {
            goto complete;
        }
        context->sample_rate_hz = sample_rate_hz;
    }
    if (context->bandwidth_hz != bandwidth_hz)
    {
        status = context->io.channel_attr_write_longlong(
            context->phy_rx_channel, "rf_bandwidth",
            (long long) bandwidth_hz);
        if (status < 0)
        {
            goto complete;
        }
        context->bandwidth_hz = bandwidth_hz;
    }
    if (context->center_hz != center_hz)
    {
        if ((context->status.flags &
             RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY) != 0U)
        {
            status = context->io.channel_attr_write_longlong(
                context->rx_lo_channel, "fastlock_recall",
                (long long)center_index);
            if (status >= 0)
            {
                context->status.flags |=
                    RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK;
                context->status.fastlock_recall_count++;
            }
            else
            {
                context->status.flags &=
                    ~RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY;
                context->status.flags |=
                    RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK;
                context->status.fallback_count++;
                status = context->io.channel_attr_write_longlong(
                    context->rx_lo_channel, "frequency",
                    (long long)center_hz);
            }
        }
        else
        {
            status = context->io.channel_attr_write_longlong(
                context->rx_lo_channel, "frequency", (long long)center_hz);
        }
        if (status < 0)
        {
            goto complete;
        }
        context->center_hz = center_hz;
    }
    status = 0;

complete:
    context->status.center_frequency_hz = context->center_hz;
    context->status.last_tune_status = status;
    context->status.tune_complete_ns = ra8p1_libiio_monotonic_ns();
    return status;
}

static int32_t ra8p1_libiio_open(
    void **opaque, const ra8p1_sdr_adapter_config_t *config)
{
    ra8p1_libiio_context_t *context = &g_libiio_context;
    int32_t status;

    if ((opaque == NULL) || (config == NULL) ||
        (config->struct_size < sizeof(*config)) ||
        (config->abi_version != RA8P1_SDR_ADAPTER_ABI_VERSION) ||
        (config->rx_channels != RA8P1_SDR_ADAPTER_RX_CHANNELS) ||
        (context->opened != 0U))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    *opaque = NULL;
    memset(context, 0, sizeof(*context));
    context->buffer_poll_fd = -1;
    status = ra8p1_libiio_parse_buffer_mode(context);
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_libiio_load(context);
    if (status != 0)
    {
        ra8p1_libiio_release(context);
        return status;
    }
    context->iio_context = context->io.create_local_context();
    if (context->iio_context == NULL)
    {
        ra8p1_libiio_release(context);
        return RA8P1_LIBIIO_ECONTEXT;
    }
    context->phy_device = context->io.context_find_device(
        context->iio_context, RA8P1_LIBIIO_PHY_DEVICE);
    context->rx_device = context->io.context_find_device(
        context->iio_context, RA8P1_LIBIIO_RX_DEVICE);
    if ((context->phy_device == NULL) || (context->rx_device == NULL))
    {
        ra8p1_libiio_release(context);
        return RA8P1_LIBIIO_EDEVICE;
    }
    context->phy_rx_channel = context->io.device_find_channel(
        context->phy_device, RA8P1_LIBIIO_PHY_RX_CHANNEL, false);
    context->rx_lo_channel = context->io.device_find_channel(
        context->phy_device, RA8P1_LIBIIO_RX_LO_CHANNEL, true);
    context->rx_i_channel = context->io.device_find_channel(
        context->rx_device, RA8P1_LIBIIO_RX_I_CHANNEL, false);
    context->rx_q_channel = context->io.device_find_channel(
        context->rx_device, RA8P1_LIBIIO_RX_Q_CHANNEL, false);
    if ((context->phy_rx_channel == NULL) ||
        (context->rx_lo_channel == NULL) ||
        (context->rx_i_channel == NULL) ||
        (context->rx_q_channel == NULL))
    {
        ra8p1_libiio_release(context);
        return RA8P1_LIBIIO_ECHANNEL;
    }
    context->io.channel_enable(context->rx_i_channel);
    context->io.channel_enable(context->rx_q_channel);
    context->opened = 1U;
    context->status.struct_size = (uint32_t)sizeof(context->status);
    context->status.version = RA8P1_SDR_ADAPTER_STATUS_VERSION;
    status = ra8p1_libiio_set_rx(context,
                                  config->initial_center_frequency_hz,
                                  config->sample_rate_hz,
                                  config->bandwidth_hz);
    if (status != 0)
    {
        ra8p1_libiio_release(context);
        return status;
    }
    status = ra8p1_libiio_prepare_fastlock(
        context, config->initial_center_frequency_hz);
    if (status != 0)
    {
        ra8p1_libiio_release(context);
        return status;
    }
    *opaque = context;
    return 0;
}

static int32_t ra8p1_libiio_ensure_buffer(
    ra8p1_libiio_context_t *context, uint32_t sample_count)
{
    const bool poll_api_available =
        (context->io.buffer_get_poll_fd != NULL) &&
        (context->io.buffer_set_blocking_mode != NULL);

    if ((context->requested_buffer_mode == RA8P1_LIBIIO_BUFFER_PERSISTENT) &&
        !poll_api_available)
    {
        return RA8P1_LIBIIO_ESYMBOL;
    }
    context->recreate_each_capture =
        ((context->requested_buffer_mode == RA8P1_LIBIIO_BUFFER_RECREATE) ||
         ((context->requested_buffer_mode == RA8P1_LIBIIO_BUFFER_AUTO) &&
          !poll_api_available)) ? 1U : 0U;
    if ((context->recreate_each_capture != 0U) &&
        (context->rx_buffer != NULL))
    {
        ra8p1_libiio_destroy_buffer(context, false);
    }
    if ((context->rx_buffer != NULL) &&
        (context->buffer_samples == sample_count))
    {
        return 0;
    }
    if (context->rx_buffer != NULL)
    {
        ra8p1_libiio_destroy_buffer(context, false);
    }
    context->rx_buffer = context->io.device_create_buffer(
        context->rx_device, (size_t) sample_count, false);
    if (context->rx_buffer == NULL)
    {
        return RA8P1_LIBIIO_EBUFFER;
    }
    context->buffer_samples = sample_count;
    if (poll_api_available &&
        (context->requested_buffer_mode != RA8P1_LIBIIO_BUFFER_RECREATE))
    {
        const int blocking_status =
            context->io.buffer_set_blocking_mode(context->rx_buffer, false);
        if (blocking_status < 0)
        {
            ra8p1_libiio_destroy_buffer(context, false);
            return blocking_status;
        }
        context->buffer_poll_fd =
            context->io.buffer_get_poll_fd(context->rx_buffer);
        if (context->buffer_poll_fd < 0)
        {
            const int poll_fd_status = context->buffer_poll_fd;
            ra8p1_libiio_destroy_buffer(context, false);
            return poll_fd_status;
        }
        context->poll_buffer_enabled = 1U;
    }
    return 0;
}

static int ra8p1_libiio_poll_until_ready(int fd, uint32_t timeout_ms)
{
    struct pollfd descriptor;
    uint64_t deadline_ns;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    deadline_ns = ra8p1_libiio_monotonic_ns() +
        (uint64_t)timeout_ms * 1000000ULL;
    for (;;)
    {
        const uint64_t now_ns = ra8p1_libiio_monotonic_ns();
        uint64_t remaining_ms;
        int status;
        if ((now_ns == 0ULL) || (now_ns >= deadline_ns))
        {
            return RA8P1_LIBIIO_ETIMEOUT;
        }
        remaining_ms = (deadline_ns - now_ns + 999999ULL) / 1000000ULL;
        if (remaining_ms > (uint64_t)INT32_MAX)
        {
            remaining_ms = (uint64_t)INT32_MAX;
        }
        descriptor.revents = 0;
        status = poll(&descriptor, 1U, (int)remaining_ms);
        if (status > 0)
        {
            if ((descriptor.revents & POLLIN) != 0)
            {
                return 0;
            }
            return -EIO;
        }
        if (status == 0)
        {
            return RA8P1_LIBIIO_ETIMEOUT;
        }
        if (errno != EINTR)
        {
            return -errno;
        }
    }
}

static int32_t ra8p1_libiio_refill(
    ra8p1_libiio_context_t *context, uint32_t sample_count,
    uint32_t timeout_ms, const uint8_t **i_first, const uint8_t **q_first,
    ptrdiff_t *step, const uint8_t **end)
{
    ssize_t bytes;
    int status;

    status = ra8p1_libiio_ensure_buffer(context, sample_count);
    if (status != 0)
    {
        return status;
    }
    if (context->io.context_set_timeout != NULL)
    {
        status = context->io.context_set_timeout(
            context->iio_context, timeout_ms);
        if (status < 0)
        {
            return status;
        }
    }
    bytes = context->io.buffer_refill(context->rx_buffer);
    while ((bytes == -EAGAIN) || (bytes == -EWOULDBLOCK))
    {
        if (context->poll_buffer_enabled == 0U)
        {
            break;
        }
        status = ra8p1_libiio_poll_until_ready(context->buffer_poll_fd,
                                                timeout_ms);
        if (status != 0)
        {
            ra8p1_libiio_destroy_buffer(context, true);
            return status;
        }
        bytes = context->io.buffer_refill(context->rx_buffer);
    }
    if (bytes < 0)
    {
        ra8p1_libiio_destroy_buffer(context, true);
        return (int32_t) bytes;
    }
    *i_first = (const uint8_t *) context->io.buffer_first(
        context->rx_buffer, context->rx_i_channel);
    *q_first = (const uint8_t *) context->io.buffer_first(
        context->rx_buffer, context->rx_q_channel);
    *step = context->io.buffer_step(context->rx_buffer);
    *end = (const uint8_t *) context->io.buffer_end(context->rx_buffer);
    if ((*i_first == NULL) || (*q_first == NULL) || (*end == NULL) ||
        (*step < 4) || (bytes < (ssize_t)((size_t) sample_count * 4U)))
    {
        ra8p1_libiio_destroy_buffer(context, true);
        return RA8P1_LIBIIO_ELAYOUT;
    }
    return 0;
}

static int32_t ra8p1_libiio_rx1_capture_le(
    void *opaque, uint8_t *rx1_iq_le, uint32_t sample_count,
    uint32_t timeout_ms)
{
    ra8p1_libiio_context_t *context =
        (ra8p1_libiio_context_t *) opaque;
    const uint8_t *i_sample;
    const uint8_t *q_sample;
    const uint8_t *end;
    ptrdiff_t step;
    uint32_t index;
    int32_t status;

    if ((context != &g_libiio_context) || (context->opened == 0U) ||
        (rx1_iq_le == NULL) || (sample_count == 0U) || (timeout_ms == 0U))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    status = ra8p1_libiio_refill(context, sample_count, timeout_ms,
                                  &i_sample, &q_sample, &step, &end);
    if (status != 0)
    {
        return status;
    }
    for (index = 0U; index < sample_count; index++)
    {
        uint8_t *output = rx1_iq_le + (size_t) index * 4U;
        if ((i_sample + 2U > end) || (q_sample + 2U > end))
        {
            return RA8P1_LIBIIO_ELAYOUT;
        }
        /* The target and AD936x IIO scan are little-endian.  This is the same
         * byte contract emitted by `iio_readdev ... voltage0 voltage1`. */
        output[0] = i_sample[0];
        output[1] = i_sample[1];
        output[2] = q_sample[0];
        output[3] = q_sample[1];
        i_sample += step;
        q_sample += step;
    }
    return 0;
}

static int32_t ra8p1_libiio_rx_capture(
    void *opaque, void *buffer, uint32_t sample_count, uint32_t timeout_ms)
{
    ra8p1_libiio_context_t *context =
        (ra8p1_libiio_context_t *) opaque;
    ra8p1_sdr_iq2_sample_t *output =
        (ra8p1_sdr_iq2_sample_t *) buffer;
    const uint8_t *i_sample;
    const uint8_t *q_sample;
    const uint8_t *end;
    ptrdiff_t step;
    uint32_t index;
    int32_t status;

    if ((context != &g_libiio_context) || (context->opened == 0U) ||
        (buffer == NULL) || (sample_count == 0U) || (timeout_ms == 0U))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    status = ra8p1_libiio_refill(context, sample_count, timeout_ms,
                                  &i_sample, &q_sample, &step, &end);
    if (status != 0)
    {
        return status;
    }
    for (index = 0U; index < sample_count; index++)
    {
        uint16_t i_bits;
        uint16_t q_bits;
        if ((i_sample + 2U > end) || (q_sample + 2U > end))
        {
            return RA8P1_LIBIIO_ELAYOUT;
        }
        i_bits = (uint16_t)((uint16_t)i_sample[0] |
                            ((uint16_t)i_sample[1] << 8U));
        q_bits = (uint16_t)((uint16_t)q_sample[0] |
                            ((uint16_t)q_sample[1] << 8U));
        output[index].rx1_i = (int16_t)i_bits;
        output[index].rx1_q = (int16_t)q_bits;
        output[index].rx2_i = 0;
        output[index].rx2_q = 0;
        i_sample += step;
        q_sample += step;
    }
    return 0;
}

static int32_t ra8p1_libiio_close(void *opaque)
{
    ra8p1_libiio_context_t *context =
        (ra8p1_libiio_context_t *) opaque;

    if ((context != &g_libiio_context) || (context->opened == 0U))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    ra8p1_libiio_release(context);
    return 0;
}

static int32_t ra8p1_libiio_get_status(
    void *opaque, ra8p1_sdr_adapter_status_t *status)
{
    ra8p1_libiio_context_t *context =
        (ra8p1_libiio_context_t *) opaque;
    uint32_t capacity;
    uint32_t copy_size;

    if ((context != &g_libiio_context) || (context->opened == 0U) ||
        (status == NULL))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    capacity = status->struct_size;
    if (capacity < RA8P1_SDR_ADAPTER_STATUS_V1_SIZE)
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    copy_size = capacity < sizeof(context->status) ?
                capacity : (uint32_t)sizeof(context->status);
    memcpy(status, &context->status, copy_size);
    status->struct_size = copy_size;
    return 0;
}

RA8P1_SDR_ADAPTER_EXPORT int32_t ra8p1_sdr_adapter_get_api_v1(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api)
{
    uint32_t capacity;
    uint32_t initialized_size;

    if ((requested_abi != RA8P1_SDR_ADAPTER_ABI_VERSION) || (api == NULL))
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    capacity = api->struct_size;
    if (capacity < RA8P1_SDR_ADAPTER_V1_CORE_SIZE)
    {
        return RA8P1_LIBIIO_EINVAL;
    }
    initialized_size = capacity < sizeof(*api) ? capacity : (uint32_t)sizeof(*api);
    memset(api, 0, initialized_size);
    api->struct_size = initialized_size;
    api->abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    api->capture_format = RA8P1_SDR_CAPTURE_NORMALIZED_IQ2;
    api->sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api->name = "Pluto local libiio 0.25 persistent RX1 adapter";
    api->open = ra8p1_libiio_open;
    api->set_rx = ra8p1_libiio_set_rx;
    api->rx_capture = ra8p1_libiio_rx_capture;
    api->close = ra8p1_libiio_close;
    if (capacity >= RA8P1_SDR_ADAPTER_V1_RX1_LE_SIZE)
    {
        api->rx1_capture_le = ra8p1_libiio_rx1_capture_le;
    }
    if (capacity >= RA8P1_SDR_ADAPTER_V1_STATUS_SIZE)
    {
        api->get_status = ra8p1_libiio_get_status;
    }
    return 0;
}
