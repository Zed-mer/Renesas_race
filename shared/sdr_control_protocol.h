#ifndef RA8P1_SDR_CONTROL_PROTOCOL_H
#define RA8P1_SDR_CONTROL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * SDRC v3 is the CPU0-to-SDR control plane. IQ samples remain on IQSC/UDP
 * port 5003 and the existing IQ window-status query remains on UDP port 5002.
 * Every multi-byte SDRC field is encoded little-endian at an explicit offset;
 * the in-memory structure below is never sent directly.
 */
#define RA8P1_SDR_CONTROL_MAGIC                (0x43524453UL) /* "SDRC" */
#define RA8P1_SDR_CONTROL_VERSION              (3U)
#define RA8P1_SDR_CONTROL_PORT                 (5004U)
#define RA8P1_SDR_CONTROL_WIRE_BYTES           (164U)
#define RA8P1_SDR_CONTROL_CRC_OFFSET           (160U)
#define RA8P1_SDR_CONTROL_DEFAULT_SAMPLE_RATE  (60000000UL)
#define RA8P1_SDR_CONTROL_DEFAULT_BANDWIDTH    (56000000UL)
#define RA8P1_SDR_CONTROL_DEFAULT_SAMPLES      (590336UL)
#define RA8P1_SDR_CONTROL_COMPAT_SAMPLES       (6000000UL)
#define RA8P1_SDR_CONTROL_CENTER_COUNT         (4U)
#define RA8P1_SDR_CONTROL_RING_SLOTS           (4096U)
#define RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000 (1000UL)
#define RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000 (940000UL)

#define RA8P1_SDR_TEST_FAULT_CRC32C                (1UL << 0)
#define RA8P1_SDR_TEST_FAULT_DROP_DATA_PACKET      (1UL << 1)
#define RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_REQUEST  (1UL << 2)
#define RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_ACK_RESPONSE (1UL << 3)
#define RA8P1_SDR_TEST_FAULT_ALL \
    (RA8P1_SDR_TEST_FAULT_CRC32C | \
     RA8P1_SDR_TEST_FAULT_DROP_DATA_PACKET | \
     RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_REQUEST | \
     RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_ACK_RESPONSE)

#define RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY     (1U << 0)
#define RA8P1_SDR_CONTROL_FLAG_WINDOW_CRC32C   (1U << 1)
#define RA8P1_SDR_CONTROL_FLAG_FASTLOCK        (1U << 2)
#define RA8P1_SDR_CONTROL_FLAG_DOUBLE_BUFFER   (1U << 3)
#define RA8P1_SDR_CONTROL_FLAG_RETRANSMIT      (1U << 4)
#define RA8P1_SDR_CONTROL_FLAG_COMPAT_6M       (1U << 5)
#define RA8P1_SDR_CONTROL_FLAG_ALL             \
    (RA8P1_SDR_CONTROL_FLAG_LOW_LATENCY |       \
     RA8P1_SDR_CONTROL_FLAG_WINDOW_CRC32C |     \
     RA8P1_SDR_CONTROL_FLAG_FASTLOCK |          \
     RA8P1_SDR_CONTROL_FLAG_DOUBLE_BUFFER |     \
     RA8P1_SDR_CONTROL_FLAG_RETRANSMIT |        \
     RA8P1_SDR_CONTROL_FLAG_COMPAT_6M)

typedef enum e_ra8p1_sdr_control_command
{
    RA8P1_SDR_CONTROL_CAPTURE_REQ = 1U,
    RA8P1_SDR_CONTROL_WINDOW_ACK = 2U,
    RA8P1_SDR_CONTROL_CANCEL = 3U,
    RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED = 0x8001U,
    RA8P1_SDR_CONTROL_CAPTURE_STARTED = 0x8002U,
    RA8P1_SDR_CONTROL_CAPTURE_COMPLETE = 0x8003U,
    RA8P1_SDR_CONTROL_CREDIT_ACCEPTED = 0x8004U,
    /* Capture is cached locally but cannot be sent until CPU0 grants credit. */
    RA8P1_SDR_CONTROL_CAPTURE_READY = 0x8005U,
    RA8P1_SDR_CONTROL_ERROR = 0x80FFU
} ra8p1_sdr_control_command_t;

