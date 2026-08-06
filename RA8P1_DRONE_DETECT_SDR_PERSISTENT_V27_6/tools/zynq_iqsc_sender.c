#include "zynq_iqsc_sender.h"

#include <limits.h>
#include <string.h>

#define RA8P1_IQSC_SENDER_MAGIC (0x49515332UL) /* IQS2 */

static void iqsc_put_u16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t) value;
    buffer[offset + 1U] = (uint8_t) (value >> 8U);
}

static void iqsc_put_u32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
    buffer[offset] = (uint8_t) value;
    buffer[offset + 1U] = (uint8_t) (value >> 8U);
    buffer[offset + 2U] = (uint8_t) (value >> 16U);
    buffer[offset + 3U] = (uint8_t) (value >> 24U);
}

static void iqsc_put_u64(uint8_t *buffer, uint32_t offset, uint64_t value)
{
    iqsc_put_u32(buffer, offset, (uint32_t) value);
    iqsc_put_u32(buffer, offset + 4U, (uint32_t) (value >> 32U));
}

static void iqsc_make_header(ra8p1_iqsc_sender_t *sender,
                             uint32_t sequence,
                             uint32_t data_length,
                             uint32_t flags,
                             uint64_t sample_index,
                             uint32_t session_id)
{
    iqsc_put_u32(sender->header, 0U, RA8P1_IQ_PACKET_MAGIC);
    iqsc_put_u32(sender->header, 4U, sequence);
    iqsc_put_u32(sender->header, 8U, data_length);
    iqsc_put_u32(sender->header, 12U, flags);
    iqsc_put_u64(sender->header, 16U, sample_index);
    iqsc_put_u32(sender->header, 24U, session_id);
    iqsc_put_u32(sender->header, 28U, RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED);
}

static void iqsc_make_config(ra8p1_iqsc_sender_t *sender,
                             uint32_t session_id,
                             uint32_t center_index)
{
    const uint32_t flags = RA8P1_IQ_FLAG_VALID_BITS_12;
    const uint64_t center_hz = ra8p1_sdr_scan_center_hz(center_index);

    memset(sender->config, 0, sizeof(sender->config));
    iqsc_put_u32(sender->config, 0U, RA8P1_IQ_STREAM_CONFIG_MAGIC);
    iqsc_put_u16(sender->config, 4U, RA8P1_IQ_STREAM_CONFIG_VERSION);
    iqsc_put_u16(sender->config, 6U,
                 (uint16_t) sizeof(ra8p1_iq_stream_config_t));
    iqsc_put_u32(sender->config, 8U, session_id);
    iqsc_put_u32(sender->config, 12U,
                 RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ);
    iqsc_put_u32(sender->config, 16U, RA8P1_ANALYSIS_SAMPLE_RATE_HZ);
    iqsc_put_u64(sender->config, 20U, center_hz);
    iqsc_put_u32(sender->config, 28U, RA8P1_ANALYSIS_BANDWIDTH_HZ);
    iqsc_put_u32(sender->config, 32U, RA8P1_ANALYSIS_TILE_SAMPLES);
    iqsc_put_u64(sender->config, 36U, RA8P1_ANALYSIS_TOTAL_SAMPLES);
    iqsc_put_u32(sender->config, 44U,
                 RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES);
    iqsc_put_u32(sender->config, 48U,
                 RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED);
    iqsc_put_u32(sender->config, 52U, 12U);
    iqsc_put_u32(sender->config, 56U, RA8P1_RF_CHANNEL_A_MASK);
    iqsc_put_u32(sender->config, 60U, flags);
    iqsc_put_u32(sender->config, 64U, center_index);
}

static int32_t iqsc_get_time(ra8p1_iqsc_sender_t *sender, uint64_t *time_us)
{
    int32_t callback_status;
    callback_status = sender->transport.time_us(
        sender->transport.callback_context, time_us);
    if (callback_status != 0)
    {
        sender->stats.last_transport_status = callback_status;
        return RA8P1_IQSC_ETIME;
    }
    return RA8P1_IQSC_OK;
}

