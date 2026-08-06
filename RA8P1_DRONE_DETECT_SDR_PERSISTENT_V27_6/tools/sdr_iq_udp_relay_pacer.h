#ifndef RA8P1_SDR_IQ_UDP_RELAY_PACER_H
#define RA8P1_SDR_IQ_UDP_RELAY_PACER_H

/* Normal syscall and clock-quantization jitter must not move the pacing epoch.
 * A longer scheduler stall is rebased to avoid emitting a catch-up burst. */
#define RA8P1_IQRELAY_PACER_REBASE_LATE_US 750ULL

struct payload_pacer
{
    unsigned int rate_mbps;
    unsigned long long epoch_us;
    unsigned long long deadline_us;
    unsigned long long payload_bits;
    unsigned int late_rebases;
    unsigned long long max_late_us;
};

static inline void ra8p1_iqrelay_pacer_init(struct payload_pacer *pacer,
                                            unsigned int rate_mbps,
                                            unsigned long long now_us)
{
    pacer->rate_mbps = rate_mbps;
    pacer->epoch_us = now_us;
    pacer->deadline_us = now_us;
    pacer->payload_bits = 0ULL;
    pacer->late_rebases = 0U;
    pacer->max_late_us = 0ULL;
}

static inline unsigned int ra8p1_iqrelay_pacer_observe(
    struct payload_pacer *pacer,
    unsigned long long now_us)
{
    unsigned long long late_us;

    if (now_us <= pacer->deadline_us)
    {
        return 0U;
    }

    late_us = now_us - pacer->deadline_us;
    if (late_us > pacer->max_late_us)
    {
        pacer->max_late_us = late_us;
    }
    if (late_us <= RA8P1_IQRELAY_PACER_REBASE_LATE_US)
    {
        return 0U;
    }

    pacer->late_rebases++;
    pacer->epoch_us = now_us;
    pacer->deadline_us = now_us;
    pacer->payload_bits = 0ULL;
    return 1U;
}

static inline void ra8p1_iqrelay_pacer_commit(struct payload_pacer *pacer,
                                              unsigned long long payload_bytes)
{
    pacer->payload_bits += payload_bytes * 8ULL;
    pacer->deadline_us = pacer->epoch_us +
        (pacer->payload_bits + pacer->rate_mbps - 1ULL) /
        pacer->rate_mbps;
}

#endif