typedef enum e_ra8p1_sdr_control_status
{
    RA8P1_SDR_CONTROL_STATUS_OK = 0U,
    RA8P1_SDR_CONTROL_STATUS_BUSY = 1U,
    RA8P1_SDR_CONTROL_STATUS_INVALID_MESSAGE = 2U,
    RA8P1_SDR_CONTROL_STATUS_INVALID_REQUEST = 3U,
    RA8P1_SDR_CONTROL_STATUS_NO_CREDIT = 4U,
    RA8P1_SDR_CONTROL_STATUS_TUNE_FAILED = 5U,
    RA8P1_SDR_CONTROL_STATUS_CAPTURE_FAILED = 6U,
    RA8P1_SDR_CONTROL_STATUS_SEND_FAILED = 7U,
    RA8P1_SDR_CONTROL_STATUS_ACK_TIMEOUT = 8U,
    RA8P1_SDR_CONTROL_STATUS_IQ_GAP = 9U,
    RA8P1_SDR_CONTROL_STATUS_IQ_DROP = 10U,
    RA8P1_SDR_CONTROL_STATUS_IQ_CRC = 11U,
    RA8P1_SDR_CONTROL_STATUS_RESULT_TIMEOUT = 12U,
    RA8P1_SDR_CONTROL_STATUS_RETRY_WINDOW = 13U,
    RA8P1_SDR_CONTROL_STATUS_CANCELLED = 14U,
    RA8P1_SDR_CONTROL_STATUS_INTERNAL = 15U,
    RA8P1_SDR_CONTROL_STATUS_STALE_EPOCH = 16U,
    RA8P1_SDR_CONTROL_STATUS_WINDOW_NOT_READY = 17U
} ra8p1_sdr_control_status_t;

/* Host-order representation of one SDRC datagram. */
typedef struct st_ra8p1_sdr_control_message
{
    uint16_t command;
    uint16_t flags;
    uint32_t request_id;
    uint32_t session_id;
    uint32_t center_index;
    uint64_t center_frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t sample_count;
    uint32_t target_payload_mbps_x1000;
    uint16_t send_batch;
    uint16_t retry_limit;
    uint32_t ack_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t credit;
    uint32_t ring_free;
    uint32_t status;
    uint32_t attempt;
    uint64_t agent_request_rx_us;
    uint64_t tune_start_us;
    uint64_t tune_complete_us;
    uint64_t capture_start_us;
    uint64_t capture_complete_us;
    uint32_t window_crc32c;
    uint32_t actual_payload_mbps_x1000;
    /* Nonzero CPU0 boot nonce. It makes request/session IDs restart-safe. */
    uint64_t boot_epoch;
    /* WINDOW_ACK receiver evidence for this exact IQSC session. */
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid_packets;
    uint32_t ring_full_drops;
    uint32_t ring_oversize_drops;
    uint32_t crc_errors;
    uint32_t test_fault_flags;
    uint32_t message_crc32c;
} ra8p1_sdr_control_message_t;

static inline uint64_t ra8p1_sdr_control_center_frequency(uint32_t index)
{
    static const uint64_t centers[RA8P1_SDR_CONTROL_CENTER_COUNT] =
    {
        2420000000ULL,
        2464000000ULL,
        5760000000ULL,
        5816000000ULL
    };
    return (index < RA8P1_SDR_CONTROL_CENTER_COUNT) ? centers[index] : 0ULL;
}

static inline void ra8p1_sdr_control_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
}

static inline void ra8p1_sdr_control_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

static inline void ra8p1_sdr_control_put_le64(uint8_t *p, uint64_t value)
{
    ra8p1_sdr_control_put_le32(p, (uint32_t)value);
    ra8p1_sdr_control_put_le32(p + 4U, (uint32_t)(value >> 32U));
}

static inline uint16_t ra8p1_sdr_control_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static inline uint32_t ra8p1_sdr_control_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static inline uint64_t ra8p1_sdr_control_get_le64(const uint8_t *p)
{
    return (uint64_t)ra8p1_sdr_control_get_le32(p) |
           ((uint64_t)ra8p1_sdr_control_get_le32(p + 4U) << 32U);
}

/* Standard reflected CRC-32C (Castagnoli), init/final xor 0xffffffff. */
static inline uint32_t ra8p1_sdr_control_crc32c(const uint8_t *data,
                                                size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t i;
    for (i = 0U; i < length; ++i)
    {
        uint32_t bit;
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            const uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0x82F63B78UL & mask);
        }
    }
    return ~crc;
}

static inline bool ra8p1_sdr_control_command_valid(uint16_t command)
{
    return (command == RA8P1_SDR_CONTROL_CAPTURE_REQ) ||
           (command == RA8P1_SDR_CONTROL_WINDOW_ACK) ||
           (command == RA8P1_SDR_CONTROL_CANCEL) ||
           (command == RA8P1_SDR_CONTROL_CAPTURE_ACCEPTED) ||
           (command == RA8P1_SDR_CONTROL_CAPTURE_STARTED) ||
           (command == RA8P1_SDR_CONTROL_CAPTURE_COMPLETE) ||
           (command == RA8P1_SDR_CONTROL_CREDIT_ACCEPTED) ||
           (command == RA8P1_SDR_CONTROL_CAPTURE_READY) ||
           (command == RA8P1_SDR_CONTROL_ERROR);
}

