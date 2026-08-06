#ifndef RA8P1_ZYNQ_IQSC_SENDER_H
#define RA8P1_ZYNQ_IQSC_SENDER_H

/*
 * OS/socket-independent IQSC v2 sender for a Zynq-7020 bare-metal project.
 *
 * The caller keeps ownership of its existing lwIP/netif/IP configuration and
 * supplies synchronous UDP, delay, and monotonic-time callbacks.  This module
 * never creates a socket, binds an address, changes eth0/usb0, or interferes
 * with a Pluto/libiio service.  The UDP callback receives the IQ header and
 * payload as separate spans so a raw-lwIP adapter may use a pbuf chain and
 * avoid an otherwise mandatory 24 MB/session payload copy.
 */

#include <stddef.h>
#include <stdint.h>

#include "../shared/analysis_contract.h"
#include "../shared/iq_protocol.h"
#include "sdr_scan_bridge.h"

#define RA8P1_IQSC_UDP_PORT                 (5003U)
#define RA8P1_IQSC_MAX_DATAGRAM_BYTES       (1472U)
#define RA8P1_IQSC_IQ_BYTES_PER_SAMPLE      (4U)
#define RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM  \
    (RA8P1_IQSC_MAX_DATAGRAM_BYTES - RA8P1_IQ_PACKET_HEADER_SIZE)
#define RA8P1_IQSC_SAMPLES_PER_DATAGRAM     \
    (RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM / RA8P1_IQSC_IQ_BYTES_PER_SAMPLE)
#define RA8P1_IQSC_DATA_DATAGRAMS_PER_SESSION \
    ((uint32_t) (((RA8P1_ANALYSIS_TOTAL_SAMPLES * \
                   RA8P1_IQSC_IQ_BYTES_PER_SAMPLE) + \
                  RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM - 1ULL) / \
                 RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM))
#define RA8P1_IQSC_DATAGRAMS_PER_SESSION \
    (RA8P1_IQSC_DATA_DATAGRAMS_PER_SESSION + 2U)

#define RA8P1_IQSC_OK             (0)
#define RA8P1_IQSC_EINVAL         (-2001)
#define RA8P1_IQSC_ESTATE         (-2002)
#define RA8P1_IQSC_ETRANSPORT     (-2003)
#define RA8P1_IQSC_ETIME          (-2004)
#define RA8P1_IQSC_EPACING        (-2005)
#define RA8P1_IQSC_ECOUNT         (-2006)

/*
 * The destination IPv4 value is opaque to this module.  Store it in the byte
 * order expected by the selected lwIP adapter and pass it through unchanged.
 * Both spans are valid only until the callback returns.  The callback must
 * have synchronously consumed, copied, or queued all bytes before returning 0.
 */
typedef int32_t (*ra8p1_iqsc_udp_send_fn)(
    void *callback_context,
    uint32_t destination_ipv4,
    uint16_t destination_port,
    const uint8_t *header,
    uint16_t header_bytes,
    const uint8_t *payload,
    uint16_t payload_bytes);

typedef int32_t (*ra8p1_iqsc_delay_us_fn)(
    void *callback_context, uint32_t delay_us);

typedef int32_t (*ra8p1_iqsc_time_us_fn)(
    void *callback_context, uint64_t *time_us);

typedef struct st_ra8p1_iqsc_transport
{
    uint32_t destination_ipv4;
    /* 0 sends as fast as the transport permits; otherwise payload Mbps. */
    uint32_t target_payload_mbps;
    void *callback_context;
    ra8p1_iqsc_udp_send_fn udp_send;
    ra8p1_iqsc_delay_us_fn delay_us;
    ra8p1_iqsc_time_us_fn time_us;
} ra8p1_iqsc_transport_t;

typedef struct st_ra8p1_iqsc_stats
{
    uint64_t sessions_started;
    uint64_t sessions_completed;
    uint64_t sessions_failed;
    uint64_t capture_windows_completed;
    uint64_t capture_centers_completed;
    uint64_t datagrams_sent;
    uint64_t control_datagrams_sent;
    uint64_t data_datagrams_sent;
    uint64_t iq_payload_bytes_sent;
    uint64_t udp_payload_bytes_sent;
    uint64_t pacing_delay_calls;
    uint64_t pacing_delay_us_requested;
    uint64_t last_session_elapsed_us;
    uint32_t last_payload_mbps_x1000;
    uint32_t last_session_id;
    uint32_t last_center_index;
    uint32_t last_sequence;
    int32_t last_transport_status;
    uint64_t last_sample_index;
    int32_t last_status;
} ra8p1_iqsc_stats_t;

