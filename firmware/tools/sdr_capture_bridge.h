#ifndef RA8P1_SDR_CAPTURE_BRIDGE_H
#define RA8P1_SDR_CAPTURE_BRIDGE_H

/*
 * OS-independent capture bridge.  It can be included by the host sender or by
 * a Zynq PS bare-metal application that supplies its own lwIP/UDP transport.
 * No allocation is performed here: the caller owns both the DMA staging area
 * and the complete RX1 cache.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdr_adapter.h"

#define RA8P1_SDR_BRIDGE_EINVAL   (-1001)
#define RA8P1_SDR_BRIDGE_ECAPACITY (-1002)
#define RA8P1_SDR_BRIDGE_EFORMAT  (-1003)

/*
 * sdr_rx_capture()/AXI-DMAC owns and writes the complete requested range
 * before returning success.  Clearing a 6M-sample 2R2T staging buffer would
 * add an unnecessary 48 MB write for every center.  Define this to 1 only
 * while diagnosing a vendor implementation that may return short/unwritten
 * DMA data; it is deliberately disabled on the production fast path.
 */
#ifndef RA8P1_SDR_BRIDGE_DIAGNOSTIC_CLEAR_STAGING
#define RA8P1_SDR_BRIDGE_DIAGNOSTIC_CLEAR_STAGING (0)
#endif

typedef struct st_ra8p1_sdr_capture_request
{
    uint64_t center_frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t total_samples;
    uint32_t chunk_samples;
    uint32_t timeout_ms;
} ra8p1_sdr_capture_request_t;

static inline uint16_t ra8p1_sdr_bridge_get_le16(const uint8_t *buffer)
{
    return (uint16_t) ((uint16_t) buffer[0] | ((uint16_t) buffer[1] << 8U));
}

static inline void ra8p1_sdr_bridge_put_le16(uint8_t *buffer, int16_t value)
{
    uint16_t bits = (uint16_t) value;
    buffer[0] = (uint8_t) bits;
    buffer[1] = (uint8_t) (bits >> 8U);
}

static inline int32_t ra8p1_sdr_convert_rx1_chunk(
    const ra8p1_sdr_adapter_api_t *api,
    const void *dma_staging,
    uint32_t sample_count,
    uint8_t *rx1_iq_le,
    size_t rx1_iq_bytes)
{
    uint32_t n;
    if ((api == NULL) || (dma_staging == NULL) || (rx1_iq_le == NULL) ||
        (sample_count == 0U) ||
        (rx1_iq_bytes < (size_t) sample_count * 4U))
    {
        return RA8P1_SDR_BRIDGE_EINVAL;
    }
    if (api->capture_format == RA8P1_SDR_CAPTURE_RAW_2R2T_LE)
    {
        const uint8_t *raw = (const uint8_t *) dma_staging;
        for (n = 0U; n < sample_count; n++)
        {
            const uint8_t *record = raw +
                (size_t) n * RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
            uint8_t *rx1 = rx1_iq_le + (size_t) n * 4U;
            const int16_t q1 =
                (int16_t) ra8p1_sdr_bridge_get_le16(record + 0U);
            const int16_t i1 =
                (int16_t) ra8p1_sdr_bridge_get_le16(record + 2U);

            /* record +4/+6 contain RX2 Q/I and are intentionally omitted. */
            ra8p1_sdr_bridge_put_le16(rx1 + 0U, i1);
            ra8p1_sdr_bridge_put_le16(rx1 + 2U, q1);
        }
        return 0;
    }
    if (api->capture_format == RA8P1_SDR_CAPTURE_NORMALIZED_IQ2)
    {
        const ra8p1_sdr_iq2_sample_t *samples =
            (const ra8p1_sdr_iq2_sample_t *) dma_staging;
        for (n = 0U; n < sample_count; n++)
        {
            uint8_t *rx1 = rx1_iq_le + (size_t) n * 4U;
            ra8p1_sdr_bridge_put_le16(rx1 + 0U, samples[n].rx1_i);
            ra8p1_sdr_bridge_put_le16(rx1 + 2U, samples[n].rx1_q);
        }
        return 0;
    }
    return RA8P1_SDR_BRIDGE_EFORMAT;
}

static inline int32_t ra8p1_sdr_capture_rx1_cached(
    const ra8p1_sdr_adapter_api_t *api,
    void *context,
    const ra8p1_sdr_capture_request_t *request,
    void *dma_staging,
    size_t dma_staging_bytes,
    uint8_t *rx1_iq_le,
    size_t rx1_iq_bytes,
    uint32_t *captured_samples)
{
    uint32_t captured = 0U;
    int32_t status;

    if (captured_samples != NULL)
    {
        *captured_samples = 0U;
    }
    if ((api == NULL) || (context == NULL) || (request == NULL) ||
        (dma_staging == NULL) || (rx1_iq_le == NULL) ||
        (api->set_rx == NULL) || (api->rx_capture == NULL) ||
        (request->total_samples == 0U) || (request->chunk_samples == 0U) ||
        (request->timeout_ms == 0U))
    {
        return RA8P1_SDR_BRIDGE_EINVAL;
    }
    if ((api->sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        (dma_staging_bytes <
         (size_t) request->chunk_samples * RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        (rx1_iq_bytes < (size_t) request->total_samples * 4U))
    {
        return RA8P1_SDR_BRIDGE_ECAPACITY;
    }
    if ((api->capture_format != RA8P1_SDR_CAPTURE_RAW_2R2T_LE) &&
        (api->capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2))
    {
        return RA8P1_SDR_BRIDGE_EFORMAT;
    }

    status = api->set_rx(context, request->center_frequency_hz,
                         request->sample_rate_hz, request->bandwidth_hz);
    if (status != 0)
    {
        return status;
    }
    while (captured < request->total_samples)
    {
        uint32_t count = request->total_samples - captured;
        if (count > request->chunk_samples)
        {
            count = request->chunk_samples;
        }
#if RA8P1_SDR_BRIDGE_DIAGNOSTIC_CLEAR_STAGING
        memset(dma_staging, 0,
               (size_t) count * RA8P1_SDR_ADAPTER_SAMPLE_BYTES);
#endif
        status = api->rx_capture(context, dma_staging, count, request->timeout_ms);
        if (status != 0)
        {
            if (captured_samples != NULL)
            {
                *captured_samples = captured;
            }
            return status;
        }
        status = ra8p1_sdr_convert_rx1_chunk(
            api, dma_staging, count,
            rx1_iq_le + (size_t) captured * 4U,
            rx1_iq_bytes - (size_t) captured * 4U);
        if (status != 0)
        {
            if (captured_samples != NULL)
            {
                *captured_samples = captured;
            }
            return status;
        }
        captured += count;
    }
    if (captured_samples != NULL)
    {
        *captured_samples = captured;
    }
    return 0;
}

#endif /* RA8P1_SDR_CAPTURE_BRIDGE_H */