static inline bool ra8p1_sdr_control_encode(
    const ra8p1_sdr_control_message_t *message,
    uint8_t wire[RA8P1_SDR_CONTROL_WIRE_BYTES])
{
    uint32_t crc;
    if ((message == NULL) || (wire == NULL) ||
        !ra8p1_sdr_control_command_valid(message->command) ||
        ((message->flags & ~RA8P1_SDR_CONTROL_FLAG_ALL) != 0U) ||
        ((message->test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) != 0U))
    {
        return false;
    }
    memset(wire, 0, RA8P1_SDR_CONTROL_WIRE_BYTES);
    ra8p1_sdr_control_put_le32(&wire[0], RA8P1_SDR_CONTROL_MAGIC);
    ra8p1_sdr_control_put_le16(&wire[4], RA8P1_SDR_CONTROL_VERSION);
    ra8p1_sdr_control_put_le16(&wire[6], RA8P1_SDR_CONTROL_WIRE_BYTES);
    ra8p1_sdr_control_put_le16(&wire[8], message->command);
    ra8p1_sdr_control_put_le16(&wire[10], message->flags);
    ra8p1_sdr_control_put_le32(&wire[12], message->request_id);
    ra8p1_sdr_control_put_le32(&wire[16], message->session_id);
    ra8p1_sdr_control_put_le32(&wire[20], message->center_index);
    ra8p1_sdr_control_put_le64(&wire[24], message->center_frequency_hz);
    ra8p1_sdr_control_put_le32(&wire[32], message->sample_rate_hz);
    ra8p1_sdr_control_put_le32(&wire[36], message->bandwidth_hz);
    ra8p1_sdr_control_put_le32(&wire[40], message->sample_count);
    ra8p1_sdr_control_put_le32(&wire[44], message->target_payload_mbps_x1000);
    ra8p1_sdr_control_put_le16(&wire[48], message->send_batch);
    ra8p1_sdr_control_put_le16(&wire[50], message->retry_limit);
    ra8p1_sdr_control_put_le32(&wire[52], message->ack_timeout_ms);
    ra8p1_sdr_control_put_le32(&wire[56], message->request_timeout_ms);
    ra8p1_sdr_control_put_le32(&wire[60], message->credit);
    ra8p1_sdr_control_put_le32(&wire[64], message->ring_free);
    ra8p1_sdr_control_put_le32(&wire[68], message->status);
    ra8p1_sdr_control_put_le32(&wire[72], message->attempt);
    ra8p1_sdr_control_put_le64(&wire[76], message->agent_request_rx_us);
    ra8p1_sdr_control_put_le64(&wire[84], message->tune_start_us);
    ra8p1_sdr_control_put_le64(&wire[92], message->tune_complete_us);
    ra8p1_sdr_control_put_le64(&wire[100], message->capture_start_us);
    ra8p1_sdr_control_put_le64(&wire[108], message->capture_complete_us);
    ra8p1_sdr_control_put_le32(&wire[116], message->window_crc32c);
    ra8p1_sdr_control_put_le32(&wire[120], message->actual_payload_mbps_x1000);
    ra8p1_sdr_control_put_le64(&wire[124], message->boot_epoch);
    ra8p1_sdr_control_put_le32(&wire[132], message->sequence_gaps);
    ra8p1_sdr_control_put_le32(&wire[136], message->reordered);
    ra8p1_sdr_control_put_le32(&wire[140], message->invalid_packets);
    ra8p1_sdr_control_put_le32(&wire[144], message->ring_full_drops);
    ra8p1_sdr_control_put_le32(&wire[148], message->ring_oversize_drops);
    ra8p1_sdr_control_put_le32(&wire[152], message->crc_errors);
    ra8p1_sdr_control_put_le32(&wire[156], message->test_fault_flags);
    crc = ra8p1_sdr_control_crc32c(wire, RA8P1_SDR_CONTROL_CRC_OFFSET);
    ra8p1_sdr_control_put_le32(&wire[RA8P1_SDR_CONTROL_CRC_OFFSET], crc);
    return true;
}