static int32_t iqsc_send_datagram(ra8p1_iqsc_sender_t *sender,
                                  const uint8_t *payload,
                                  uint16_t payload_bytes,
                                  uint32_t is_control)
{
    int32_t callback_status = sender->transport.udp_send(
        sender->transport.callback_context,
        sender->transport.destination_ipv4,
        (uint16_t) RA8P1_IQSC_UDP_PORT,
        sender->header,
        (uint16_t) RA8P1_IQ_PACKET_HEADER_SIZE,
        payload,
        payload_bytes);
    if (callback_status != 0)
    {
        sender->stats.last_transport_status = callback_status;
        return RA8P1_IQSC_ETRANSPORT;
    }
    sender->stats.datagrams_sent++;
    sender->stats.udp_payload_bytes_sent +=
        RA8P1_IQ_PACKET_HEADER_SIZE + payload_bytes;
    if (is_control != 0U)
    {
        sender->stats.control_datagrams_sent++;
    }
    else
    {
        sender->stats.data_datagrams_sent++;
        sender->stats.iq_payload_bytes_sent += payload_bytes;
    }
    return RA8P1_IQSC_OK;
}

static int32_t iqsc_pace(ra8p1_iqsc_sender_t *sender,
                         uint64_t started_us,
                         uint64_t payload_bytes)
{
    uint64_t now_us;
    uint64_t target_elapsed_us;
    uint64_t elapsed_us;
    uint64_t delay_us;
    int32_t status;

    if (sender->transport.target_payload_mbps == 0U)
    {
        return RA8P1_IQSC_OK;
    }
    /* 1 Mbps is exactly 1 bit/us.  Round up to avoid exceeding the target. */
    target_elapsed_us =
        ((payload_bytes * 8ULL) + sender->transport.target_payload_mbps - 1ULL) /
        sender->transport.target_payload_mbps;
    status = iqsc_get_time(sender, &now_us);
    if (status != RA8P1_IQSC_OK)
    {
        return status;
    }
    if (now_us < started_us)
    {
        return RA8P1_IQSC_ETIME;
    }
    elapsed_us = now_us - started_us;
    if (elapsed_us >= target_elapsed_us)
    {
        return RA8P1_IQSC_OK;
    }
    delay_us = target_elapsed_us - elapsed_us;
    if (delay_us > UINT32_MAX)
    {
        return RA8P1_IQSC_EPACING;
    }
    status = sender->transport.delay_us(
        sender->transport.callback_context, (uint32_t) delay_us);
    if (status != 0)
    {
        sender->stats.last_transport_status = status;
        return RA8P1_IQSC_EPACING;
    }
    sender->stats.pacing_delay_calls++;
    sender->stats.pacing_delay_us_requested += delay_us;
    return RA8P1_IQSC_OK;
}

static void iqsc_clear_active(ra8p1_iqsc_sender_t *sender)
{
    sender->active = 0U;
    sender->active_session_id = 0U;
    sender->active_center_index = 0U;
    sender->next_sequence = 0U;
    sender->next_sample_index = 0ULL;
    sender->session_started_us = 0ULL;
    sender->session_payload_bytes = 0ULL;
}

static int32_t iqsc_finish_session(ra8p1_iqsc_sender_t *sender,
                                   int32_t status)
{
    uint64_t finished_us;
    uint64_t started_us = sender->session_started_us;
    uint64_t payload_bytes = sender->session_payload_bytes;

    sender->stats.last_status = status;
    if (status != RA8P1_IQSC_OK)
    {
        iqsc_clear_active(sender);
        sender->stats.sessions_failed++;
        return status;
    }
    status = iqsc_get_time(sender, &finished_us);
    if ((status != RA8P1_IQSC_OK) || (finished_us < started_us))
    {
        sender->stats.last_status = RA8P1_IQSC_ETIME;
        iqsc_clear_active(sender);
        sender->stats.sessions_failed++;
        return RA8P1_IQSC_ETIME;
    }
    sender->stats.last_session_elapsed_us = finished_us - started_us;
    if (sender->stats.last_session_elapsed_us != 0ULL)
    {
        uint64_t rate_x1000 =
            (payload_bytes * 8000ULL) /
            sender->stats.last_session_elapsed_us;
        sender->stats.last_payload_mbps_x1000 =
            rate_x1000 > UINT32_MAX ? UINT32_MAX : (uint32_t) rate_x1000;
    }
    else
    {
        sender->stats.last_payload_mbps_x1000 = 0U;
    }
    sender->stats.sessions_completed++;
    sender->stats.last_status = RA8P1_IQSC_OK;
    iqsc_clear_active(sender);
    return RA8P1_IQSC_OK;
}

int32_t ra8p1_iqsc_sender_init(ra8p1_iqsc_sender_t *sender,
                               const ra8p1_iqsc_transport_t *transport)
{
    if ((sender == NULL) || (transport == NULL) ||
        (transport->udp_send == NULL) || (transport->delay_us == NULL) ||
        (transport->time_us == NULL))
    {
        return RA8P1_IQSC_EINVAL;
    }
    memset(sender, 0, sizeof(*sender));
    sender->transport = *transport;
    sender->initialized = RA8P1_IQSC_SENDER_MAGIC;
    return RA8P1_IQSC_OK;
}

