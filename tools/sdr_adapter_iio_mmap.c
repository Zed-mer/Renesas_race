/*
 * Pluto local IIO DMA block/mmap adapter.
 *
 * This is a user-space plugin. It does not change the SDR image, FPGA, boot
 * files, or network address. RF controls are written through the existing IIO
 * sysfs ABI; sample data uses the block API present in the pinned Pluto tree:
 * IIO_BUFFER_GET_FD_IOCTL followed by IIO_BLOCK_* ioctls and mmap().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sdr_adapter.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#error "sdr_adapter_iio_mmap.c is a POSIX/Pluto plugin"
#endif

#if defined(RA8P1_IIO_MMAP_TESTING)
#include "sdr_adapter_iio_mmap_test.h"
#endif

#define RA8P1_IIO_MMAP_SYSFS_ENV       "RA8P1_IIO_SYSFS_ROOT"
#define RA8P1_IIO_MMAP_DEV_ENV         "RA8P1_IIO_DEV_ROOT"
#define RA8P1_IIO_MMAP_SYSFS_DEFAULT   "/sys/bus/iio/devices"
#define RA8P1_IIO_MMAP_DEV_DEFAULT     "/dev"
#define RA8P1_IIO_MMAP_PHY_NAME        "ad9361-phy"
#define RA8P1_IIO_MMAP_RX_NAME         "cf-ad9361-lpc"
#define RA8P1_IIO_MMAP_BLOCK_COUNT     (2U)
#define RA8P1_IIO_MMAP_PAGE_SIZE       (4096U)
#define RA8P1_IIO_MMAP_MAX_BLOCK_SIZE  (16U * 1024U * 1024U)
#define RA8P1_IIO_MMAP_SAMPLE_RATE     (60000000U)
#define RA8P1_IIO_MMAP_BANDWIDTH      (56000000U)
#define RA8P1_IIO_MMAP_MAX_TEXT        (256U)
#define RA8P1_IIO_MMAP_MAX_CENTER      (4U)

/* The IIO ABI has no guaranteed FPGA FIFO reset. Keep DMA stopped long enough
 * for retuning, then omit an oversized block's prefix from the RF window. */
#define RA8P1_IIO_MMAP_TUNE_SETTLE_ENV "RA8P1_IIO_TUNE_SETTLE_US"
#define RA8P1_IIO_MMAP_TUNE_DISCARD_ENV \
    "RA8P1_IIO_TUNE_DISCARD_SAMPLES"
#define RA8P1_IIO_MMAP_TUNE_SETTLE_DEFAULT_US       (1000U)
#define RA8P1_IIO_MMAP_TUNE_SETTLE_MAX_US           (1000000U)
#define RA8P1_IIO_MMAP_TUNE_DISCARD_DEFAULT_SAMPLES (4096U)
#define RA8P1_IIO_MMAP_TUNE_DISCARD_MAX_SAMPLES     (1048576U)

/* Keep private errors outside the errno range used by the caller. */
#define RA8P1_IIO_MMAP_ESTATE          (-2301)
#define RA8P1_IIO_MMAP_EDEVICE         (-2302)
#define RA8P1_IIO_MMAP_ECHANNEL        (-2303)
#define RA8P1_IIO_MMAP_ELAYOUT         (-2304)
#define RA8P1_IIO_MMAP_EBUFFER         (-2305)
#define RA8P1_IIO_MMAP_ETIMEOUT        (-2306)

#define RA8P1_SDR_ADAPTER_EXPORT __attribute__((visibility("default")))

/* The pinned kernel exposes these definitions in a non-UAPI header. */
#define IIO_BUFFER_GET_FD_IOCTL _IOWR('i', 0x91, int)
#define IIO_BLOCK_ALLOC_IOCTL \
    _IOWR('i', 0xa0, struct ra8p1_iio_block_alloc_req)
#define IIO_BLOCK_FREE_IOCTL _IO('i', 0xa1)
#define IIO_BLOCK_QUERY_IOCTL \
    _IOWR('i', 0xa2, struct ra8p1_iio_block)
#define IIO_BLOCK_ENQUEUE_IOCTL \
    _IOWR('i', 0xa3, struct ra8p1_iio_block)
#define IIO_BLOCK_DEQUEUE_IOCTL \
    _IOWR('i', 0xa4, struct ra8p1_iio_block)

struct ra8p1_iio_block_alloc_req
{
    uint32_t type;
    uint32_t size;
    uint32_t count;
    uint32_t id;
};

struct ra8p1_iio_block
{
    uint32_t id;
    uint32_t size;
    uint32_t bytes_used;
    uint32_t type;
    uint32_t flags;
    union
    {
        uint32_t offset;
    } data;
    uint64_t timestamp;
};

typedef struct st_ra8p1_iio_mmap_block
{
    uint32_t id;
    uint32_t size;
    uint32_t owned;
    uint32_t queued;
    void *mapping;
} ra8p1_iio_mmap_block_t;

typedef struct st_ra8p1_iio_mmap_context
{
    int device_fd;
    int buffer_fd;
    char phy_dir[PATH_MAX];
    char rx_dir[PATH_MAX];
    char rx_device_path[PATH_MAX];
    char phy_sample_path[PATH_MAX];
    char phy_bandwidth_path[PATH_MAX];
    char rx_lo_path[PATH_MAX];
    char fastlock_store_path[PATH_MAX];
    char fastlock_recall_path[PATH_MAX];
    char scan_dir[PATH_MAX];
    char rx_i_enable_path[PATH_MAX];
    char rx_q_enable_path[PATH_MAX];
    char buffer_enable_path[PATH_MAX];
    char buffer_length_path[PATH_MAX];
    uint32_t i_offset;
    uint32_t q_offset;
    uint32_t scan_step;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint64_t center_hz;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t next_block;
    uint32_t enabled;
    uint32_t opened;
    uint32_t capture_active;
    uint32_t tune_settle_us;
    uint32_t tune_discard_samples;
    uint32_t tune_guard_pending;
    ra8p1_iio_mmap_block_t blocks[RA8P1_IIO_MMAP_BLOCK_COUNT];
    ra8p1_sdr_adapter_status_t status;
} ra8p1_iio_mmap_context_t;

static ra8p1_iio_mmap_context_t g_iio_mmap_context;

#if defined(RA8P1_IIO_MMAP_TESTING)
static ra8p1_iio_mmap_test_hooks_t g_test_hooks;
static uint32_t g_iio_mmap_fast_copy_hits;

void ra8p1_iio_mmap_set_test_hooks(
    const ra8p1_iio_mmap_test_hooks_t *hooks)
{
    if (hooks == NULL)
    {
        memset(&g_test_hooks, 0, sizeof(g_test_hooks));
    }
    else
    {
        g_test_hooks = *hooks;
    }
}
#endif

