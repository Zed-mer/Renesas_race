#include "sdr_adapter.h"
#include "sdr_capture_bridge.h"
#include "sdr_scan_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int32_t ra8p1_sdr_adapter_get_api_v1(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api);

static int16_t get_s16_le(const uint8_t *buffer)
{
    uint16_t bits = (uint16_t) ((uint16_t) buffer[0] |
                                ((uint16_t) buffer[1] << 8U));
    return (int16_t) bits;
}

typedef struct st_scan_callback_state
{
    uint32_t calls;
    uint32_t first_session_id;
} scan_callback_state_t;

typedef struct st_no_clear_state
{
    uint32_t saw_untouched_staging;
} no_clear_state_t;

static void put_s16_le(uint8_t *buffer, int16_t value)
{
    uint16_t bits = (uint16_t) value;
    buffer[0] = (uint8_t) bits;
    buffer[1] = (uint8_t) (bits >> 8U);
}

static int32_t no_clear_set_rx(void *context, uint64_t lo_hz,
                               uint32_t sample_rate_hz,
                               uint32_t bandwidth_hz)
{
    return ((context != NULL) && (lo_hz != 0ULL) &&
            (sample_rate_hz == 60000000U) &&
            (bandwidth_hz == 56000000U)) ? 0 : -30;
}

static int32_t no_clear_capture(void *context, void *buffer,
                                uint32_t sample_count, uint32_t timeout_ms)
{
    no_clear_state_t *state = (no_clear_state_t *) context;
    uint8_t *raw = (uint8_t *) buffer;
    uint32_t byte;
    uint32_t n;
    if ((state == NULL) || (buffer == NULL) || (sample_count != 2U) ||
        (timeout_ms == 0U))
    {
        return -31;
    }
    state->saw_untouched_staging = 1U;
    for (byte = 0U; byte < sample_count * 8U; byte++)
    {
        if (raw[byte] != 0xA5U)
        {
            state->saw_untouched_staging = 0U;
            return -32;
        }
    }
    for (n = 0U; n < sample_count; n++)
    {
        put_s16_le(raw + (size_t) n * 8U, (int16_t) (11 + n));
        put_s16_le(raw + (size_t) n * 8U + 2U, (int16_t) (-11 - (int32_t) n));
        put_s16_le(raw + (size_t) n * 8U + 4U, (int16_t) (22 + n));
        put_s16_le(raw + (size_t) n * 8U + 6U, (int16_t) (-22 - (int32_t) n));
    }
    return 0;
}

static int test_default_fast_path_does_not_clear_staging(void)
{
    ra8p1_sdr_adapter_api_t api;
    ra8p1_sdr_capture_request_t request;
    no_clear_state_t state;
    uint8_t staging[16];
    uint8_t rx1[8];
    uint32_t captured = 0U;
    int32_t status;

    memset(&api, 0, sizeof(api));
    api.capture_format = RA8P1_SDR_CAPTURE_RAW_2R2T_LE;
    api.sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    api.set_rx = no_clear_set_rx;
    api.rx_capture = no_clear_capture;
    memset(&request, 0, sizeof(request));
    request.center_frequency_hz = 2420000000ULL;
    request.sample_rate_hz = 60000000U;
    request.bandwidth_hz = 56000000U;
    request.total_samples = 2U;
    request.chunk_samples = 2U;
    request.timeout_ms = 500U;
    memset(&state, 0, sizeof(state));
    memset(staging, 0xA5, sizeof(staging));
    status = ra8p1_sdr_capture_rx1_cached(
        &api, &state, &request, staging, sizeof(staging),
        rx1, sizeof(rx1), &captured);
    if ((status != 0) || (captured != 2U) ||
        (state.saw_untouched_staging == 0U) ||
        (get_s16_le(rx1) != -11) || (get_s16_le(rx1 + 2U) != 11) ||
        (get_s16_le(rx1 + 4U) != -12) || (get_s16_le(rx1 + 6U) != 12))
    {
        return 1;
    }
    return 0;
}

static int test_normalized_iq2_conversion(void)
{
    ra8p1_sdr_adapter_api_t api;
    ra8p1_sdr_iq2_sample_t input[2];
    uint8_t rx1[8];
    memset(&api, 0, sizeof(api));
    api.capture_format = RA8P1_SDR_CAPTURE_NORMALIZED_IQ2;
    api.sample_bytes = RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    input[0].rx1_i = -101;
    input[0].rx1_q = 102;
    input[0].rx2_i = -201;
    input[0].rx2_q = 202;
    input[1].rx1_i = -103;
    input[1].rx1_q = 104;
    input[1].rx2_i = -203;
    input[1].rx2_q = 204;
    if ((ra8p1_sdr_convert_rx1_chunk(
             &api, input, 2U, rx1, sizeof(rx1)) != 0) ||
        (get_s16_le(rx1) != -101) ||
        (get_s16_le(rx1 + 2U) != 102) ||
        (get_s16_le(rx1 + 4U) != -103) ||
        (get_s16_le(rx1 + 6U) != 104))
    {
        return 1;
    }
    return 0;
}

