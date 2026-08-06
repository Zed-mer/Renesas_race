#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdr_iq_udp_relay_state.h"

#define TEST_START_FLAG   (1U << 3)
#define TEST_SLOT_COUNT   (8U)
#define TEST_SLOT_BYTES   (32U)

static void accept_packet(ra8p1_iqrelay_tracker_t *tracker,
                          unsigned char slots[TEST_SLOT_COUNT][TEST_SLOT_BYTES],
                          unsigned int sequence,
                          unsigned int flags,
                          unsigned int source_address,
                          unsigned short source_port,
                          unsigned char marker,
                          ra8p1_iqrelay_accept_t expected)
{
    const unsigned int receive_slot = tracker->packet_count;
    ra8p1_iqrelay_accept_t result;

    assert(receive_slot < TEST_SLOT_COUNT);
    memset(slots[receive_slot], marker, TEST_SLOT_BYTES);
    result = ra8p1_iqrelay_tracker_classify(tracker, flags, TEST_START_FLAG,
                                            sequence, source_address,
                                            source_port);
    assert(result == expected);
    if (result == RA8P1_IQRELAY_RESTART_ACCEPT)
    {
        ra8p1_iqrelay_copy_datagram(slots[0], slots[receive_slot],
                                    TEST_SLOT_BYTES);
    }
    if ((result == RA8P1_IQRELAY_ACCEPT) ||
        (result == RA8P1_IQRELAY_RESTART_ACCEPT))
    {
        ra8p1_iqrelay_tracker_commit(tracker);
    }
}

int main(void)
{
    const unsigned int source_a = UINT32_C(0x0100007f);
    const unsigned int source_b = UINT32_C(0x0200007f);
    const unsigned short port_a = UINT16_C(0x3412);
    const unsigned short port_b = UINT16_C(0x7856);
    ra8p1_iqrelay_tracker_t tracker;
    unsigned char slots[TEST_SLOT_COUNT][TEST_SLOT_BYTES];

    assert(sizeof(unsigned int) == 4U);
    assert(sizeof(unsigned short) == 2U);
    memset(slots, 0, sizeof(slots));
    ra8p1_iqrelay_tracker_reset(&tracker);

    accept_packet(&tracker, slots, 0U, TEST_START_FLAG, source_a, port_a,
                  0x10U, RA8P1_IQRELAY_ACCEPT);
    accept_packet(&tracker, slots, 1U, 0U, source_a, port_a,
                  0x11U, RA8P1_IQRELAY_ACCEPT);
    accept_packet(&tracker, slots, 2U, 0U, source_a, port_a,
                  0x12U, RA8P1_IQRELAY_ACCEPT);
    assert(tracker.packet_count == 3U);

    /* A foreign sender cannot append to or replace the active window. */
    accept_packet(&tracker, slots, 3U, 0U, source_b, port_b,
                  0x20U, RA8P1_IQRELAY_DROP);
    accept_packet(&tracker, slots, 0U, TEST_START_FLAG, source_b, port_b,
                  0x21U, RA8P1_IQRELAY_DROP);
    assert(tracker.packet_count == 3U);
    assert(slots[0][0] == 0x10U);

    /* The same sender may restart.  Its new START was received in slot N,
     * and must become slot zero before the old window is discarded. */
    accept_packet(&tracker, slots, 0U, TEST_START_FLAG, source_a, port_a,
                  0x30U, RA8P1_IQRELAY_RESTART_ACCEPT);
    assert(tracker.active == 1U);
    assert(tracker.packet_count == 1U);
    assert(tracker.expected_sequence == 1U);
    assert(slots[0][0] == 0x30U);
    assert(slots[0][TEST_SLOT_BYTES - 1U] == 0x30U);

    accept_packet(&tracker, slots, 1U, 0U, source_a, port_a,
                  0x31U, RA8P1_IQRELAY_ACCEPT);
    assert(tracker.packet_count == 2U);

    /* A real sequence error retires the source lock and the partial window. */
    accept_packet(&tracker, slots, 3U, 0U, source_a, port_a,
                  0x32U, RA8P1_IQRELAY_SEQUENCE_RESET);
    assert(tracker.active == 0U);
    assert(tracker.packet_count == 0U);
    accept_packet(&tracker, slots, 0U, TEST_START_FLAG, source_b, port_b,
                  0x40U, RA8P1_IQRELAY_ACCEPT);
    assert(tracker.source_address == source_b);
    assert(tracker.source_port == port_b);

    puts("SDR IQ relay state tests passed");
    return 0;
}
