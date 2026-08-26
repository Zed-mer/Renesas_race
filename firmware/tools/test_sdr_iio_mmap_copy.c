/* Host-only regression tests for the IIO mmap RX1 conversion fast path. */

#include "sdr_adapter_iio_mmap_test.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_contiguous_iq_uses_contract_order(void)
{
    static const uint8_t source[] = {
        0x01, 0x02, 0xA1, 0xA2,
        0x03, 0x04, 0xA3, 0xA4,
        0x05, 0x06, 0xA5, 0xA6,
    };
    uint8_t output[sizeof(source)];
    const uint32_t hits = ra8p1_iio_mmap_fast_copy_hits_for_test();

    memset(output, 0xCC, sizeof(output));
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source), output, 0U, 3U, 4U, 0U, 2U) == 0);
    assert(memcmp(source, output, sizeof(source)) == 0);
    assert(ra8p1_iio_mmap_fast_copy_hits_for_test() == hits + 1U);
}

static void test_reversed_channels_use_fallback_shuffle(void)
{
    static const uint8_t source[] = {
        0xA1, 0xA2, 0x01, 0x02,
        0xA3, 0xA4, 0x03, 0x04,
    };
    static const uint8_t expected[] = {
        0x01, 0x02, 0xA1, 0xA2,
        0x03, 0x04, 0xA3, 0xA4,
    };
    uint8_t output[sizeof(source)];
    const uint32_t hits = ra8p1_iio_mmap_fast_copy_hits_for_test();

    memset(output, 0xCC, sizeof(output));
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source), output, 0U, 2U, 4U, 2U, 0U) == 0);
    assert(memcmp(expected, output, sizeof(expected)) == 0);
    assert(ra8p1_iio_mmap_fast_copy_hits_for_test() == hits);
}

static void test_strided_layout_and_bounds_are_checked(void)
{
    static const uint8_t source[] = {
        0x01, 0x02, 0xA1, 0xA2, 0xEE, 0xEE,
        0x03, 0x04, 0xA3, 0xA4, 0xEE, 0xEE,
    };
    static const uint8_t expected[] = {
        0x01, 0x02, 0xA1, 0xA2,
        0x03, 0x04, 0xA3, 0xA4,
    };
    uint8_t output[sizeof(expected)];

    memset(output, 0xCC, sizeof(output));
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source), output, 0U, 2U, 6U, 0U, 2U) == 0);
    assert(memcmp(expected, output, sizeof(expected)) == 0);
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source) - 3U, output,
               0U, 2U, 6U, 0U, 2U) ==
           -2305);
}

static void test_tune_guard_prefix_is_not_published(void)
{
    static const uint8_t source[] = {
        0xDE, 0xAD, 0xBE, 0xEF,
        0x10, 0x11, 0x20, 0x21,
        0x12, 0x13, 0x22, 0x23,
    };
    static const uint8_t expected[] = {
        0x10, 0x11, 0x20, 0x21,
        0x12, 0x13, 0x22, 0x23,
    };
    static const uint8_t strided_source[] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xEE, 0xEE,
        0x20, 0x21, 0x10, 0x11, 0xEE, 0xEE,
        0x22, 0x23, 0x12, 0x13, 0xEE, 0xEE,
    };
    uint8_t output[sizeof(expected)];
    const uint32_t hits = ra8p1_iio_mmap_fast_copy_hits_for_test();

    memset(output, 0xCC, sizeof(output));
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source), output, 1U, 2U, 4U, 0U, 2U) == 0);
    assert(memcmp(expected, output, sizeof(expected)) == 0);
    assert(ra8p1_iio_mmap_fast_copy_hits_for_test() == hits + 1U);
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               source, sizeof(source) - 1U, output,
               1U, 2U, 4U, 0U, 2U) == -2305);
    assert(ra8p1_iio_mmap_copy_rx1_for_test(
               strided_source, sizeof(strided_source), output,
               1U, 2U, 6U, 2U, 0U) == 0);
    assert(memcmp(expected, output, sizeof(expected)) == 0);
}

static void test_tune_guard_capture_plan_is_bounded(void)
{
    const uint32_t maximum_samples = (16U * 1024U * 1024U) / 4U;
    uint32_t dma_samples = 0U;
    uint32_t discard_samples = 0U;

    assert(ra8p1_iio_mmap_capture_plan_for_test(
               590336U, 4096U, 1U, &dma_samples, &discard_samples) == 0);
    assert(dma_samples == 594432U);
    assert(discard_samples == 4096U);
    assert(ra8p1_iio_mmap_capture_plan_for_test(
               590336U, 4096U, 0U, &dma_samples, &discard_samples) == 0);
    assert(dma_samples == 590336U);
    assert(discard_samples == 0U);
    assert(ra8p1_iio_mmap_capture_plan_for_test(
               maximum_samples - 100U, 4096U, 1U,
               &dma_samples, &discard_samples) == 0);
    assert(dma_samples == maximum_samples);
    assert(discard_samples == 100U);
    assert(ra8p1_iio_mmap_capture_plan_for_test(
               maximum_samples + 1U, 4096U, 1U,
               &dma_samples, &discard_samples) == -EINVAL);
}

int main(void)
{
    test_contiguous_iq_uses_contract_order();
    test_reversed_channels_use_fallback_shuffle();
    test_strided_layout_and_bounds_are_checked();
    test_tune_guard_prefix_is_not_published();
    test_tune_guard_capture_plan_is_bounded();
    puts("iio_mmap RX1 copy tests passed");
    return 0;
}