static int32_t scan_session_ready(void *callback_context, uint32_t session_id,
                                  uint32_t center_index, uint64_t center_hz,
                                  const uint8_t *rx1_iq_le, uint32_t sample_count)
{
    scan_callback_state_t *state = (scan_callback_state_t *) callback_context;
    if ((state == NULL) || (center_index != state->calls) ||
        (session_id != state->first_session_id + center_index) ||
        (center_hz != ra8p1_sdr_scan_center_hz(center_index)) ||
        (sample_count != 6000000U) ||
        (get_s16_le(rx1_iq_le) != -1000) ||
        (get_s16_le(rx1_iq_le + 2U) != 1000))
    {
        return -20;
    }
    state->calls++;
    return 0;
}

static int test_four_center_scan(const ra8p1_sdr_adapter_api_t *api,
                                 const ra8p1_sdr_adapter_config_t *config)
{
    ra8p1_sdr_capture_request_t request;
    ra8p1_sdr_scan_result_t result;
    scan_callback_state_t callback_state;
    void *context = NULL;
    uint8_t *staging = NULL;
    uint8_t *rx1 = NULL;
    int test_result = 10;

    if (api->open(&context, config) != 0)
    {
        return test_result;
    }
    staging = (uint8_t *) malloc(6000000U * 8U);
    rx1 = (uint8_t *) malloc(6000000U * 4U);
    if ((staging == NULL) || (rx1 == NULL))
    {
        test_result = 11;
        goto cleanup;
    }
    memset(&request, 0, sizeof(request));
    request.sample_rate_hz = 60000000U;
    request.bandwidth_hz = 56000000U;
    request.total_samples = 6000000U;
    request.chunk_samples = 6000000U;
    request.timeout_ms = 500U;
    memset(&callback_state, 0, sizeof(callback_state));
    callback_state.first_session_id = 800U;
    if ((ra8p1_sdr_capture_four_center_cycle(
             api, context, &request, staging, 6000000U * 8U,
             rx1, 6000000U * 4U, callback_state.first_session_id,
             scan_session_ready, &callback_state, &result) != 0) ||
        (callback_state.calls != 4U) || (result.centers_completed != 4U) ||
        (result.failed_center_index != UINT32_MAX) || (result.status != 0))
    {
        test_result = 12;
        goto cleanup;
    }
    test_result = 0;

cleanup:
    free(rx1);
    free(staging);
    if (api->close(context) != 0 && test_result == 0)
    {
        test_result = 13;
    }
    return test_result;
}

int main(void)
{
    ra8p1_sdr_adapter_api_t api;
    ra8p1_sdr_adapter_config_t config;
    ra8p1_sdr_capture_request_t request;
    uint8_t staging[16];
    uint8_t rx1[12];
    void *context = NULL;
    uint32_t captured = 0U;
    uint32_t n;
    int32_t status;

    if (test_default_fast_path_does_not_clear_staging() != 0)
    {
        return 20;
    }
    if (test_normalized_iq2_conversion() != 0)
    {
        return 21;
    }

    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t) sizeof(api);
    status = ra8p1_sdr_adapter_get_api_v1(RA8P1_SDR_ADAPTER_ABI_VERSION, &api);
    if (status != 0)
    {
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t) sizeof(config);
    config.abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    config.initial_center_frequency_hz = 2420000000ULL;
    config.sample_rate_hz = 60000000U;
    config.bandwidth_hz = 56000000U;
    config.rx_channels = 2U;
    if (api.open(&context, &config) != 0)
    {
        return 2;
    }
    memset(&request, 0, sizeof(request));
    request.center_frequency_hz = 5816000000ULL;
    request.sample_rate_hz = 60000000U;
    request.bandwidth_hz = 56000000U;
    request.total_samples = 3U;
    request.chunk_samples = 2U;
    request.timeout_ms = 500U;
    status = ra8p1_sdr_capture_rx1_cached(&api, context, &request,
                                           staging, sizeof(staging),
                                           rx1, sizeof(rx1), &captured);
    if ((status != 0) || (captured != 3U))
    {
        (void) api.close(context);
        return 3;
    }
    for (n = 0U; n < 3U; n++)
    {
        if ((get_s16_le(&rx1[n * 4U]) != (int16_t) (-1000 - (int32_t) n)) ||
            (get_s16_le(&rx1[n * 4U + 2U]) != (int16_t) (1000 + n)))
        {
            (void) api.close(context);
            return 4;
        }
    }
    if (api.close(context) != 0)
    {
        return 5;
    }
    status = test_four_center_scan(&api, &config);
    if (status != 0)
    {
        return status;
    }
    puts("sdr RAW/normalized bridge, no-clear fast path, and four-center scan tests passed");
    return 0;
}