static inline bool ra8p1_sdr_control_decode(
    const uint8_t *wire,
    size_t length,
    ra8p1_sdr_control_message_t *message)
{
    uint32_t expected_crc;
    if ((wire == NULL) || (message == NULL) ||
        (length != RA8P1_SDR_CONTROL_WIRE_BYTES) ||
        (ra8p1_sdr_control_get_le32(&wire[0]) != RA8P1_SDR_CONTROL_MAGIC) ||
        (ra8p1_sdr_control_get_le16(&wire[4]) != RA8P1_SDR_CONTROL_VERSION) ||
        (ra8p1_sdr_control_get_le16(&wire[6]) != RA8P1_SDR_CONTROL_WIRE_BYTES))
    {
        return false;
    }
    expected_crc = ra8p1_sdr_control_get_le32(
        &wire[RA8P1_SDR_CONTROL_CRC_OFFSET]);
    if (expected_crc !=
        ra8p1_sdr_control_crc32c(wire, RA8P1_SDR_CONTROL_CRC_OFFSET))
    {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->command = ra8p1_sdr_control_get_le16(&wire[8]);
    message->flags = ra8p1_sdr_control_get_le16(&wire[10]);
    if (!ra8p1_sdr_control_command_valid(message->command) ||
        ((message->flags & ~RA8P1_SDR_CONTROL_FLAG_ALL) != 0U))
    {
        return false;
    }
    message->request_id = ra8p1_sdr_control_get_le32(&wire[12]);
    message->session_id = ra8p1_sdr_control_get_le32(&wire[16]);
    message->center_index = ra8p1_sdr_control_get_le32(&wire[20]);
    message->center_frequency_hz = ra8p1_sdr_control_get_le64(&wire[24]);
    message->sample_rate_hz = ra8p1_sdr_control_get_le32(&wire[32]);
    message->bandwidth_hz = ra8p1_sdr_control_get_le32(&wire[36]);
    message->sample_count = ra8p1_sdr_control_get_le32(&wire[40]);
    message->target_payload_mbps_x1000 = ra8p1_sdr_control_get_le32(&wire[44]);
    message->send_batch = ra8p1_sdr_control_get_le16(&wire[48]);
    message->retry_limit = ra8p1_sdr_control_get_le16(&wire[50]);
    message->ack_timeout_ms = ra8p1_sdr_control_get_le32(&wire[52]);
    message->request_timeout_ms = ra8p1_sdr_control_get_le32(&wire[56]);
    message->credit = ra8p1_sdr_control_get_le32(&wire[60]);
    message->ring_free = ra8p1_sdr_control_get_le32(&wire[64]);
    message->status = ra8p1_sdr_control_get_le32(&wire[68]);
    message->attempt = ra8p1_sdr_control_get_le32(&wire[72]);
    message->agent_request_rx_us = ra8p1_sdr_control_get_le64(&wire[76]);
    message->tune_start_us = ra8p1_sdr_control_get_le64(&wire[84]);
    message->tune_complete_us = ra8p1_sdr_control_get_le64(&wire[92]);
    message->capture_start_us = ra8p1_sdr_control_get_le64(&wire[100]);
    message->capture_complete_us = ra8p1_sdr_control_get_le64(&wire[108]);
    message->window_crc32c = ra8p1_sdr_control_get_le32(&wire[116]);
    message->actual_payload_mbps_x1000 =
        ra8p1_sdr_control_get_le32(&wire[120]);
    message->boot_epoch = ra8p1_sdr_control_get_le64(&wire[124]);
    message->sequence_gaps = ra8p1_sdr_control_get_le32(&wire[132]);
    message->reordered = ra8p1_sdr_control_get_le32(&wire[136]);
    message->invalid_packets = ra8p1_sdr_control_get_le32(&wire[140]);
    message->ring_full_drops = ra8p1_sdr_control_get_le32(&wire[144]);
    message->ring_oversize_drops = ra8p1_sdr_control_get_le32(&wire[148]);
    message->crc_errors = ra8p1_sdr_control_get_le32(&wire[152]);
    message->test_fault_flags = ra8p1_sdr_control_get_le32(&wire[156]);
    message->message_crc32c = expected_crc;
    return (message->test_fault_flags & ~RA8P1_SDR_TEST_FAULT_ALL) == 0U;
}

typedef char ra8p1_sdr_control_wire_must_be_164[
    (RA8P1_SDR_CONTROL_WIRE_BYTES == 164U) ? 1 : -1];
typedef char ra8p1_sdr_control_crc_must_be_last_word[
    ((RA8P1_SDR_CONTROL_CRC_OFFSET + 4U) ==
     RA8P1_SDR_CONTROL_WIRE_BYTES) ? 1 : -1];

#endif
