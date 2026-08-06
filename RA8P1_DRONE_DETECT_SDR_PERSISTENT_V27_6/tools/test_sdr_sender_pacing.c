#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* Keep the production pacing implementation in one place while exposing its
 * static pure-state helpers to this translation-unit test. */
#define main ra8p1_sdr_sender_program_main
#include "sdr_iq_udp_stream.c"
#undef main

static int require_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual == expected)
    {
        return 1;
    }
    fprintf(stderr, "%s: actual=%" PRIu64 " expected=%" PRIu64 "\n",
            name, actual, expected);
    return 0;
}

int main(void)
{
    payload_pacer_t pacer;
    int passed = 1;

    passed &= require_u64("default batch", IQ_SEND_BATCH_DEFAULT, 1U);

    payload_pacer_init(&pacer, 40U, 1000U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("40 Mbps packet deadline", pacer.deadline_us, 1288U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("40 Mbps cumulative deadline", pacer.deadline_us, 1576U);

    payload_pacer_rebase_if_late(&pacer, 2000U);
    passed &= require_u64("small late deadline", pacer.deadline_us, 1576U);
    passed &= require_u64("small late bit debt", pacer.paced_bits, 23040U);
    passed &= require_u64("small late rebase count", pacer.late_rebases, 0U);
    passed &= require_u64("small late maximum", pacer.max_late_us, 424U);

    payload_pacer_rebase_if_late(&pacer,
                                 1576U + IQ_PACER_REBASE_LATE_US);
    passed &= require_u64("threshold deadline", pacer.deadline_us, 1576U);
    passed &= require_u64("threshold bit debt", pacer.paced_bits, 23040U);
    passed &= require_u64("threshold rebase count", pacer.late_rebases, 0U);
    passed &= require_u64("threshold maximum", pacer.max_late_us,
                          IQ_PACER_REBASE_LATE_US);

    payload_pacer_rebase_if_late(&pacer,
                                 1577U + IQ_PACER_REBASE_LATE_US);
    passed &= require_u64("threshold+1 rebase deadline", pacer.deadline_us,
                          1577U + IQ_PACER_REBASE_LATE_US);
    passed &= require_u64("threshold+1 rebase bit debt", pacer.paced_bits, 0U);
    passed &= require_u64("threshold+1 rebase count", pacer.late_rebases, 1U);
    passed &= require_u64("threshold+1 rebase maximum", pacer.max_late_us,
                          IQ_PACER_REBASE_LATE_US + 1U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("no catch-up deadline", pacer.deadline_us,
                          1577U + IQ_PACER_REBASE_LATE_US + 288U);

    payload_pacer_init(&pacer, 400U, 5000U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("fractional first deadline", pacer.deadline_us, 5029U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("fractional cumulative deadline", pacer.deadline_us, 5058U);

    payload_pacer_init(&pacer, 0U, 7000U);
    payload_pacer_rebase_if_late(&pacer, 9000U);
    payload_pacer_commit(&pacer, 1440U);
    passed &= require_u64("unpaced deadline", pacer.deadline_us, 7000U);
    passed &= require_u64("unpaced rebase count", pacer.late_rebases, 0U);

    if (!passed)
    {
        return 1;
    }
    puts("SDR sender pacing tests passed");
    return 0;
}
