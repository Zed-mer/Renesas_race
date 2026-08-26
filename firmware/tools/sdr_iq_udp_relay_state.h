#ifndef RA8P1_SDR_IQ_UDP_RELAY_STATE_H
#define RA8P1_SDR_IQ_UDP_RELAY_STATE_H

typedef struct st_ra8p1_iqrelay_tracker
{
    unsigned int active;
    unsigned int packet_count;
    unsigned int expected_sequence;
    unsigned int source_address;
    unsigned short source_port;
} ra8p1_iqrelay_tracker_t;

typedef enum e_ra8p1_iqrelay_accept
{
    RA8P1_IQRELAY_DROP = 0,
    RA8P1_IQRELAY_ACCEPT,
    RA8P1_IQRELAY_RESTART_ACCEPT,
    RA8P1_IQRELAY_SEQUENCE_RESET
} ra8p1_iqrelay_accept_t;

static inline void ra8p1_iqrelay_tracker_reset(ra8p1_iqrelay_tracker_t *tracker)
{
    tracker->active = 0U;
    tracker->packet_count = 0U;
    tracker->expected_sequence = 0U;
    tracker->source_address = 0U;
    tracker->source_port = 0U;
}

static inline ra8p1_iqrelay_accept_t ra8p1_iqrelay_tracker_classify(
    ra8p1_iqrelay_tracker_t *tracker,
    unsigned int flags,
    unsigned int start_flag,
    unsigned int sequence,
    unsigned int source_address,
    unsigned short source_port)
{
    const unsigned int is_start = ((flags & start_flag) != 0U);
    const unsigned int had_packets = tracker->packet_count;

    if (is_start != 0U)
    {
        if ((tracker->active != 0U) &&
            ((tracker->source_address != source_address) ||
             (tracker->source_port != source_port)))
        {
            return RA8P1_IQRELAY_DROP;
        }
        tracker->active = 1U;
        tracker->packet_count = 0U;
        tracker->expected_sequence = 0U;
        tracker->source_address = source_address;
        tracker->source_port = source_port;
    }
    else if ((tracker->active == 0U) ||
             (tracker->source_address != source_address) ||
             (tracker->source_port != source_port))
    {
        return RA8P1_IQRELAY_DROP;
    }

    if (sequence != tracker->expected_sequence)
    {
        ra8p1_iqrelay_tracker_reset(tracker);
        return RA8P1_IQRELAY_SEQUENCE_RESET;
    }

    return ((is_start != 0U) && (had_packets != 0U)) ?
        RA8P1_IQRELAY_RESTART_ACCEPT : RA8P1_IQRELAY_ACCEPT;
}

static inline void ra8p1_iqrelay_tracker_commit(
    ra8p1_iqrelay_tracker_t *tracker)
{
    tracker->packet_count++;
    tracker->expected_sequence++;
}

static inline void ra8p1_iqrelay_copy_datagram(unsigned char *destination,
                                                const unsigned char *source,
                                                unsigned int length)
{
    unsigned int index;
    for (index = 0U; index < length; index++)
    {
        destination[index] = source[index];
    }
}

#endif
