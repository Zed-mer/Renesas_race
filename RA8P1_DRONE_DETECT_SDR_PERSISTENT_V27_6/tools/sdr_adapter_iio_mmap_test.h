#ifndef RA8P1_SDR_ADAPTER_IIO_MMAP_TEST_H
#define RA8P1_SDR_ADAPTER_IIO_MMAP_TEST_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Test-only hooks. Production builds do not expose or use these callbacks. */
typedef struct st_ra8p1_iio_mmap_test_hooks
{
    int (*ioctl_fn)(int fd, unsigned long request, void *argument);
    void *(*mmap_fn)(void *address, size_t length, int protection,
                     int flags, int fd, off_t offset);
    int (*munmap_fn)(void *address, size_t length);
    int (*poll_fn)(struct pollfd *fds, nfds_t count, int timeout_ms);
} ra8p1_iio_mmap_test_hooks_t;

void ra8p1_iio_mmap_set_test_hooks(
    const ra8p1_iio_mmap_test_hooks_t *hooks);

/* Pure conversion hook used by the host fast-path regression test. */
int32_t ra8p1_iio_mmap_copy_rx1_for_test(const uint8_t *source,
                                         size_t source_bytes,
                                         uint8_t *output,
                                         uint32_t source_sample_offset,
                                         uint32_t sample_count,
                                         uint32_t scan_step,
                                         uint32_t i_offset,
                                         uint32_t q_offset);
uint32_t ra8p1_iio_mmap_fast_copy_hits_for_test(void);
int32_t ra8p1_iio_mmap_capture_plan_for_test(
    uint32_t sample_count, uint32_t requested_discard_samples,
    uint32_t tune_guard_pending, uint32_t *dma_sample_count,
    uint32_t *discard_samples);

#endif
