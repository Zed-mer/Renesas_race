#ifndef RA8P1_SDR_SCAN_BRIDGE_H
#define RA8P1_SDR_SCAN_BRIDGE_H

/* Fixed four-center capture cycle for Zynq PS/bare-metal integration. */

#include <stddef.h>
#include <stdint.h>

#include "../shared/analysis_contract.h"
#include "sdr_capture_bridge.h"

#define RA8P1_SDR_SCAN_CENTER_COUNT (4U)

typedef int32_t (*ra8p1_sdr_session_ready_fn)(
    void *callback_context,
    uint32_t session_id,
    uint32_t center_index,
    uint64_t center_frequency_hz,
    const uint8_t *rx1_iq_le,
    uint32_t sample_count);

typedef struct st_ra8p1_sdr_scan_result
{
    uint32_t centers_completed;
    uint32_t failed_center_index;
    uint32_t captured_samples;
    int32_t status;
} ra8p1_sdr_scan_result_t;

static inline uint64_t ra8p1_sdr_scan_center_hz(uint32_t center_index)
{
    static const uint64_t centers[RA8P1_SDR_SCAN_CENTER_COUNT] =
    {
        2420000000ULL,
        2464000000ULL,
        5760000000ULL,
        5816000000ULL
    };
    return center_index < RA8P1_SDR_SCAN_CENTER_COUNT ? centers[center_index] : 0ULL;
}

static inline int32_t ra8p1_sdr_capture_four_center_cycle(
    const ra8p1_sdr_adapter_api_t *api,
    void *adapter_context,
    const ra8p1_sdr_capture_request_t *request_template,
    void *dma_staging,
    size_t dma_staging_bytes,
    uint8_t *rx1_iq_le,
    size_t rx1_iq_bytes,
    uint32_t first_session_id,
    ra8p1_sdr_session_ready_fn session_ready,
    void *callback_context,
    ra8p1_sdr_scan_result_t *result)
{
    ra8p1_sdr_scan_result_t local_result;
    uint32_t center_index;

    local_result.centers_completed = 0U;
    local_result.failed_center_index = UINT32_MAX;
    local_result.captured_samples = 0U;
    local_result.status = 0;
    if ((api == NULL) || (adapter_context == NULL) || (request_template == NULL) ||
        (dma_staging == NULL) || (rx1_iq_le == NULL) || (session_ready == NULL) ||
        (first_session_id == 0U) ||
        (first_session_id > (UINT32_MAX - (RA8P1_SDR_SCAN_CENTER_COUNT - 1U))) ||
        (request_template->sample_rate_hz != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (request_template->bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ) ||
        (request_template->total_samples != (uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES))
    {
        local_result.status = RA8P1_SDR_BRIDGE_EINVAL;
        if (result != NULL)
        {
            *result = local_result;
        }
        return local_result.status;
    }
    for (center_index = 0U; center_index < RA8P1_SDR_SCAN_CENTER_COUNT; center_index++)
    {
        ra8p1_sdr_capture_request_t request = *request_template;
        uint32_t session_id = first_session_id + center_index;
        int32_t status;

        if (session_id == 0U)
        {
            local_result.failed_center_index = center_index;
            local_result.status = RA8P1_SDR_BRIDGE_EINVAL;
            break;
        }
        request.center_frequency_hz = ra8p1_sdr_scan_center_hz(center_index);
        local_result.captured_samples = 0U;
        status = ra8p1_sdr_capture_rx1_cached(
            api, adapter_context, &request, dma_staging, dma_staging_bytes,
            rx1_iq_le, rx1_iq_bytes, &local_result.captured_samples);
        if (status != 0)
        {
            local_result.failed_center_index = center_index;
            local_result.status = status;
            break;
        }
        status = session_ready(callback_context, session_id, center_index,
                               request.center_frequency_hz, rx1_iq_le,
                               local_result.captured_samples);
        if (status != 0)
        {
            local_result.failed_center_index = center_index;
            local_result.status = status;
            break;
        }
        local_result.centers_completed++;
    }
    if (result != NULL)
    {
        *result = local_result;
    }
    return local_result.status;
}

#endif /* RA8P1_SDR_SCAN_BRIDGE_H */