static int ra8p1_iio_ioctl(int fd, unsigned long request, void *argument)
{
#if defined(RA8P1_IIO_MMAP_TESTING)
    if (g_test_hooks.ioctl_fn != NULL)
    {
        return g_test_hooks.ioctl_fn(fd, request, argument);
    }
#endif
    return ioctl(fd, request, argument);
}

static void *ra8p1_iio_mmap(void *address, size_t length, int protection,
                            int flags, int fd, off_t offset)
{
#if defined(RA8P1_IIO_MMAP_TESTING)
    if (g_test_hooks.mmap_fn != NULL)
    {
        return g_test_hooks.mmap_fn(address, length, protection, flags,
                                    fd, offset);
    }
#endif
    return mmap(address, length, protection, flags, fd, offset);
}

static int ra8p1_iio_munmap(void *address, size_t length)
{
#if defined(RA8P1_IIO_MMAP_TESTING)
    if (g_test_hooks.munmap_fn != NULL)
    {
        return g_test_hooks.munmap_fn(address, length);
    }
#endif
    return munmap(address, length);
}

static int ra8p1_iio_poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
#if defined(RA8P1_IIO_MMAP_TESTING)
    if (g_test_hooks.poll_fn != NULL)
    {
        return g_test_hooks.poll_fn(fds, count, timeout_ms);
    }
#endif
    return poll(fds, count, timeout_ms);
}

static uint64_t ra8p1_iio_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0ULL;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL +
           (uint64_t)now.tv_nsec;
}

