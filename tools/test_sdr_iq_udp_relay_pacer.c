#include <stdint.h>
#include <stdio.h>

#include "sdr_iq_udp_relay_pacer.h"

#define TEST_RATE_MBPS       800U
#define TEST_BATCH_BYTES     (16ULL * 1440ULL)

static unsigned int g_failures;

static void require_u64(const char *label, unsigned long long actual,
                        unsigned long long expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "%s: got %llu, expected %llu\n", label, actual,
                expected);
        g_failures++;
    }
}

static unsigned long long deadline_for(unsigned long long epoch_us,
                                       unsigned long long payload_bytes,
                                       unsigned int rate_mbps)
{
    const unsigned long long bits = payload_bytes * 8ULL;
    return epoch_us + (bits + rate_mbps - 1ULL) / rate_mbps;
}

static void test_cumulative_deadlines(void)
{
    struct payload_pacer pacer;
    const unsigned long long epoch_us = 1000000ULL;

    ra8p1_iqrelay_pacer_init(&pacer, TEST_RATE_MBPS, epoch_us);
    require_u64("init rate", pacer.rate_mbps, TEST_RATE_MBPS);
    require_u64("init epoch", pacer.epoch_us, epoch_us);
    require_u64("init deadline", pacer.deadline_us, epoch_us);
    require_u64("init payload", pacer.payload_bits, 0ULL);
    require_u64("init rebases", pacer.late_rebases, 0ULL);
    require_u64("init max late", pacer.max_late_us, 0ULL);

    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    require_u64("batch 1 deadline", pacer.deadline_us, epoch_us + 231ULL);
    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    require_u64("batch 2 cumulative deadline", pacer.deadline_us,
                epoch_us + 461ULL);
    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    require_u64("batch 3 cumulative deadline", pacer.deadline_us,
                epoch_us + 692ULL);
}

static void test_small_lateness_does_not_drift(void)
{
    struct payload_pacer pacer;
    const unsigned long long epoch_us = 2000000ULL;
    unsigned int batch;

    ra8p1_iqrelay_pacer_init(&pacer, 850U, epoch_us);
    for (batch = 0U; batch < 103U; batch++)
    {
        const unsigned long long late_us = (batch % 7U) + 1ULL;
        require_u64("small lateness classification",
                    ra8p1_iqrelay_pacer_observe(
                        &pacer, pacer.deadline_us + late_us),
                    0ULL);
        ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    }

    require_u64("small lateness epoch", pacer.epoch_us, epoch_us);
    require_u64("small lateness rebase count", pacer.late_rebases, 0ULL);
    require_u64("small lateness maximum", pacer.max_late_us, 7ULL);
    require_u64("103 batch absolute deadline", pacer.deadline_us,
                deadline_for(epoch_us, 103ULL * TEST_BATCH_BYTES, 850U));
    require_u64("103 batch payload debt", pacer.payload_bits,
                103ULL * TEST_BATCH_BYTES * 8ULL);
}

static void test_rebase_boundary_and_restart(void)
{
    struct payload_pacer pacer;
    unsigned long long old_deadline;
    unsigned long long rebase_us;

    ra8p1_iqrelay_pacer_init(&pacer, TEST_RATE_MBPS, 3000000ULL);
    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    old_deadline = pacer.deadline_us;

    require_u64("750 us is tolerated",
                ra8p1_iqrelay_pacer_observe(
                    &pacer,
                    old_deadline + RA8P1_IQRELAY_PACER_REBASE_LATE_US),
                0ULL);
    require_u64("boundary keeps epoch", pacer.epoch_us, 3000000ULL);
    require_u64("boundary keeps deadline", pacer.deadline_us, old_deadline);
    require_u64("boundary keeps payload debt", pacer.payload_bits,
                TEST_BATCH_BYTES * 8ULL);
    require_u64("boundary records maximum", pacer.max_late_us,
                RA8P1_IQRELAY_PACER_REBASE_LATE_US);

    rebase_us = old_deadline + RA8P1_IQRELAY_PACER_REBASE_LATE_US + 1ULL;
    require_u64("751 us rebases",
                ra8p1_iqrelay_pacer_observe(&pacer, rebase_us), 1ULL);
    require_u64("rebase epoch", pacer.epoch_us, rebase_us);
    require_u64("rebase deadline", pacer.deadline_us, rebase_us);
    require_u64("rebase clears payload debt", pacer.payload_bits, 0ULL);
    require_u64("rebase count", pacer.late_rebases, 1ULL);
    require_u64("rebase maximum", pacer.max_late_us,
                RA8P1_IQRELAY_PACER_REBASE_LATE_US + 1ULL);

    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    require_u64("post-stall deadline starts at new epoch", pacer.deadline_us,
                deadline_for(rebase_us, TEST_BATCH_BYTES, TEST_RATE_MBPS));
}

static void test_early_and_on_time_are_noops(void)
{
    struct payload_pacer pacer;

    ra8p1_iqrelay_pacer_init(&pacer, TEST_RATE_MBPS, 4000000ULL);
    ra8p1_iqrelay_pacer_commit(&pacer, TEST_BATCH_BYTES);
    require_u64("early observation",
                ra8p1_iqrelay_pacer_observe(&pacer,
                                             pacer.deadline_us - 1ULL),
                0ULL);
    require_u64("on-time observation",
                ra8p1_iqrelay_pacer_observe(&pacer, pacer.deadline_us),
                0ULL);
    require_u64("no-op max late", pacer.max_late_us, 0ULL);
    require_u64("no-op rebase count", pacer.late_rebases, 0ULL);
}

int main(void)
{
    test_cumulative_deadlines();
    test_small_lateness_does_not_drift();
    test_rebase_boundary_and_restart();
    test_early_and_on_time_are_noops();

    if (g_failures != 0U)
    {
        fprintf(stderr, "SDR IQ relay pacer tests failed: %u\n", g_failures);
        return 1;
    }
    puts("SDR IQ relay pacer tests passed");
    return 0;
}