void ra8p1_iqsc_sender_reset_stats(ra8p1_iqsc_sender_t *sender)
{
    if ((sender != NULL) &&
        (sender->initialized == RA8P1_IQSC_SENDER_MAGIC) &&
        (sender->active == 0U))
    {
        memset(&sender->stats, 0, sizeof(sender->stats));
    }
}

void ra8p1_iqsc_session_abort(ra8p1_iqsc_sender_t *sender, int32_t reason)
{
    if ((sender != NULL) &&
        (sender->initialized == RA8P1_IQSC_SENDER_MAGIC) &&
        (sender->active != 0U))
    {
        sender->stats.last_status = reason;
        sender->stats.sessions_failed++;
        iqsc_clear_active(sender);
    }
}

int32_t ra8p1_iqsc_session_begin(ra8p1_iqsc_sender_t *sender,
                                 uint32_t session_id,
                                 uint32_t center_index)
{
    int32_t status;

    if ((sender == NULL) ||
        (sender->initialized != RA8P1_IQSC_SENDER_MAGIC) ||
        (sender->active != 0U))
    {
        return RA8P1_IQSC_ESTATE;
    }
    if ((session_id == 0U) ||
        (center_index >= RA8P1_SDR_SCAN_CENTER_COUNT))
    {
        sender->stats.last_status = RA8P1_IQSC_EINVAL;
        return RA8P1_IQSC_EINVAL;
    }

    sender->active = 1U;
    sender->stats.sessions_started++;
    sender->stats.last_session_id = session_id;
    sender->stats.last_center_index = center_index;
    sender->stats.last_sequence = 0U;
    sender->stats.last_sample_index = 0ULL;
    sender->stats.last_transport_status = 0;
    sender->active_session_id = session_id;
    sender->active_center_index = center_index;
    sender->next_sequence = 0U;
    sender->next_sample_index = 0ULL;
    sender->session_payload_bytes = 0ULL;
    iqsc_make_config(sender, session_id, center_index);
    iqsc_make_header(sender, sender->next_sequence,
                     (uint32_t) sizeof(ra8p1_iq_stream_config_t),
                     RA8P1_IQ_FLAG_VALID_BITS_12 |
                     RA8P1_IQ_FLAG_STREAM_START,
                     0ULL, session_id);
    status = iqsc_send_datagram(
        sender, sender->config,
        (uint16_t) sizeof(ra8p1_iq_stream_config_t), 1U);
    if (status != RA8P1_IQSC_OK)
    {
        return iqsc_finish_session(sender, status);
    }
    sender->next_sequence++;
    status = iqsc_get_time(sender, &sender->session_started_us);
    if (status != RA8P1_IQSC_OK)
    {
        return iqsc_finish_session(sender, status);
    }
    return RA8P1_IQSC_OK;
}

int32_t ra8p1_iqsc_session_write(ra8p1_iqsc_sender_t *sender,
                                 const uint8_t *rx1_iq_le,
                                 uint32_t sample_count)
{
    uint32_t consumed = 0U;
    int32_t status;

    if ((sender == NULL) ||
        (sender->initialized != RA8P1_IQSC_SENDER_MAGIC) ||
        (sender->active == 0U))
    {
        return RA8P1_IQSC_ESTATE;
    }
    if ((rx1_iq_le == NULL) || (sample_count == 0U) ||
        ((uint64_t) sample_count >
         (RA8P1_ANALYSIS_TOTAL_SAMPLES - sender->next_sample_index)))
    {
        sender->stats.last_status = RA8P1_IQSC_ECOUNT;
        return RA8P1_IQSC_ECOUNT;
    }
    while (consumed < sample_count)
    {
        uint32_t samples_left = sample_count - consumed;
        uint32_t samples = samples_left > RA8P1_IQSC_SAMPLES_PER_DATAGRAM ?
                           RA8P1_IQSC_SAMPLES_PER_DATAGRAM : samples_left;
        uint16_t data_bytes =
            (uint16_t) (samples * RA8P1_IQSC_IQ_BYTES_PER_SAMPLE);
        const uint8_t *payload =
            rx1_iq_le + (size_t) consumed *
                        RA8P1_IQSC_IQ_BYTES_PER_SAMPLE;

        iqsc_make_header(sender, sender->next_sequence, data_bytes,
                         RA8P1_IQ_FLAG_VALID_BITS_12,
                         sender->next_sample_index,
                         sender->active_session_id);
        status = iqsc_send_datagram(sender, payload, data_bytes, 0U);
        if (status != RA8P1_IQSC_OK)
        {
            sender->stats.last_sequence = sender->next_sequence;
            sender->stats.last_sample_index = sender->next_sample_index;
            return iqsc_finish_session(sender, status);
        }
        sender->session_payload_bytes += data_bytes;
        sender->next_sample_index += samples;
        consumed += samples;
        sender->stats.last_sequence = sender->next_sequence;
        sender->stats.last_sample_index = sender->next_sample_index;
        sender->next_sequence++;
        status = iqsc_pace(sender, sender->session_started_us,
                           sender->session_payload_bytes);
        if (status != RA8P1_IQSC_OK)
        {
            return iqsc_finish_session(sender, status);
        }
    }
    return RA8P1_IQSC_OK;
}