static int ra8p1_iio_env_u32(const char *name, uint32_t default_value,
                             uint32_t maximum, uint32_t *value)
{
    const char *text;
    char *end = NULL;
    unsigned long parsed;

    if ((name == NULL) || (value == NULL))
    {
        return -EINVAL;
    }
    text = getenv(name);
    if ((text == NULL) || (text[0] == '\0'))
    {
        *value = default_value;
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if ((errno != 0) || (end == text) || (*end != '\0') ||
        (parsed > (unsigned long)maximum))
    {
        return -EINVAL;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int ra8p1_iio_wait_for_tune_settle(
    const ra8p1_iio_mmap_context_t *context)
{
    uint64_t deadline_ns;

    if ((context->tune_guard_pending == 0U) ||
        (context->tune_settle_us == 0U) ||
        (context->status.tune_complete_ns == 0ULL))
    {
        return 0;
    }
    deadline_ns = context->status.tune_complete_ns +
        (uint64_t)context->tune_settle_us * 1000ULL;
    if (deadline_ns < context->status.tune_complete_ns)
    {
        return -EOVERFLOW;
    }
    for (;;)
    {
        const uint64_t now_ns = ra8p1_iio_now_ns();
        uint64_t remaining_ns;
        struct timespec delay;

        if (now_ns == 0ULL)
        {
            return -EIO;
        }
        if (now_ns >= deadline_ns)
        {
            return 0;
        }
        remaining_ns = deadline_ns - now_ns;
        delay.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
        delay.tv_nsec = (long)(remaining_ns % 1000000000ULL);
        if (nanosleep(&delay, NULL) == 0)
        {
            return 0;
        }
        if (errno != EINTR)
        {
            return -errno;
        }
    }
}

static int ra8p1_iio_path(char *destination, size_t capacity,
                          const char *directory, const char *name)
{
    int length;

    if ((destination == NULL) || (capacity == 0U) ||
        (directory == NULL) || (name == NULL))
    {
        return -EINVAL;
    }
    length = snprintf(destination, capacity, "%s/%s", directory, name);
    if ((length < 0) || ((size_t)length >= capacity))
    {
        return -ENAMETOOLONG;
    }
    return 0;
}

static void ra8p1_iio_trim(char *text)
{
    size_t length;
    size_t start = 0U;

    if (text == NULL)
    {
        return;
    }
    length = strlen(text);
    while ((start < length) &&
           ((text[start] == ' ') || (text[start] == '\t') ||
            (text[start] == '\r') || (text[start] == '\n')))
    {
        start++;
    }
    while ((length > start) &&
           ((text[length - 1U] == ' ') || (text[length - 1U] == '\t') ||
            (text[length - 1U] == '\r') || (text[length - 1U] == '\n')))
    {
        length--;
    }
    if (start != 0U)
    {
        memmove(text, text + start, length - start);
    }
    text[length - start] = '\0';
}

static int ra8p1_iio_read_text(const char *path, char *text, size_t capacity)
{
    int fd;
    ssize_t count;
    size_t used = 0U;

    if ((path == NULL) || (text == NULL) || (capacity < 2U))
    {
        return -EINVAL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }
    while (used + 1U < capacity)
    {
        count = read(fd, text + used, capacity - used - 1U);
        if (count < 0)
        {
            const int status = -errno;
            (void)close(fd);
            return status;
        }
        if (count == 0)
        {
            break;
        }
        used += (size_t)count;
    }
    (void)close(fd);
    text[used] = '\0';
    ra8p1_iio_trim(text);
    return 0;
}

static int ra8p1_iio_write_text(const char *path, const char *text)
{
    int fd;
    size_t length;
    size_t written = 0U;

    if ((path == NULL) || (text == NULL))
    {
        return -EINVAL;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }
    length = strlen(text);
    while (written < length)
    {
        const ssize_t count = write(fd, text + written, length - written);
        if (count < 0)
        {
            const int status = -errno;
            (void)close(fd);
            return status;
        }
        if (count == 0)
        {
            (void)close(fd);
            return -EIO;
        }
        written += (size_t)count;
    }
    if (close(fd) != 0)
    {
        return -errno;
    }
    return 0;
}

static int ra8p1_iio_write_u64(const char *path, uint64_t value)
{
    char text[32];

    if (snprintf(text, sizeof(text), "%llu\n",
                 (unsigned long long)value) < 0)
    {
        return -EIO;
    }
    return ra8p1_iio_write_text(path, text);
}

static int ra8p1_iio_write_u32(const char *path, uint32_t value)
{
    return ra8p1_iio_write_u64(path, (uint64_t)value);
}

static int ra8p1_iio_parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL) || (text[0] == '\0'))
    {
        return -EINVAL;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if ((errno != 0) || (end == text) || (*end != '\0') ||
        (parsed > UINT32_MAX))
    {
        return -EINVAL;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int ra8p1_iio_find_device(const char *root, const char *wanted,
                                 char *destination, size_t capacity)
{
    DIR *directory;
    struct dirent *entry;
    int status = RA8P1_IIO_MMAP_EDEVICE;

    directory = opendir(root);
    if (directory == NULL)
    {
        return -errno;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        char path[PATH_MAX];
        char name[RA8P1_IIO_MMAP_MAX_TEXT];

        if (strncmp(entry->d_name, "iio:device", 10U) != 0)
        {
            continue;
        }
        if (ra8p1_iio_path(path, sizeof(path), root, entry->d_name) != 0)
        {
            status = -ENAMETOOLONG;
            break;
        }
        if (ra8p1_iio_path(name, sizeof(name), path, "name") != 0)
        {
            status = -ENAMETOOLONG;
            break;
        }
        if (ra8p1_iio_read_text(name, name, sizeof(name)) != 0)
        {
            continue;
        }
        if (strcmp(name, wanted) == 0)
        {
            status = ra8p1_iio_path(destination, capacity, root,
                                     entry->d_name);
            break;
        }
    }
    (void)closedir(directory);
    return status;
}

static int ra8p1_iio_find_attr(const char *directory,
                               const char *const *names, size_t count,
                               char *destination, size_t capacity)
{
    size_t index;

    for (index = 0U; index < count; index++)
    {
        char path[PATH_MAX];
        if (ra8p1_iio_path(path, sizeof(path), directory, names[index]) != 0)
        {
            return -ENAMETOOLONG;
        }
        if (access(path, R_OK | W_OK) == 0)
        {
            return ra8p1_iio_path(destination, capacity, directory,
                                   names[index]);
        }
    }
    return -ENOENT;
}

static int ra8p1_iio_resolve_paths(ra8p1_iio_mmap_context_t *context)
{
    static const char *const sample_names[] = {
        "in_voltage_sampling_frequency", "in_voltage0_sampling_frequency"
    };
    static const char *const bandwidth_names[] = {
        "in_voltage_rf_bandwidth", "in_voltage0_rf_bandwidth"
    };
    static const char *const lo_names[] = {
        "out_altvoltage0_RX_LO_frequency", "out_altvoltage0_frequency"
    };
    static const char *const store_names[] = {
        "out_altvoltage0_RX_LO_fastlock_store",
        "out_altvoltage0_fastlock_store"
    };
    static const char *const recall_names[] = {
        "out_altvoltage0_RX_LO_fastlock_recall",
        "out_altvoltage0_fastlock_recall"
    };
    int status;

    status = ra8p1_iio_path(context->scan_dir, sizeof(context->scan_dir),
                            context->rx_dir, "scan_elements");
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_path(context->buffer_enable_path,
                            sizeof(context->buffer_enable_path),
                            context->rx_dir, "buffer/enable");
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_path(context->buffer_length_path,
                            sizeof(context->buffer_length_path),
                            context->rx_dir, "buffer/length");
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_path(context->rx_i_enable_path,
                            sizeof(context->rx_i_enable_path),
                            context->scan_dir, "in_voltage0_en");
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_path(context->rx_q_enable_path,
                            sizeof(context->rx_q_enable_path),
                            context->scan_dir, "in_voltage1_en");
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_find_attr(context->phy_dir, sample_names,
                                 sizeof(sample_names) / sizeof(sample_names[0]),
                                 context->phy_sample_path,
                                 sizeof(context->phy_sample_path));
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_find_attr(context->phy_dir, bandwidth_names,
                                 sizeof(bandwidth_names) /
                                     sizeof(bandwidth_names[0]),
                                 context->phy_bandwidth_path,
                                 sizeof(context->phy_bandwidth_path));
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_find_attr(context->phy_dir, lo_names,
                                 sizeof(lo_names) / sizeof(lo_names[0]),
                                 context->rx_lo_path,
                                 sizeof(context->rx_lo_path));
    if (status != 0)
    {
        return status;
    }
    /* Fastlock is optional on older images. */
    if (ra8p1_iio_find_attr(context->phy_dir, store_names,
                            sizeof(store_names) / sizeof(store_names[0]),
                            context->fastlock_store_path,
                            sizeof(context->fastlock_store_path)) != 0)
    {
        context->fastlock_store_path[0] = '\0';
    }
    if (ra8p1_iio_find_attr(context->phy_dir, recall_names,
                            sizeof(recall_names) / sizeof(recall_names[0]),
                            context->fastlock_recall_path,
                            sizeof(context->fastlock_recall_path)) != 0)
    {
        context->fastlock_recall_path[0] = '\0';
    }
    return 0;
}

static int ra8p1_iio_parse_type(const char *text)
{
    const char *cursor;
    uint32_t digits = 0U;
    uint32_t storage = 0U;
    uint32_t shift = 0U;

    if ((text == NULL) || (strncmp(text, "le:", 3U) != 0))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    cursor = text + 3U;
    if ((*cursor != 's') && (*cursor != 'S'))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    cursor++;
    while ((*cursor >= '0') && (*cursor <= '9'))
    {
        digits = digits * 10U + (uint32_t)(*cursor - '0');
        cursor++;
    }
    if (*cursor != '/')
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    cursor++;
    while ((*cursor >= '0') && (*cursor <= '9'))
    {
        storage = storage * 10U + (uint32_t)(*cursor - '0');
        cursor++;
    }
    if (strncmp(cursor, ">>", 2U) != 0)
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    cursor += 2U;
    while ((*cursor >= '0') && (*cursor <= '9'))
    {
        shift = shift * 10U + (uint32_t)(*cursor - '0');
        cursor++;
    }
    if ((*cursor != '\0') || (digits == 0U) || (storage != 16U) ||
        (shift != 0U))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    return 0;
}

static int ra8p1_iio_configure_scan(ra8p1_iio_mmap_context_t *context)
{
    DIR *directory;
    struct dirent *entry;
    char path[PATH_MAX];
    char text[RA8P1_IIO_MMAP_MAX_TEXT];
    char index_i_path[PATH_MAX];
    char index_q_path[PATH_MAX];
    char type_i_path[PATH_MAX];
    char type_q_path[PATH_MAX];
    uint32_t index_i;
    uint32_t index_q;
    int status;

    status = ra8p1_iio_write_text(context->buffer_enable_path, "0\n");
    if (status != 0)
    {
        return status;
    }
    directory = opendir(context->scan_dir);
    if (directory == NULL)
    {
        return -errno;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        const size_t length = strlen(entry->d_name);
        if ((length < 4U) || (strcmp(entry->d_name + length - 3U, "_en") != 0))
        {
            continue;
        }
        if (ra8p1_iio_path(path, sizeof(path), context->scan_dir,
                           entry->d_name) != 0)
        {
            (void)closedir(directory);
            return -ENAMETOOLONG;
        }
        status = ra8p1_iio_write_text(path, "0\n");
        if (status != 0)
        {
            (void)closedir(directory);
            return status;
        }
    }
    (void)closedir(directory);

    if (ra8p1_iio_path(index_i_path, sizeof(index_i_path), context->scan_dir,
                       "in_voltage0_index") != 0 ||
        ra8p1_iio_path(index_q_path, sizeof(index_q_path), context->scan_dir,
                       "in_voltage1_index") != 0 ||
        ra8p1_iio_path(type_i_path, sizeof(type_i_path), context->scan_dir,
                       "in_voltage0_type") != 0 ||
        ra8p1_iio_path(type_q_path, sizeof(type_q_path), context->scan_dir,
                       "in_voltage1_type") != 0)
    {
        return -ENAMETOOLONG;
    }
    status = ra8p1_iio_read_text(index_i_path, text, sizeof(text));
    if ((status != 0) || (ra8p1_iio_parse_u32(text, &index_i) != 0))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    status = ra8p1_iio_read_text(index_q_path, text, sizeof(text));
    if ((status != 0) || (ra8p1_iio_parse_u32(text, &index_q) != 0))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    if (index_i == index_q)
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    status = ra8p1_iio_read_text(type_i_path, text, sizeof(text));
    if ((status != 0) || (ra8p1_iio_parse_type(text) != 0))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    status = ra8p1_iio_read_text(type_q_path, text, sizeof(text));
    if ((status != 0) || (ra8p1_iio_parse_type(text) != 0))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    context->i_offset = (index_i < index_q) ? 0U : 2U;
    context->q_offset = (index_i < index_q) ? 2U : 0U;
    context->scan_step = 4U;
    status = ra8p1_iio_write_text(context->rx_i_enable_path, "1\n");
    if (status != 0)
    {
        return status;
    }
    return ra8p1_iio_write_text(context->rx_q_enable_path, "1\n");
}

static int ra8p1_iio_center_index(uint64_t center_hz, uint32_t *index)
{
    static const uint64_t centers[RA8P1_IIO_MMAP_MAX_CENTER] = {
        2420000000ULL, 2464000000ULL, 5760000000ULL, 5816000000ULL
    };
    uint32_t current;

    for (current = 0U; current < RA8P1_IIO_MMAP_MAX_CENTER; current++)
    {
        if (centers[current] == center_hz)
        {
            if (index != NULL)
            {
                *index = current;
            }
            return 1;
        }
    }
    return 0;
}

static int32_t ra8p1_iio_set_rx(ra8p1_iio_mmap_context_t *context,
                                uint64_t center_hz, uint32_t sample_rate_hz,
                                uint32_t bandwidth_hz);

static void ra8p1_iio_arm_tune_guard(ra8p1_iio_mmap_context_t *context)
{
    context->tune_guard_pending =
        ((context->tune_settle_us != 0U) ||
         (context->tune_discard_samples != 0U)) ? 1U : 0U;
}

static int32_t ra8p1_iio_prepare_fastlock(
    ra8p1_iio_mmap_context_t *context, uint64_t restore_center_hz)
{
    static const uint64_t centers[RA8P1_IIO_MMAP_MAX_CENTER] = {
        2420000000ULL, 2464000000ULL, 5760000000ULL, 5816000000ULL
    };
    uint32_t restore_index = 0U;
    uint32_t index;
    int status;

    context->status.fastlock_profiles = 0U;
    if ((context->fastlock_store_path[0] == '\0') ||
        (context->fastlock_recall_path[0] == '\0'))
    {
        context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK;
        context->status.fallback_count++;
        return 0;
    }
    (void)ra8p1_iio_center_index(restore_center_hz, &restore_index);
    for (index = 0U; index < RA8P1_IIO_MMAP_MAX_CENTER; index++)
    {
        status = ra8p1_iio_write_u64(context->rx_lo_path, centers[index]);
        if (status != 0)
        {
            goto fallback;
        }
        status = ra8p1_iio_write_u64(context->fastlock_store_path, index);
        if (status != 0)
        {
            goto fallback;
        }
        context->status.fastlock_profiles++;
    }
    status = ra8p1_iio_write_u64(context->fastlock_recall_path, restore_index);
    if (status != 0)
    {
        goto fallback;
    }
    context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_SUPPORTED |
                             RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY;
    context->status.center_frequency_hz = restore_center_hz;
    context->status.tune_complete_ns = ra8p1_iio_now_ns();
    ra8p1_iio_arm_tune_guard(context);
    return 0;

fallback:
    context->status.fastlock_profiles = 0U;
    context->status.flags &= ~RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY;
    context->status.flags |= RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK;
    context->status.fallback_count++;
    status = ra8p1_iio_write_u64(context->rx_lo_path, restore_center_hz);
    if (status == 0)
    {
        context->center_hz = restore_center_hz;
        context->status.center_frequency_hz = restore_center_hz;
        context->status.tune_complete_ns = ra8p1_iio_now_ns();
        ra8p1_iio_arm_tune_guard(context);
    }
    return status;
}

static int32_t ra8p1_iio_set_rx(ra8p1_iio_mmap_context_t *context,
                                uint64_t center_hz, uint32_t sample_rate_hz,
                                uint32_t bandwidth_hz)
{
    uint32_t center_index = 0U;
    bool center_changed;
    int status = 0;

    if ((context == NULL) || (context->opened == 0U) ||
        !ra8p1_iio_center_index(center_hz, &center_index) ||
        (sample_rate_hz != RA8P1_IIO_MMAP_SAMPLE_RATE) ||
        (bandwidth_hz != RA8P1_IIO_MMAP_BANDWIDTH))
    {
        return -EINVAL;
    }
    center_changed = context->center_hz != center_hz;
    context->status.flags &= ~RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK;
    context->status.tune_start_ns = ra8p1_iio_now_ns();
    context->status.tune_count++;
    if (context->sample_rate_hz != sample_rate_hz)
    {
        status = ra8p1_iio_write_u64(context->phy_sample_path,
                                     sample_rate_hz);
        if (status != 0)
        {
            goto complete;
        }
        context->sample_rate_hz = sample_rate_hz;
    }
    if (context->bandwidth_hz != bandwidth_hz)
    {
        status = ra8p1_iio_write_u64(context->phy_bandwidth_path,
                                     bandwidth_hz);
        if (status != 0)
        {
            goto complete;
        }
        context->bandwidth_hz = bandwidth_hz;
    }
    if (center_changed)
    {
        if ((context->status.flags &
             RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY) != 0U)
        {
            status = ra8p1_iio_write_u64(context->fastlock_recall_path,
                                         center_index);
            if (status == 0)
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
                status = ra8p1_iio_write_u64(context->rx_lo_path, center_hz);
            }
        }
        else
        {
            status = ra8p1_iio_write_u64(context->rx_lo_path, center_hz);
        }
        if (status != 0)
        {
            goto complete;
        }
        context->center_hz = center_hz;
        ra8p1_iio_arm_tune_guard(context);
    }

complete:
    context->status.center_frequency_hz = context->center_hz;
    context->status.last_tune_status = status;
    context->status.tune_complete_ns = ra8p1_iio_now_ns();
    return status;
}

static int32_t ra8p1_iio_set_rx_callback(void *opaque, uint64_t center_hz,
                                         uint32_t sample_rate_hz,
                                         uint32_t bandwidth_hz)
{
    return ra8p1_iio_set_rx((ra8p1_iio_mmap_context_t *)opaque, center_hz,
                            sample_rate_hz, bandwidth_hz);
}

static void ra8p1_iio_mark_blocks_empty(ra8p1_iio_mmap_context_t *context)
{
    uint32_t index;

    for (index = 0U; index < RA8P1_IIO_MMAP_BLOCK_COUNT; index++)
    {
        context->blocks[index].id = 0U;
        context->blocks[index].size = 0U;
        context->blocks[index].owned = 0U;
        context->blocks[index].queued = 0U;
        context->blocks[index].mapping = NULL;
    }
    context->block_count = 0U;
    context->block_size = 0U;
}

static int ra8p1_iio_disable(ra8p1_iio_mmap_context_t *context)
{
    int status;

    if (context->enabled == 0U)
    {
        return 0;
    }
    status = ra8p1_iio_write_text(context->buffer_enable_path, "0\n");
    context->enabled = 0U;
    return status;
}

static void ra8p1_iio_destroy_blocks(ra8p1_iio_mmap_context_t *context)
{
    uint32_t index;

    (void)ra8p1_iio_disable(context);
    for (index = 0U; index < RA8P1_IIO_MMAP_BLOCK_COUNT; index++)
    {
        if (context->blocks[index].mapping != NULL)
        {
            (void)ra8p1_iio_munmap(context->blocks[index].mapping,
                                   context->blocks[index].size);
            context->blocks[index].mapping = NULL;
        }
    }
    if ((context->buffer_fd >= 0) && (context->block_count != 0U))
    {
        (void)ra8p1_iio_ioctl(context->buffer_fd, IIO_BLOCK_FREE_IOCTL, NULL);
    }
    ra8p1_iio_mark_blocks_empty(context);
}

static void ra8p1_iio_reset_buffer_fd(ra8p1_iio_mmap_context_t *context)
{
    ra8p1_iio_destroy_blocks(context);
    if (context->buffer_fd >= 0)
    {
        (void)close(context->buffer_fd);
        context->buffer_fd = -1;
    }
    if ((context->device_fd >= 0) && (context->opened != 0U))
    {
        int buffer_index = 0;
        if (ra8p1_iio_ioctl(context->device_fd, IIO_BUFFER_GET_FD_IOCTL,
                            &buffer_index) == 0)
        {
            context->buffer_fd = buffer_index;
            {
                int nonblocking = 1;
                (void)ra8p1_iio_ioctl(context->buffer_fd, FIONBIO,
                                      &nonblocking);
            }
        }
    }
}

static int32_t ra8p1_iio_ensure_blocks(ra8p1_iio_mmap_context_t *context,
                                       uint32_t sample_count)
{
    struct ra8p1_iio_block_alloc_req request;
    uint64_t byte_count;
    uint32_t index;
    int status;

    byte_count = (uint64_t)sample_count * 4ULL;
    if ((byte_count == 0ULL) || (byte_count > RA8P1_IIO_MMAP_MAX_BLOCK_SIZE))
    {
        return -EINVAL;
    }
    if ((context->block_count == RA8P1_IIO_MMAP_BLOCK_COUNT) &&
        (context->block_size == (uint32_t)byte_count))
    {
        return 0;
    }
    ra8p1_iio_destroy_blocks(context);
    memset(&request, 0, sizeof(request));
    request.size = (uint32_t)byte_count;
    request.count = RA8P1_IIO_MMAP_BLOCK_COUNT;
    status = ra8p1_iio_ioctl(context->buffer_fd, IIO_BLOCK_ALLOC_IOCTL,
                             &request);
    if (status != 0)
    {
        return status < 0 ? -errno : RA8P1_IIO_MMAP_EBUFFER;
    }
    if (request.count != RA8P1_IIO_MMAP_BLOCK_COUNT)
    {
        ra8p1_iio_destroy_blocks(context);
        return RA8P1_IIO_MMAP_EBUFFER;
    }
    context->block_count = request.count;
    for (index = 0U; index < RA8P1_IIO_MMAP_BLOCK_COUNT; index++)
    {
        struct ra8p1_iio_block descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.id = request.id + index;
        status = ra8p1_iio_ioctl(context->buffer_fd,
                                 IIO_BLOCK_QUERY_IOCTL, &descriptor);
        if ((status != 0) || (descriptor.size != (uint32_t)byte_count) ||
            ((descriptor.data.offset % RA8P1_IIO_MMAP_PAGE_SIZE) != 0U))
        {
            ra8p1_iio_destroy_blocks(context);
            return status != 0 ? -errno : RA8P1_IIO_MMAP_EBUFFER;
        }
        context->blocks[index].id = descriptor.id;
        context->blocks[index].size = descriptor.size;
        context->blocks[index].mapping = ra8p1_iio_mmap(
            NULL, descriptor.size, PROT_READ, MAP_SHARED,
            context->buffer_fd, (off_t)descriptor.data.offset);
        if ((context->blocks[index].mapping == MAP_FAILED) ||
            (context->blocks[index].mapping == NULL))
        {
            context->blocks[index].mapping = NULL;
            ra8p1_iio_destroy_blocks(context);
            return RA8P1_IIO_MMAP_EBUFFER;
        }
    }
    context->block_count = RA8P1_IIO_MMAP_BLOCK_COUNT;
    context->block_size = (uint32_t)byte_count;
    context->next_block = 0U;
    return 0;
}

static int ra8p1_iio_enqueue(ra8p1_iio_mmap_context_t *context,
                             uint32_t index)
{
    struct ra8p1_iio_block descriptor;
    int status;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = context->blocks[index].id;
    descriptor.size = context->blocks[index].size;
    descriptor.bytes_used = descriptor.size;
    status = ra8p1_iio_ioctl(context->buffer_fd,
                             IIO_BLOCK_ENQUEUE_IOCTL, &descriptor);
    if (status != 0)
    {
        return -errno;
    }
    context->blocks[index].queued = 1U;
    context->blocks[index].owned = 0U;
    return 0;
}

static int32_t ra8p1_iio_wait_dequeue(ra8p1_iio_mmap_context_t *context,
                                      uint32_t expected_index,
                                      uint32_t timeout_ms)
{
    struct pollfd descriptor;
    const uint64_t start_ns = ra8p1_iio_now_ns();
    const uint64_t deadline_ns = start_ns + (uint64_t)timeout_ms * 1000000ULL;

    if ((start_ns == 0ULL) || (deadline_ns < start_ns))
    {
        return RA8P1_IIO_MMAP_ETIMEOUT;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = context->buffer_fd;
    descriptor.events = POLLIN;
    for (;;)
    {
        const uint64_t now_ns = ra8p1_iio_now_ns();
        uint64_t remaining_ms;
        int status;

        if ((now_ns == 0ULL) || (now_ns >= deadline_ns))
        {
            return RA8P1_IIO_MMAP_ETIMEOUT;
        }
        remaining_ms = (deadline_ns - now_ns + 999999ULL) / 1000000ULL;
        if (remaining_ms > (uint64_t)INT32_MAX)
        {
            remaining_ms = (uint64_t)INT32_MAX;
        }
        descriptor.revents = 0;
        status = ra8p1_iio_poll(&descriptor, 1U, (int)remaining_ms);
        if (status == 0)
        {
            return RA8P1_IIO_MMAP_ETIMEOUT;
        }
        if (status < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -errno;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }
        if ((descriptor.revents & POLLIN) != 0)
        {
            struct ra8p1_iio_block block;
            memset(&block, 0, sizeof(block));
            status = ra8p1_iio_ioctl(context->buffer_fd,
                                     IIO_BLOCK_DEQUEUE_IOCTL, &block);
            if (status == 0)
            {
                uint32_t index;
                for (index = 0U; index < context->block_count; index++)
                {
                    if (context->blocks[index].id == block.id)
                    {
                        context->blocks[index].queued = 0U;
                        context->blocks[index].owned = 1U;
                        if ((index != expected_index) ||
                            (block.bytes_used < context->block_size))
                        {
                            return RA8P1_IIO_MMAP_EBUFFER;
                        }
                        return 0;
                    }
                }
                return RA8P1_IIO_MMAP_EBUFFER;
            }
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            {
                continue;
            }
            return -errno;
        }
    }
}

/*
 * Convert one completed IIO block to the agent's RX1 S16-IQ contract.  Pluto's
 * normal two-channel scan is already little-endian, tightly packed I,Q, so a
 * single memcpy avoids 590,336 per-sample byte shuffles on every window.  The
 * generic path remains for a reversed channel order or a future padded scan
 * layout; bounds are checked before either path touches the mapping.
 */
static int32_t ra8p1_iio_copy_rx1_layout(const uint8_t *source,
                                         size_t source_bytes,
                                         uint8_t *output,
                                         uint32_t source_sample_offset,
                                         uint32_t sample_count,
                                         uint32_t scan_step,
                                         uint32_t i_offset,
                                         uint32_t q_offset)
{
    uint64_t required_bytes;
    uint64_t first_sample_offset;
    uint64_t last_sample_index;
    uint64_t last_sample_offset;
    uint32_t last_offset;
    uint32_t index;

    if ((source == NULL) || (output == NULL) || (sample_count == 0U) ||
        (scan_step < 2U))
    {
        return -EINVAL;
    }
    if ((i_offset > (scan_step - 2U)) || (q_offset > (scan_step - 2U)))
    {
        return RA8P1_IIO_MMAP_ELAYOUT;
    }
    required_bytes = (uint64_t)sample_count * 4ULL;
    first_sample_offset = (uint64_t)source_sample_offset * scan_step;
    last_sample_index = (uint64_t)source_sample_offset + sample_count - 1ULL;
    if (last_sample_index > UINT64_MAX / scan_step)
    {
        return RA8P1_IIO_MMAP_EBUFFER;
    }
    last_sample_offset = last_sample_index * scan_step;
    last_offset = (i_offset > q_offset) ? i_offset : q_offset;
    if ((required_bytes > (uint64_t)SIZE_MAX) ||
        (first_sample_offset > (uint64_t)SIZE_MAX) ||
        (last_sample_offset > UINT64_MAX - (uint64_t)last_offset - 2ULL) ||
        (last_sample_offset + (uint64_t)last_offset + 2ULL > source_bytes))
    {
        return RA8P1_IIO_MMAP_EBUFFER;
    }

    if ((scan_step == 4U) && (i_offset == 0U) && (q_offset == 2U))
    {
        /* The source and destination are separate agent buffers. */
#if defined(RA8P1_IIO_MMAP_TESTING)
        g_iio_mmap_fast_copy_hits++;
#endif
        memcpy(output, source + (size_t)first_sample_offset,
               (size_t)required_bytes);
        return 0;
    }

    for (index = 0U; index < sample_count; index++)
    {
        const uint8_t *sample = source + (size_t)first_sample_offset +
            (size_t)index * scan_step;
        uint8_t *destination = output + (size_t)index * 4U;
        destination[0] = sample[i_offset];
        destination[1] = sample[i_offset + 1U];
        destination[2] = sample[q_offset];
        destination[3] = sample[q_offset + 1U];
    }
    return 0;
}

#if defined(RA8P1_IIO_MMAP_TESTING)
int32_t ra8p1_iio_mmap_copy_rx1_for_test(const uint8_t *source,
                                         size_t source_bytes,
                                         uint8_t *output,
                                         uint32_t source_sample_offset,
                                         uint32_t sample_count,
                                         uint32_t scan_step,
                                         uint32_t i_offset,
                                         uint32_t q_offset)
{
    return ra8p1_iio_copy_rx1_layout(source, source_bytes, output,
                                     source_sample_offset, sample_count,
                                     scan_step,
                                     i_offset, q_offset);
}

uint32_t ra8p1_iio_mmap_fast_copy_hits_for_test(void)
{
    return g_iio_mmap_fast_copy_hits;
}
#endif

static int32_t ra8p1_iio_capture_plan(uint32_t sample_count,
                                      uint32_t requested_discard_samples,
                                      uint32_t tune_guard_pending,
                                      uint32_t *dma_sample_count,
                                      uint32_t *discard_samples)
{
    const uint32_t maximum_samples =
        RA8P1_IIO_MMAP_MAX_BLOCK_SIZE / 4U;
    uint32_t discard = 0U;

    if ((sample_count == 0U) || (sample_count > maximum_samples) ||
        (dma_sample_count == NULL) || (discard_samples == NULL))
    {
        return -EINVAL;
    }
    if ((tune_guard_pending != 0U) && (sample_count < maximum_samples))
    {
        const uint32_t available_samples = maximum_samples - sample_count;
        discard = requested_discard_samples;
        if (discard > available_samples)
        {
            discard = available_samples;
        }
    }
    *discard_samples = discard;
    *dma_sample_count = sample_count + discard;
    return 0;
}

#if defined(RA8P1_IIO_MMAP_TESTING)
int32_t ra8p1_iio_mmap_capture_plan_for_test(
    uint32_t sample_count, uint32_t requested_discard_samples,
    uint32_t tune_guard_pending, uint32_t *dma_sample_count,
    uint32_t *discard_samples)
{
    return ra8p1_iio_capture_plan(sample_count, requested_discard_samples,
                                  tune_guard_pending, dma_sample_count,
                                  discard_samples);
}
#endif

static int32_t ra8p1_iio_capture_to_le(
    ra8p1_iio_mmap_context_t *context, uint8_t *output,
    uint32_t sample_count, uint32_t timeout_ms)
{
    const bool tune_guard_pending = context->tune_guard_pending != 0U;
    uint32_t dma_sample_count;
    uint32_t discard_samples;
    uint32_t block_index;
    int32_t status;

    if ((output == NULL) || (sample_count == 0U) || (timeout_ms == 0U))
    {
        return -EINVAL;
    }
    if (context->capture_active != 0U)
    {
        return RA8P1_IIO_MMAP_ESTATE;
    }
    status = ra8p1_iio_capture_plan(sample_count,
                                     context->tune_discard_samples,
                                     tune_guard_pending ? 1U : 0U,
                                     &dma_sample_count, &discard_samples);
    if (status != 0)
    {
        return status;
    }
    context->status.capture_prepare_ns = ra8p1_iio_now_ns();
    context->status.blocks_ready_ns = 0ULL;
    context->status.buffer_enable_ns = 0ULL;
    context->status.block_dequeue_ns = 0ULL;
    context->status.buffer_disable_ns = 0ULL;
    context->status.copy_complete_ns = 0ULL;
    context->status.flags &=
        ~RA8P1_SDR_ADAPTER_STATUS_LAST_CAPTURE_TUNE_GUARDED;
    context->capture_active = 1U;
    status = ra8p1_iio_ensure_blocks(context, dma_sample_count);
    if (status != 0)
    {
        context->capture_active = 0U;
        return status;
    }
    context->status.blocks_ready_ns = ra8p1_iio_now_ns();
    status = ra8p1_iio_wait_for_tune_settle(context);
    if (status != 0)
    {
        context->capture_active = 0U;
        return status;
    }
    block_index = context->next_block % RA8P1_IIO_MMAP_BLOCK_COUNT;
    status = ra8p1_iio_enqueue(context, block_index);
    if (status != 0)
    {
        context->capture_active = 0U;
        ra8p1_iio_reset_buffer_fd(context);
        return status;
    }
    status = ra8p1_iio_write_u32(context->buffer_length_path,
                                  dma_sample_count);
    /* The DMA block size, not buffer/length, is authoritative. */
    (void)status;
    status = ra8p1_iio_write_text(context->buffer_enable_path, "1\n");
    if (status != 0)
    {
        context->capture_active = 0U;
        ra8p1_iio_reset_buffer_fd(context);
        return status;
    }
    context->enabled = 1U;
    context->status.buffer_enable_ns = ra8p1_iio_now_ns();
    status = ra8p1_iio_wait_dequeue(context, block_index, timeout_ms);
    if (status != 0)
    {
        (void)ra8p1_iio_disable(context);
        ra8p1_iio_reset_buffer_fd(context);
        context->capture_active = 0U;
        return status;
    }
    context->status.block_dequeue_ns = ra8p1_iio_now_ns();
    status = ra8p1_iio_disable(context);
    if (status != 0)
    {
        ra8p1_iio_reset_buffer_fd(context);
        context->capture_active = 0U;
        return status;
    }
    context->status.buffer_disable_ns = ra8p1_iio_now_ns();
    status = ra8p1_iio_copy_rx1_layout(
        (const uint8_t *)context->blocks[block_index].mapping,
        context->blocks[block_index].size,
        output,
        discard_samples,
        sample_count,
        context->scan_step,
        context->i_offset,
        context->q_offset);
    if (status != 0)
    {
        context->capture_active = 0U;
        return status;
    }
    context->status.copy_complete_ns = ra8p1_iio_now_ns();
    if (tune_guard_pending)
    {
        context->tune_guard_pending = 0U;
        if ((context->tune_settle_us != 0U) || (discard_samples != 0U))
        {
            context->status.flags |=
                RA8P1_SDR_ADAPTER_STATUS_LAST_CAPTURE_TUNE_GUARDED;
        }
    }
    context->blocks[block_index].owned = 1U;
    context->next_block = (block_index + 1U) % RA8P1_IIO_MMAP_BLOCK_COUNT;
    context->capture_active = 0U;
    return 0;
}

static int32_t ra8p1_iio_open(void **opaque,
                              const ra8p1_sdr_adapter_config_t *config)
{
    ra8p1_iio_mmap_context_t *context = &g_iio_mmap_context;
    const char *sysfs_root;
    const char *device_root;
    int buffer_index = 0;
    int status;

    if ((opaque == NULL) || (config == NULL) ||
        (config->struct_size < sizeof(*config)) ||
        (config->abi_version != RA8P1_SDR_ADAPTER_ABI_VERSION) ||
        (config->rx_channels != RA8P1_SDR_ADAPTER_RX_CHANNELS) ||
        (context->opened != 0U))
    {
        return -EINVAL;
    }
    *opaque = NULL;
    memset(context, 0, sizeof(*context));
    context->device_fd = -1;
    context->buffer_fd = -1;
    status = ra8p1_iio_env_u32(RA8P1_IIO_MMAP_TUNE_SETTLE_ENV,
                                RA8P1_IIO_MMAP_TUNE_SETTLE_DEFAULT_US,
                                RA8P1_IIO_MMAP_TUNE_SETTLE_MAX_US,
                                &context->tune_settle_us);
    if (status == 0)
    {
        status = ra8p1_iio_env_u32(
            RA8P1_IIO_MMAP_TUNE_DISCARD_ENV,
            RA8P1_IIO_MMAP_TUNE_DISCARD_DEFAULT_SAMPLES,
            RA8P1_IIO_MMAP_TUNE_DISCARD_MAX_SAMPLES,
            &context->tune_discard_samples);
    }
    if (status != 0)
    {
        return status;
    }
    sysfs_root = getenv(RA8P1_IIO_MMAP_SYSFS_ENV);
    device_root = getenv(RA8P1_IIO_MMAP_DEV_ENV);
    if ((sysfs_root == NULL) || (sysfs_root[0] == '\0'))
    {
        sysfs_root = RA8P1_IIO_MMAP_SYSFS_DEFAULT;
    }
    if ((device_root == NULL) || (device_root[0] == '\0'))
    {
        device_root = RA8P1_IIO_MMAP_DEV_DEFAULT;
    }
    status = ra8p1_iio_find_device(sysfs_root, RA8P1_IIO_MMAP_PHY_NAME,
                                    context->phy_dir,
                                    sizeof(context->phy_dir));
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_find_device(sysfs_root, RA8P1_IIO_MMAP_RX_NAME,
                                    context->rx_dir, sizeof(context->rx_dir));
    if (status != 0)
    {
        return status;
    }
    status = ra8p1_iio_resolve_paths(context);
    if (status != 0)
    {
        return status;
    }
    {
        const char *rx_name = strrchr(context->rx_dir, '/');
        if ((rx_name == NULL) ||
            (ra8p1_iio_path(context->rx_device_path,
                            sizeof(context->rx_device_path), device_root,
                            rx_name + 1U) != 0))
        {
            return RA8P1_IIO_MMAP_EDEVICE;
        }
    }
    context->device_fd = open(context->rx_device_path, O_RDWR | O_CLOEXEC);
    if (context->device_fd < 0)
    {
        return -errno;
    }
    status = ra8p1_iio_ioctl(context->device_fd, IIO_BUFFER_GET_FD_IOCTL,
                             &buffer_index);
    if (status != 0)
    {
        status = -errno;
        (void)close(context->device_fd);
        context->device_fd = -1;
        return status;
    }
    context->buffer_fd = buffer_index;
    {
        int nonblocking = 1;
        status = ra8p1_iio_ioctl(context->buffer_fd, FIONBIO,
                                 &nonblocking);
        if (status != 0)
        {
            status = -errno;
            (void)close(context->buffer_fd);
            (void)close(context->device_fd);
            context->buffer_fd = -1;
            context->device_fd = -1;
            return status;
        }
    }
    context->status.struct_size = (uint32_t)sizeof(context->status);
    context->status.version = RA8P1_SDR_ADAPTER_STATUS_VERSION;
    context->opened = 1U;
    status = ra8p1_iio_configure_scan(context);
    if (status == 0)
    {
        status = ra8p1_iio_set_rx(context, config->initial_center_frequency_hz,
                                  config->sample_rate_hz,
                                  config->bandwidth_hz);
    }
    if (status == 0)
    {
        status = ra8p1_iio_prepare_fastlock(
            context, config->initial_center_frequency_hz);
    }
    if (status != 0)
    {
        (void)ra8p1_iio_disable(context);
        (void)ra8p1_iio_write_text(context->rx_i_enable_path, "0\n");
        (void)ra8p1_iio_write_text(context->rx_q_enable_path, "0\n");
        (void)close(context->buffer_fd);
        (void)close(context->device_fd);
        memset(context, 0, sizeof(*context));
        context->device_fd = -1;
        context->buffer_fd = -1;
        return status;
    }
    *opaque = context;
    return 0;
}

static int32_t ra8p1_iio_rx1_capture_le(void *opaque, uint8_t *output,
                                        uint32_t sample_count,
                                        uint32_t timeout_ms)
{
    ra8p1_iio_mmap_context_t *context = (ra8p1_iio_mmap_context_t *)opaque;

    if ((context != &g_iio_mmap_context) || (context->opened == 0U))
    {
        return RA8P1_IIO_MMAP_ESTATE;
    }
    return ra8p1_iio_capture_to_le(context, output, sample_count, timeout_ms);
}

static int32_t ra8p1_iio_rx_capture(void *opaque, void *buffer,
                                    uint32_t sample_count,
                                    uint32_t timeout_ms)
{
    ra8p1_iio_mmap_context_t *context = (ra8p1_iio_mmap_context_t *)opaque;
    ra8p1_sdr_iq2_sample_t *samples;
    uint32_t index;
    int32_t status;

    if ((context != &g_iio_mmap_context) || (context->opened == 0U) ||
        (buffer == NULL))
    {
        return RA8P1_IIO_MMAP_ESTATE;
    }
    status = ra8p1_iio_capture_to_le(context, (uint8_t *)buffer,
                                     sample_count, timeout_ms);
    if (status != 0)
    {
        return status;
    }
    samples = (ra8p1_sdr_iq2_sample_t *)buffer;
    for (index = sample_count; index-- > 0U;)
    {
        const uint8_t *source = ((const uint8_t *)buffer) +
                                (size_t)index * 4U;
        uint16_t i_bits = (uint16_t)source[0] |
                          ((uint16_t)source[1] << 8U);
        uint16_t q_bits = (uint16_t)source[2] |
                          ((uint16_t)source[3] << 8U);
        samples[index].rx1_i = (int16_t)i_bits;
        samples[index].rx1_q = (int16_t)q_bits;
        samples[index].rx2_i = 0;
        samples[index].rx2_q = 0;
    }
    return 0;
}

static int32_t ra8p1_iio_close(void *opaque)
{
    ra8p1_iio_mmap_context_t *context = (ra8p1_iio_mmap_context_t *)opaque;

    if ((context != &g_iio_mmap_context) || (context->opened == 0U))
    {
        return RA8P1_IIO_MMAP_ESTATE;
    }
    (void)ra8p1_iio_destroy_blocks(context);
    (void)ra8p1_iio_write_text(context->buffer_enable_path, "0\n");
    (void)ra8p1_iio_write_text(context->rx_i_enable_path, "0\n");
    (void)ra8p1_iio_write_text(context->rx_q_enable_path, "0\n");
    if (context->buffer_fd >= 0)
    {
        (void)close(context->buffer_fd);
    }
    if (context->device_fd >= 0)
    {
        (void)close(context->device_fd);
    }
    memset(context, 0, sizeof(*context));
    context->device_fd = -1;
    context->buffer_fd = -1;
    return 0;
}

static int32_t ra8p1_iio_get_status(void *opaque,
                                    ra8p1_sdr_adapter_status_t *status)
{
    ra8p1_iio_mmap_context_t *context = (ra8p1_iio_mmap_context_t *)opaque;
    uint32_t capacity;
    uint32_t copy_size;

    if ((context != &g_iio_mmap_context) || (context->opened == 0U) ||
        (status == NULL))
    {
        return RA8P1_IIO_MMAP_ESTATE;
    }
    capacity = status->struct_size;
    if (capacity < RA8P1_SDR_ADAPTER_STATUS_V1_SIZE)
    {
        return -EINVAL;
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
        return -EINVAL;
    }
    capacity = api->struct_size;
    if (capacity < RA8P1_SDR_ADAPTER_V1_CORE_SIZE)
    {
        return -EINVAL;
    }
    initialized_size = capacity < sizeof(*api) ? capacity : (uint32_t)sizeof(*api);
    memset(api, 0, initialized_size);
    api->struct_size = initialized_size;
    api->abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    api->capture_format = RA8P1_SDR_CAPTURE_NORMALIZED_IQ2;
    api->sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api->name = "Pluto local IIO DMA block+mmap RX1 S16 LE (2-block ping-pong)";
    api->open = ra8p1_iio_open;
    api->set_rx = ra8p1_iio_set_rx_callback;
    api->rx_capture = ra8p1_iio_rx_capture;
    api->close = ra8p1_iio_close;
    if (capacity >= RA8P1_SDR_ADAPTER_V1_RX1_LE_SIZE)
    {
        api->rx1_capture_le = ra8p1_iio_rx1_capture_le;
    }
    if (capacity >= RA8P1_SDR_ADAPTER_V1_STATUS_SIZE)
    {
        api->get_status = ra8p1_iio_get_status;
    }
    return 0;
}