typedef struct st_ra8p1_iqsc_sender
{
    ra8p1_iqsc_transport_t transport;
    ra8p1_iqsc_stats_t stats;
    uint8_t header[RA8P1_IQ_PACKET_HEADER_SIZE];
    uint8_t config[sizeof(ra8p1_iq_stream_config_t)];
    union
    {
        uint64_t alignment;
        uint8_t bytes[RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM];
    } payload_scratch;
    uint32_t initialized;
    uint32_t active;
    uint32_t active_session_id;
    uint32_t active_center_index;
    uint32_t next_sequence;
    uint64_t next_sample_index;
    uint64_t session_started_us;
    uint64_t session_payload_bytes;
} ra8p1_iqsc_sender_t;

int32_t ra8p1_iqsc_sender_init(
    ra8p1_iqsc_sender_t *sender,
    const ra8p1_iqsc_transport_t *transport);

void ra8p1_iqsc_sender_reset_stats(ra8p1_iqsc_sender_t *sender);

int32_t ra8p1_iqsc_session_begin(
    ra8p1_iqsc_sender_t *sender,
    uint32_t session_id,
    uint32_t center_index);

int32_t ra8p1_iqsc_session_write(
    ra8p1_iqsc_sender_t *sender,
    const uint8_t *rx1_iq_le,
    uint32_t sample_count);

/* Convert documented 2R2T DMA records packet-by-packet into the hot scratch. */
int32_t ra8p1_iqsc_session_write_dma(
    ra8p1_iqsc_sender_t *sender,
    const ra8p1_sdr_adapter_api_t *sdr_api,
    const void *dma_staging,
    uint32_t sample_count);

int32_t ra8p1_iqsc_session_end(ra8p1_iqsc_sender_t *sender);

void ra8p1_iqsc_session_abort(
    ra8p1_iqsc_sender_t *sender, int32_t reason);

int32_t ra8p1_iqsc_send_cached_session(
    ra8p1_iqsc_sender_t *sender,
    uint32_t session_id,
    uint32_t center_index,
    const uint8_t *rx1_iq_le,
    uint32_t sample_count);

/*
 * At each fixed center, capture request_template->chunk_samples, convert RX1,
 * and immediately send that window before starting the next DMA capture.  A
 * tile-sized chunk (590336 samples, about 9.84 ms at 60 MSPS) minimizes first
 * inference latency while the IQSC session still totals 6M samples and yields
 * the receiver's strict 19 overlapping tiles.  Conversion goes directly from
 * the DMA window through a 1440-byte sender scratch into UDP, so no 24 MB
 * session cache or 2.36 MB RX1 window buffer is required.
 *
 * The request must be the formal 60 MSPS/56 MHz/6M contract.  Vendor/bridge
 * capture failures are returned unchanged; sender failures use RA8P1_IQSC_E*
 * above.  The protocol assigns consecutive sample indices across one-shot DMA
 * calls; that indexing is not proof of gap-free RF time.  Continuity still
 * requires hardware validation and is not claimed by the host mock tests.
 */
int32_t ra8p1_iqsc_capture_send_four_centers(
    ra8p1_iqsc_sender_t *sender,
    const ra8p1_sdr_adapter_api_t *sdr_api,
    void *sdr_context,
    const ra8p1_sdr_capture_request_t *request_template,
    void *dma_staging,
    size_t dma_staging_bytes,
    uint32_t first_session_id,
    ra8p1_sdr_scan_result_t *result);

typedef char ra8p1_iqsc_data_bytes_are_whole_samples[
    ((RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM %
      RA8P1_IQSC_IQ_BYTES_PER_SAMPLE) == 0U) ? 1 : -1];
typedef char ra8p1_iqsc_formal_packet_count_is_16667[
    (RA8P1_IQSC_DATA_DATAGRAMS_PER_SESSION == 16667U) ? 1 : -1];

#endif /* RA8P1_ZYNQ_IQSC_SENDER_H */
