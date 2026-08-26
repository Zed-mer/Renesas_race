#include "zynq_iqsc_sender.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DESTINATION_IPV4 (0xC0A81F14UL)
#define TEST_CAPTURE_CHUNK     (RA8P1_ANALYSIS_TILE_SAMPLES)
#define TEST_WINDOWS_PER_CENTER \
    (((uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES + TEST_CAPTURE_CHUNK - 1U) / \
     TEST_CAPTURE_CHUNK)
#define TEST_FULL_CAPTURE_WINDOWS \
    ((uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES / TEST_CAPTURE_CHUNK)
#define TEST_LAST_CAPTURE_SAMPLES \
    ((uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES % TEST_CAPTURE_CHUNK)
#define TEST_WINDOWED_DATA_DATAGRAMS \
    (TEST_FULL_CAPTURE_WINDOWS * \
     ((TEST_CAPTURE_CHUNK * 4U + RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM - 1U) / \
      RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM) + \
     ((TEST_LAST_CAPTURE_SAMPLES * 4U + \
       RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM - 1U) / \
      RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM))

typedef struct st_mock_capture
{
    uint32_t set_rx_calls;
    uint32_t capture_calls;
    uint32_t center_index;
    uint32_t sample_cursor;
} mock_capture_t;

typedef struct st_mock_transport
{
    uint64_t now_us;
    uint64_t calls;
    uint64_t fail_call;
    uint32_t sessions_completed;
    uint32_t in_session;
    uint32_t expected_sequence;
    uint32_t session_id;
    uint32_t center_index;
    uint64_t expected_sample_index;
    uint32_t validation_error;
    uint32_t first_data_seen;
    mock_capture_t *capture;
} mock_transport_t;

static uint16_t get_u16(const uint8_t *data)
{
    return (uint16_t) ((uint16_t) data[0] | ((uint16_t) data[1] << 8U));
}

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t) data[0] |
           ((uint32_t) data[1] << 8U) |
           ((uint32_t) data[2] << 16U) |
           ((uint32_t) data[3] << 24U);
}

static uint64_t get_u64(const uint8_t *data)
{
    return (uint64_t) get_u32(data) |
           ((uint64_t) get_u32(data + 4U) << 32U);
}

static int16_t get_s16(const uint8_t *data)
{
    return (int16_t) get_u16(data);
}

static int16_t expected_i(uint64_t sample_index)
{
    return (int16_t) (-1000 - (int32_t) (sample_index & 0x3FFULL));
}

static int16_t expected_q(uint64_t sample_index)
{
    return (int16_t) (1000 + (uint32_t) (sample_index & 0x3FFULL));
}

static void put_s16(uint8_t *data, int16_t value)
{
    uint16_t bits = (uint16_t) value;
    data[0] = (uint8_t) bits;
    data[1] = (uint8_t) (bits >> 8U);
}

static void fill_rx1(uint8_t *buffer, uint32_t sample_count,
                     uint64_t first_sample)
{
    uint32_t n;
    for (n = 0U; n < sample_count; n++)
    {
        uint16_t i_bits = (uint16_t) expected_i(first_sample + n);
        uint16_t q_bits = (uint16_t) expected_q(first_sample + n);
        buffer[(size_t) n * 4U] = (uint8_t) i_bits;
        buffer[(size_t) n * 4U + 1U] = (uint8_t) (i_bits >> 8U);
        buffer[(size_t) n * 4U + 2U] = (uint8_t) q_bits;
        buffer[(size_t) n * 4U + 3U] = (uint8_t) (q_bits >> 8U);
    }
}

static int32_t mock_udp_send(void *callback_context,
                             uint32_t destination_ipv4,
                             uint16_t destination_port,
                             const uint8_t *header,
                             uint16_t header_bytes,
                             const uint8_t *payload,
                             uint16_t payload_bytes)
{
    mock_transport_t *mock = (mock_transport_t *) callback_context;
    uint32_t sequence;
    uint32_t data_length;
    uint32_t flags;
    uint64_t sample_index;
    uint32_t session_id;

    mock->calls++;
    if ((mock->fail_call != 0ULL) && (mock->calls == mock->fail_call))
    {
        return -77;
    }
    if ((destination_ipv4 != TEST_DESTINATION_IPV4) ||
        (destination_port != RA8P1_IQSC_UDP_PORT) ||
        (header == NULL) ||
        (header_bytes != RA8P1_IQ_PACKET_HEADER_SIZE) ||
        (payload == NULL) ||
        (get_u32(header) != RA8P1_IQ_PACKET_MAGIC))
    {
        mock->validation_error = 1U;
        return -90;
    }
    sequence = get_u32(header + 4U);
    data_length = get_u32(header + 8U);
    flags = get_u32(header + 12U);
    sample_index = get_u64(header + 16U);
    session_id = get_u32(header + 24U);
    if ((data_length != payload_bytes) ||
        (get_u32(header + 28U) != RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED))
    {
        mock->validation_error = 2U;
        return -91;
    }
    if ((flags & RA8P1_IQ_FLAG_STREAM_START) != 0U)
    {
        if ((mock->in_session != 0U) || (sequence != 0U) ||
            (sample_index != 0ULL) ||
            (payload_bytes != sizeof(ra8p1_iq_stream_config_t)) ||
            (get_u32(payload) != RA8P1_IQ_STREAM_CONFIG_MAGIC) ||
            (get_u16(payload + 4U) != RA8P1_IQ_STREAM_CONFIG_VERSION) ||
            (get_u16(payload + 6U) != sizeof(ra8p1_iq_stream_config_t)) ||
            (get_u32(payload + 8U) != session_id) ||
            (get_u32(payload + 12U) != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
            (get_u64(payload + 20U) !=
             ra8p1_sdr_scan_center_hz(get_u32(payload + 64U))) ||
            (get_u32(payload + 28U) != RA8P1_ANALYSIS_BANDWIDTH_HZ) ||
            (get_u32(payload + 32U) != RA8P1_ANALYSIS_TILE_SAMPLES) ||
            (get_u64(payload + 36U) != RA8P1_ANALYSIS_TOTAL_SAMPLES) ||
            (get_u32(payload + 44U) != RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES) ||
            (get_u32(payload + 60U) != RA8P1_IQ_FLAG_VALID_BITS_12))
        {
            mock->validation_error = 3U;
            return -92;
        }
        mock->in_session = 1U;
        mock->session_id = session_id;
        mock->center_index = get_u32(payload + 64U);
        mock->expected_sequence = 1U;
        mock->expected_sample_index = 0ULL;
        mock->first_data_seen = 0U;
        return 0;
    }
    if ((mock->in_session == 0U) || (session_id != mock->session_id) ||
        (sequence != mock->expected_sequence) ||
        (sample_index != mock->expected_sample_index))
    {
        mock->validation_error = 4U;
        return -93;
    }
    if ((flags & RA8P1_IQ_FLAG_STREAM_END) != 0U)
    {
        if ((sample_index != RA8P1_ANALYSIS_TOTAL_SAMPLES) ||
            (payload_bytes != sizeof(ra8p1_iq_stream_config_t)) ||
            (get_u32(payload + 8U) != session_id) ||
            (get_u32(payload + 64U) != mock->center_index))
        {
            mock->validation_error = 5U;
            return -94;
        }
        mock->in_session = 0U;
        mock->sessions_completed++;
        return 0;
    }
    if ((flags != RA8P1_IQ_FLAG_VALID_BITS_12) ||
        (payload_bytes == 0U) || ((payload_bytes & 3U) != 0U) ||
        (payload_bytes > RA8P1_IQSC_DATA_BYTES_PER_DATAGRAM))
    {
        mock->validation_error = 6U;
        return -95;
    }
    if ((get_s16(payload) != expected_i(sample_index)) ||
        (get_s16(payload + 2U) != expected_q(sample_index)) ||
        (get_s16(payload + payload_bytes - 4U) !=
         expected_i(sample_index + payload_bytes / 4U - 1U)) ||
        (get_s16(payload + payload_bytes - 2U) !=
         expected_q(sample_index + payload_bytes / 4U - 1U)))
    {
        mock->validation_error = 7U;
        return -96;
    }
    if (mock->capture != NULL)
    {
        uint32_t expected_capture_calls =
            mock->center_index * TEST_WINDOWS_PER_CENTER +
            (uint32_t) (sample_index / TEST_CAPTURE_CHUNK) + 1U;
        if (mock->capture->capture_calls != expected_capture_calls)
        {
            /* Proves a window is sent before the next DMA capture starts. */
            mock->validation_error = 8U;
            return -97;
        }
    }
    mock->first_data_seen = 1U;
    mock->expected_sample_index += payload_bytes / 4U;
    mock->expected_sequence++;
    return 0;
}

static int32_t mock_delay_us(void *callback_context, uint32_t delay_us)
{
    mock_transport_t *mock = (mock_transport_t *) callback_context;
    mock->now_us += delay_us;
    return 0;
}

static int32_t mock_time_us(void *callback_context, uint64_t *time_us)
{
    mock_transport_t *mock = (mock_transport_t *) callback_context;
    if (time_us == NULL)
    {
        return -1;
    }
    *time_us = mock->now_us;
    return 0;
}

static void init_transport(ra8p1_iqsc_transport_t *transport,
                           mock_transport_t *mock,
                           uint32_t target_mbps)
{
    memset(transport, 0, sizeof(*transport));
    transport->destination_ipv4 = TEST_DESTINATION_IPV4;
    transport->target_payload_mbps = target_mbps;
    transport->callback_context = mock;
    transport->udp_send = mock_udp_send;
    transport->delay_us = mock_delay_us;
    transport->time_us = mock_time_us;
}

static int32_t mock_set_rx(void *context, uint64_t lo_hz,
                           uint32_t sample_rate_hz, uint32_t bandwidth_hz)
{
    mock_capture_t *mock = (mock_capture_t *) context;
    if ((mock == NULL) ||
        (lo_hz != ra8p1_sdr_scan_center_hz(mock->set_rx_calls)) ||
        (sample_rate_hz != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ))
    {
        return -40;
    }
    mock->center_index = mock->set_rx_calls;
    mock->sample_cursor = 0U;
    mock->set_rx_calls++;
    return 0;
}

static int32_t mock_rx_capture(void *context, void *buffer,
                               uint32_t sample_count, uint32_t timeout_ms)
{
    mock_capture_t *mock = (mock_capture_t *) context;
    uint8_t *raw = (uint8_t *) buffer;
    uint32_t n;
    if ((mock == NULL) || (buffer == NULL) || (sample_count == 0U) ||
        (timeout_ms == 0U))
    {
        return -41;
    }
    for (n = 0U; n < sample_count; n++)
    {
        uint64_t sample = (uint64_t) mock->sample_cursor + n;
        uint8_t *record = raw + (size_t) n *
                                RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
        /* Documented AXI ADC DMA byte order: Q1,I1,Q2,I2. */
        put_s16(record, expected_q(sample));
        put_s16(record + 2U, expected_i(sample));
        put_s16(record + 4U, (int16_t) 2000);
        put_s16(record + 6U, (int16_t) -2000);
    }
    mock->sample_cursor += sample_count;
    mock->capture_calls++;
    return 0;
}

static int test_windowed_session(uint8_t *full_cache)
{
    ra8p1_iqsc_transport_t transport;
    ra8p1_iqsc_sender_t sender;
    mock_transport_t mock;
    uint32_t sent = 0U;
    int32_t status;

    memset(&mock, 0, sizeof(mock));
    mock.now_us = 1000ULL;
    init_transport(&transport, &mock, 800U);
    if (ra8p1_iqsc_sender_init(&sender, &transport) != RA8P1_IQSC_OK)
    {
        return 10;
    }
    status = ra8p1_iqsc_session_begin(&sender, 10U, 3U);
    if (status != RA8P1_IQSC_OK)
    {
        return 11;
    }
    status = ra8p1_iqsc_session_write(
        &sender, full_cache, TEST_CAPTURE_CHUNK);
    if ((status != RA8P1_IQSC_OK) ||
        (sender.stats.last_sample_index != TEST_CAPTURE_CHUNK) ||
        (mock.first_data_seen == 0U) ||
        (mock.sessions_completed != 0U))
    {
        return 12;
    }
    if ((ra8p1_iqsc_session_end(&sender) != RA8P1_IQSC_ECOUNT) ||
        (sender.active == 0U))
    {
        return 15;
    }
    sent = TEST_CAPTURE_CHUNK;
    while (sent < (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES)
    {
        uint32_t count =
            (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES - sent;
        if (count > TEST_CAPTURE_CHUNK)
        {
            count = TEST_CAPTURE_CHUNK;
        }
        status = ra8p1_iqsc_session_write(
            &sender, full_cache + (size_t) sent * 4U, count);
        if (status != RA8P1_IQSC_OK)
        {
            return 13;
        }
        sent += count;
    }
    status = ra8p1_iqsc_session_end(&sender);
    if ((status != RA8P1_IQSC_OK) || (mock.validation_error != 0U) ||
        (mock.sessions_completed != 1U) ||
        (sender.stats.sessions_completed != 1ULL) ||
        (sender.stats.data_datagrams_sent != TEST_WINDOWED_DATA_DATAGRAMS) ||
        (sender.stats.control_datagrams_sent != 2ULL) ||
        (sender.stats.iq_payload_bytes_sent != 24000000ULL) ||
        (sender.stats.last_session_elapsed_us != 240000ULL) ||
        (sender.stats.last_payload_mbps_x1000 != 800000U) ||
        (sender.stats.last_sequence != TEST_WINDOWED_DATA_DATAGRAMS + 1U) ||
        (sender.stats.last_sample_index != RA8P1_ANALYSIS_TOTAL_SAMPLES))
    {
        fprintf(stderr,
                "windowed status=%ld validation=%lu completed=%lu "
                "sessions=%llu data=%llu control=%llu iq=%llu elapsed=%llu "
                "rate=%lu sequence=%lu sample=%llu calls=%llu delay=%llu\n",
                (long) status, (unsigned long) mock.validation_error,
                (unsigned long) mock.sessions_completed,
                (unsigned long long) sender.stats.sessions_completed,
                (unsigned long long) sender.stats.data_datagrams_sent,
                (unsigned long long) sender.stats.control_datagrams_sent,
                (unsigned long long) sender.stats.iq_payload_bytes_sent,
                (unsigned long long) sender.stats.last_session_elapsed_us,
                (unsigned long) sender.stats.last_payload_mbps_x1000,
                (unsigned long) sender.stats.last_sequence,
                (unsigned long long) sender.stats.last_sample_index,
                (unsigned long long) mock.calls,
                (unsigned long long) sender.stats.pacing_delay_us_requested);
        return 14;
    }
    return 0;
}

static int test_transport_failure(const uint8_t *full_cache)
{
    ra8p1_iqsc_transport_t transport;
    ra8p1_iqsc_sender_t sender;
    mock_transport_t mock;
    int32_t status;

    memset(&mock, 0, sizeof(mock));
    mock.fail_call = 7ULL;
    init_transport(&transport, &mock, 0U);
    if (ra8p1_iqsc_sender_init(&sender, &transport) != RA8P1_IQSC_OK)
    {
        return 20;
    }
    status = ra8p1_iqsc_send_cached_session(
        &sender, 20U, 0U, full_cache,
        (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES);
    if ((status != RA8P1_IQSC_ETRANSPORT) || (sender.active != 0U) ||
        (sender.stats.sessions_failed != 1ULL) ||
        (sender.stats.last_transport_status != -77))
    {
        return 21;
    }
    return 0;
}

static int test_incremental_capture_four_centers(void)
{
    ra8p1_iqsc_transport_t transport;
    ra8p1_iqsc_sender_t sender;
    ra8p1_sdr_adapter_api_t api;
    ra8p1_sdr_capture_request_t request;
    ra8p1_sdr_scan_result_t result;
    mock_transport_t mock_transport;
    mock_capture_t mock_capture;
    void *staging;
    int32_t status;

    staging = malloc((size_t) TEST_CAPTURE_CHUNK *
                     RA8P1_SDR_ADAPTER_SAMPLE_BYTES);
    if (staging == NULL)
    {
        free(staging);
        return 30;
    }
    memset(&mock_capture, 0, sizeof(mock_capture));
    memset(&mock_transport, 0, sizeof(mock_transport));
    mock_transport.capture = &mock_capture;
    init_transport(&transport, &mock_transport, 0U);
    if (ra8p1_iqsc_sender_init(&sender, &transport) != RA8P1_IQSC_OK)
    {
        free(staging);
        return 31;
    }
    memset(&api, 0, sizeof(api));
    api.capture_format = RA8P1_SDR_CAPTURE_RAW_2R2T_LE;
    api.sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api.set_rx = mock_set_rx;
    api.rx_capture = mock_rx_capture;
    memset(&request, 0, sizeof(request));
    request.sample_rate_hz = RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ;
    request.bandwidth_hz = RA8P1_ANALYSIS_BANDWIDTH_HZ;
    request.total_samples = (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES;
    request.chunk_samples = TEST_CAPTURE_CHUNK;
    request.timeout_ms = 500U;
    status = ra8p1_iqsc_capture_send_four_centers(
        &sender, &api, &mock_capture, &request,
        staging, (size_t) TEST_CAPTURE_CHUNK *
                 RA8P1_SDR_ADAPTER_SAMPLE_BYTES,
        100U, &result);
    free(staging);
    if ((status != RA8P1_IQSC_OK) || (result.status != 0) ||
        (result.centers_completed != 4U) ||
        (result.failed_center_index != UINT32_MAX) ||
        (result.captured_samples !=
         (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES) ||
        (mock_capture.set_rx_calls != 4U) ||
        (mock_capture.capture_calls != 4U * TEST_WINDOWS_PER_CENTER) ||
        (mock_transport.sessions_completed != 4U) ||
        (mock_transport.validation_error != 0U) ||
        (sender.stats.capture_windows_completed !=
         4ULL * TEST_WINDOWS_PER_CENTER) ||
        (sender.stats.capture_centers_completed != 4ULL) ||
        (sender.stats.sessions_completed != 4ULL))
    {
        return 32;
    }
    return 0;
}

int main(void)
{
    uint8_t *full_cache =
        (uint8_t *) malloc((size_t) RA8P1_ANALYSIS_TOTAL_SAMPLES * 4U);
    int status;
    if (full_cache == NULL)
    {
        return 1;
    }
    fill_rx1(full_cache, (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES, 0ULL);
    status = test_windowed_session(full_cache);
    if (status == 0)
    {
        status = test_transport_failure(full_cache);
    }
    free(full_cache);
    if (status == 0)
    {
        status = test_incremental_capture_four_centers();
    }
    if (status != 0)
    {
        fprintf(stderr, "zynq IQSC sender test failed: %d\n", status);
        return status;
    }
    puts("zynq IQSC v2 windowed sender mock tests passed");
    return 0;
}
