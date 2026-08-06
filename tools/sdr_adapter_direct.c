/*
 * Direct shim for the sdr_api.md interface.  Build this file only when the
 * real vendor implementation and its sdr_api.h are available.
 *
 * Required define:
 *   RA8P1_SDR_ADAPTER_DIRECT
 *
 * The vendor header defaults to "sdr_api.h".  Define
 * RA8P1_SDR_VENDOR_HEADER only when the real project uses another name.
 */

#include "sdr_adapter.h"

#ifdef RA8P1_SDR_ADAPTER_DIRECT

#ifndef RA8P1_SDR_VENDOR_HEADER
#define RA8P1_SDR_VENDOR_HEADER "sdr_api.h"
#endif

#include RA8P1_SDR_VENDOR_HEADER

#include <stddef.h>
#include <string.h>

#include "../shared/analysis_contract.h"

#ifdef _WIN32
#define RA8P1_SDR_ADAPTER_EXPORT __declspec(dllexport)
#else
#define RA8P1_SDR_ADAPTER_EXPORT __attribute__((visibility("default")))
#endif

typedef char ra8p1_vendor_iq2_size_must_be_8[
    (sizeof(sdr_iq2_sample_t) == sizeof(ra8p1_sdr_iq2_sample_t)) ? 1 : -1];
typedef char ra8p1_vendor_iq2_i1_offset_must_be_0[
    (offsetof(sdr_iq2_sample_t, rx1_i) == 0U) ? 1 : -1];
typedef char ra8p1_vendor_iq2_q1_offset_must_be_2[
    (offsetof(sdr_iq2_sample_t, rx1_q) == 2U) ? 1 : -1];
typedef char ra8p1_vendor_iq2_i2_offset_must_be_4[
    (offsetof(sdr_iq2_sample_t, rx2_i) == 4U) ? 1 : -1];
typedef char ra8p1_vendor_iq2_q2_offset_must_be_6[
    (offsetof(sdr_iq2_sample_t, rx2_q) == 6U) ? 1 : -1];

typedef struct st_ra8p1_sdr_direct_context
{
    sdr_handle_t device;
    uint32_t initialized;
} ra8p1_sdr_direct_context_t;

static ra8p1_sdr_direct_context_t g_direct_context;

static int32_t direct_open(void **context, const ra8p1_sdr_adapter_config_t *config)
{
    sdr_config_t vendor_config;
    sdr_status_t status;

    if ((context == NULL) || (config == NULL) ||
        (config->struct_size < sizeof(*config)) ||
        (config->abi_version != RA8P1_SDR_ADAPTER_ABI_VERSION) ||
        (config->sample_rate_hz != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (config->bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ) ||
        (config->rx_channels != RA8P1_SDR_ADAPTER_RX_CHANNELS) ||
        (g_direct_context.initialized != 0U))
    {
        return -1;
    }
    memset(&g_direct_context, 0, sizeof(g_direct_context));
    sdr_default_config(&vendor_config);
    vendor_config.rx_lo_hz = config->initial_center_frequency_hz;
    vendor_config.sample_rate_hz = config->sample_rate_hz;
    vendor_config.rx_bw_hz = config->bandwidth_hz;
    vendor_config.rx_channels = (uint8_t) config->rx_channels;
    status = sdr_init(&g_direct_context.device, &vendor_config);
    if (status != SDR_OK)
    {
        memset(&g_direct_context, 0, sizeof(g_direct_context));
        return (int32_t) status;
    }
    g_direct_context.initialized = 1U;
    *context = &g_direct_context;
    return 0;
}

static int32_t direct_set_rx(void *context, uint64_t lo_hz,
                             uint32_t sample_rate_hz, uint32_t bandwidth_hz)
{
    ra8p1_sdr_direct_context_t *direct =
        (ra8p1_sdr_direct_context_t *) context;
    if ((direct != &g_direct_context) || (direct->initialized == 0U) ||
        (sample_rate_hz != RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ) ||
        (bandwidth_hz != RA8P1_ANALYSIS_BANDWIDTH_HZ))
    {
        return -1;
    }
    return (int32_t) sdr_set_rx(&direct->device, lo_hz, sample_rate_hz,
                                bandwidth_hz);
}

static int32_t direct_rx_capture(void *context, void *buffer,
                                 uint32_t sample_count, uint32_t timeout_ms)
{
    ra8p1_sdr_direct_context_t *direct =
        (ra8p1_sdr_direct_context_t *) context;
    if ((direct != &g_direct_context) || (direct->initialized == 0U) ||
        (buffer == NULL) || (sample_count == 0U) || (timeout_ms == 0U))
    {
        return -1;
    }
    return (int32_t) sdr_rx_capture(&direct->device,
                                    (sdr_iq2_sample_t *) buffer,
                                    sample_count, timeout_ms);
}

static int32_t direct_close(void *context)
{
    ra8p1_sdr_direct_context_t *direct =
        (ra8p1_sdr_direct_context_t *) context;
    sdr_status_t status;
    if ((direct != &g_direct_context) || (direct->initialized == 0U))
    {
        return -1;
    }
    status = sdr_deinit(&direct->device);
    memset(direct, 0, sizeof(*direct));
    return (int32_t) status;
}

RA8P1_SDR_ADAPTER_EXPORT int32_t ra8p1_sdr_adapter_get_api_v1(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api)
{
    if ((requested_abi != RA8P1_SDR_ADAPTER_ABI_VERSION) || (api == NULL) ||
        (api->struct_size < sizeof(*api)))
    {
        return -1;
    }
    memset(api, 0, sizeof(*api));
    api->struct_size = (uint32_t) sizeof(*api);
    api->abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    api->capture_format = RA8P1_SDR_CAPTURE_NORMALIZED_IQ2;
    api->sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api->name = "7020 AD936X direct sdr_api adapter";
    api->open = direct_open;
    api->set_rx = direct_set_rx;
    api->rx_capture = direct_rx_capture;
    api->close = direct_close;
    return 0;
}

#else

/* Keep standalone source scans/builds valid when the vendor API is absent. */
const int ra8p1_sdr_adapter_direct_disabled = 0;

#endif /* RA8P1_SDR_ADAPTER_DIRECT */
