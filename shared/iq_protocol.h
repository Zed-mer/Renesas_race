#ifndef RA8P1_IQ_PROTOCOL_H
#define RA8P1_IQ_PROTOCOL_H

#include <stdint.h>
#include "system_protocol.h"

#define RA8P1_IQ_PACKET_MAGIC       (0x5149504BUL)
#define RA8P1_IQ_PACKET_HEADER_SIZE (32U)
#define RA8P1_IQ_STREAM_CONFIG_MAGIC (0x49515343UL) /* IQSC */
#define RA8P1_IQ_STREAM_CONFIG_VERSION (2U)

#define RA8P1_IQ_CRC32C_INIT          (0xFFFFFFFFUL)
#define RA8P1_IQ_CRC32C_XOROUT        (0xFFFFFFFFUL)
#define RA8P1_IQ_CRC32C_TRAILER_BYTES (4U)

#define RA8P1_IQ_ACK_REQUEST_MAGIC  (0x5141434BUL)
#define RA8P1_IQ_ACK_RESPONSE_MAGIC (0x51415253UL)
#define RA8P1_IQ_ACK_VERSION        (1U)

#define RA8P1_IQ_FLAG_SYNTHETIC     (1UL << 0)
#define RA8P1_IQ_FLAG_DISCONTINUITY (1UL << 1)
#define RA8P1_IQ_FLAG_FREQUENCY_B   (1UL << 2)
#define RA8P1_IQ_FLAG_STREAM_START  (1UL << 3)
#define RA8P1_IQ_FLAG_STREAM_END    (1UL << 4)
#define RA8P1_IQ_FLAG_VALID_BITS_12 (1UL << 5)
#define RA8P1_IQ_FLAG_WINDOW_CRC    (1UL << 6)

#define RA8P1_IQ_ACK_FLAG_ACTIVE         (1UL << 0)
#define RA8P1_IQ_ACK_FLAG_COMPLETE       (1UL << 1)
#define RA8P1_IQ_ACK_FLAG_CRC_PRESENT    (1UL << 2)
#define RA8P1_IQ_ACK_FLAG_CRC_VALID      (1UL << 3)
#define RA8P1_IQ_ACK_FLAG_SEQUENCE_ERROR (1UL << 4)
#define RA8P1_IQ_ACK_FLAG_RING_DROP      (1UL << 5)

typedef enum e_ra8p1_iq_ack_status
{
    RA8P1_IQ_ACK_STATUS_OK = 0U,
    RA8P1_IQ_ACK_STATUS_ACTIVE = 1U,
    RA8P1_IQ_ACK_STATUS_NO_SESSION = 2U,
    RA8P1_IQ_ACK_STATUS_SESSION_MISMATCH = 3U,
    RA8P1_IQ_ACK_STATUS_CRC_MISMATCH = 4U,
    RA8P1_IQ_ACK_STATUS_SEQUENCE_ERROR = 5U,
    RA8P1_IQ_ACK_STATUS_RING_DROP = 6U,
    RA8P1_IQ_ACK_STATUS_INVALID_DATA = 7U,
    RA8P1_IQ_ACK_STATUS_INVALID_REQUEST = 8U
} ra8p1_iq_ack_status_t;

typedef struct st_ra8p1_iq_packet_header
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t data_length;
    uint32_t flags;
    uint32_t sample_index_low;
    uint32_t sample_index_high;
    uint32_t sender_us;
    uint32_t format;
} ra8p1_iq_packet_header_t;

/* Sent in the first/last packet alongside the normal 32-byte IQ header. */
typedef struct __attribute__((packed, aligned(4))) st_ra8p1_iq_stream_config
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t session_id;
    uint32_t source_sample_rate_hz;
    uint32_t sample_rate_hz;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    uint32_t window_samples;
    uint32_t total_samples_low;
    uint32_t total_samples_high;
    uint32_t tile_stride_samples;
    uint32_t format;
    uint32_t valid_bits;
    uint32_t channel_mask;
    uint32_t flags;
    uint32_t center_index;
} ra8p1_iq_stream_config_t;

typedef struct st_ra8p1_iq_ack_request
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t request_id;
    uint32_t session_id;
} ra8p1_iq_ack_request_t;

typedef struct __attribute__((packed, aligned(4))) st_ra8p1_iq_ack_response
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t request_id;
    uint32_t session_id;
    uint32_t status;
    uint32_t flags;
    uint32_t packets;
    uint64_t payload_bytes;
    uint32_t sequence_gaps;
    uint32_t reordered;
    uint32_t invalid;
    uint32_t ring_full_drops;
    uint32_t ring_oversize_drops;
    uint32_t ring_free;
    uint32_t crc32c;
    uint32_t expected_crc32c;
    uint32_t crc_errors;
} ra8p1_iq_ack_response_t;

typedef char ra8p1_iq_header_size_must_be_32[(sizeof(ra8p1_iq_packet_header_t) == 32U) ? 1 : -1];
typedef char ra8p1_iq_stream_config_size_must_be_68[(sizeof(ra8p1_iq_stream_config_t) == 68U) ? 1 : -1];
typedef char ra8p1_iq_ack_request_size_must_be_16[
    (sizeof(ra8p1_iq_ack_request_t) == 16U) ? 1 : -1];
typedef char ra8p1_iq_ack_response_size_must_be_72[
    (sizeof(ra8p1_iq_ack_response_t) == 72U) ? 1 : -1];

#endif