int32_t ra8p1_iqsc_session_write_dma(ra8p1_iqsc_sender_t *sender,
                                     const ra8p1_sdr_adapter_api_t *sdr_api,
                                     const void *dma_staging,
                                     uint32_t sample_count)
{
    const uint8_t *dma_bytes = (const uint8_t *) dma_staging;
    uint32_t converted = 0U;
    int32_t status;

    if ((sender == NULL) ||
        (sender->initialized != RA8P1_IQSC_SENDER_MAGIC) ||
        (sender->active == 0U))
    {
        return RA8P1_IQSC_ESTATE;
    }
    if ((sdr_api == NULL) || (dma_staging == NULL) ||
        (sample_count == 0U) ||
        (sdr_api->sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        ((sdr_api->capture_format != RA8P1_SDR_CAPTURE_RAW_2R2T_LE) &&
         (sdr_api->capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2)))
    {
        return RA8P1_IQSC_EINVAL;
    }
    while (converted < sample_count)
    {
        uint32_t count = sample_count - converted;
        if (count > RA8P1_IQSC_SAMPLES_PER_DATAGRAM)
        {
            count = RA8P1_IQSC_SAMPLES_PER_DATAGRAM;
        }
        status = ra8p1_sdr_convert_rx1_chunk(
            sdr_api,
            dma_bytes + (size_t) converted *
                        RA8P1_SDR_ADAPTER_SAMPLE_BYTES,
            count,
            sender->payload_scratch.bytes,
            sizeof(sender->payload_scratch.bytes));
        if (status != 0)
        {
            ra8p1_iqsc_session_abort(sender, status);
            return status;
        }
        status = ra8p1_iqsc_session_write(
            sender, sender->payload_scratch.bytes, count);
        if (status != RA8P1_IQSC_OK)
        {
            return status;
        }
        converted += count;
    }
    return RA8P1_IQSC_OK;
}

int32_t ra8p1_iqsc_session_end(ra8p1_iqsc_sender_t *sender)
{
    int32_t status;
    if ((sender == NULL) ||
        (sender->initialized != RA8P1_IQSC_SENDER_MAGIC) ||
        (sender->active == 0U))
    {
        return RA8P1_IQSC_ESTATE;
    }
    if (sender->next_sample_index != RA8P1_ANALYSIS_TOTAL_SAMPLES)
    {
        sender->stats.last_status = RA8P1_IQSC_ECOUNT;
        return RA8P1_IQSC_ECOUNT;
    }

    iqsc_make_header(sender, sender->next_sequence,
                     (uint32_t) sizeof(ra8p1_iq_stream_config_t),
                     RA8P1_IQ_FLAG_VALID_BITS_12 |
                     RA8P1_IQ_FLAG_STREAM_END,
                     sender->next_sample_index,
                     sender->active_session_id);
    status = iqsc_send_datagram(
        sender, sender->config,
        (uint16_t) sizeof(ra8p1_iq_stream_config_t), 1U);
    sender->stats.last_sequence = sender->next_sequence;
    sender->stats.last_sample_index = sender->next_sample_index;
    return iqsc_finish_session(sender, status);
}

int32_t ra8p1_iqsc_send_cached_session(ra8p1_iqsc_sender_t *sender,
                                       uint32_t session_id,
                                       uint32_t center_index,
                                       const uint8_t *rx1_iq_le,
                                       uint32_t sample_count)
{
    int32_t status;
    if ((rx1_iq_le == NULL) ||
        (sample_count != (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES))
    {
        return RA8P1_IQSC_EINVAL;
    }
    status = ra8p1_iqsc_session_begin(sender, session_id, center_index);
    if (status == RA8P1_IQSC_OK)
    {
        status = ra8p1_iqsc_session_write(sender, rx1_iq_le, sample_count);
    }
    if (status == RA8P1_IQSC_OK)
    {
        status = ra8p1_iqsc_session_end(sender);
    }
    return status;
}

int32_t ra8p1_iqsc_capture_send_four_centers(
    ra8p1_iqsc_sender_t *sender,
    const ra8p1_sdr_adapter_api_t *sdr_api,
    void *sdr_context,
    const ra8p1_sdr_capture_request_t *request_template,
    void *dma_staging,
    size_t dma_staging_bytes,
    uint32_t first_session_id,
    ra8p1_sdr_scan_result_t *result)
{
    ra8p1_sdr_scan_result_t local_result;
    uint32_t center_index;
    int32_t status;

    local_result.centers_completed = 0U;
    local_result.failed_center_index = UINT32_MAX;
    local_result.captured_samples = 0U;
    local_result.status = RA8P1_IQSC_OK;
    if ((sender == NULL) ||
        (sender->initialized != RA8P1_IQSC_SENDER_MAGIC) ||
        (sender->active != 0U))
    {
        local_result.status = RA8P1_IQSC_ESTATE;
        if (result != NULL)
        {
            *result = local_result;
        }
        return local_result.status;
    }
    if ((sdr_api == NULL) || (sdr_context == NULL) ||
        (request_template == NULL) || (dma_staging == NULL) ||
        (first_session_id == 0U) ||
        (first_session_id >
         (UINT32_MAX - (RA8P1_SDR_SCAN_CENTER_COUNT - 1U))) ||
        (sdr_api->set_rx == NULL) || (sdr_api->rx_capture == NULL) ||
        (sdr_api->sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        ((sdr_api->capture_format != RA8P1_SDR_CAPTURE_RAW_2R2T_LE) &&
         (sdr_api->capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2)) ||
        (request_template->sample_rate_hz !=
         RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (request_template->bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ) ||
        (request_template->total_samples !=
         (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES) ||
        (request_template->chunk_samples == 0U) ||
        (request_template->chunk_samples > request_template->total_samples) ||
        (request_template->timeout_ms == 0U) ||
        (dma_staging_bytes <
         (size_t) request_template->chunk_samples *
         RA8P1_SDR_ADAPTER_SAMPLE_BYTES))
    {
        local_result.status = RA8P1_IQSC_EINVAL;
        if (result != NULL)
        {
            *result = local_result;
        }
        return local_result.status;
    }
    for (center_index = 0U;
         center_index < RA8P1_SDR_SCAN_CENTER_COUNT;
         center_index++)
    {
        uint32_t captured = 0U;
        uint32_t session_id = first_session_id + center_index;
        local_result.captured_samples = 0U;
        status = sdr_api->set_rx(
            sdr_context, ra8p1_sdr_scan_center_hz(center_index),
            request_template->sample_rate_hz,
            request_template->bandwidth_hz);
        if (status != 0)
        {
            local_result.failed_center_index = center_index;
            local_result.status = status;
            break;
        }
        status = ra8p1_iqsc_session_begin(sender, session_id, center_index);
        if (status != RA8P1_IQSC_OK)
        {
            local_result.failed_center_index = center_index;
            local_result.status = status;
            break;
        }
        while (captured < request_template->total_samples)
        {
            uint32_t count = request_template->total_samples - captured;
            if (count > request_template->chunk_samples)
            {
                count = request_template->chunk_samples;
            }
#if RA8P1_SDR_BRIDGE_DIAGNOSTIC_CLEAR_STAGING
            memset(dma_staging, 0,
                   (size_t) count * RA8P1_SDR_ADAPTER_SAMPLE_BYTES);
#endif
            status = sdr_api->rx_capture(
                sdr_context, dma_staging, count,
                request_template->timeout_ms);
            if (status != 0)
            {
                ra8p1_iqsc_session_abort(sender, status);
                local_result.failed_center_index = center_index;
                local_result.status = status;
                break;
            }
            captured += count;
            local_result.captured_samples = captured;
            sender->stats.capture_windows_completed++;
            status = ra8p1_iqsc_session_write_dma(
                sender, sdr_api, dma_staging, count);
            if (status != RA8P1_IQSC_OK)
            {
                local_result.failed_center_index = center_index;
                local_result.status = status;
                break;
            }
        }
        if (status != RA8P1_IQSC_OK)
        {
            break;
        }
        status = ra8p1_iqsc_session_end(sender);
        if (status != RA8P1_IQSC_OK)
        {
            local_result.failed_center_index = center_index;
            local_result.status = status;
            break;
        }
        sender->stats.capture_centers_completed++;
        local_result.centers_completed++;
    }
    if (result != NULL)
    {
        *result = local_result;
    }
    sender->stats.last_status = local_result.status;
    return local_result.status;
}
