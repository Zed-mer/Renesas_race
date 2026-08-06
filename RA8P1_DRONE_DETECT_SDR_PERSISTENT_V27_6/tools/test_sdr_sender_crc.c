/* Host regression test for the SDR sender's optional CRC32C backends. */
#define RA8P1_SDR_IQ_STREAM_EMBEDDED 1
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "sdr_iq_udp_stream.c"

static int verify_cached_window_crc(const uint8_t *data, uint32_t sample_count,
                                    int use_retry_cache, int drop_first,
                                    int corrupt_end, int use_precompute)
{
    options_t options;
    struct sockaddr_in peer;
    send_session_result_t result;
    send_crc_precompute_t precompute;
    const send_crc_precompute_t *precomputed = NULL;
    const uint32_t bytes = sample_count * IQ_BYTES_PER_SAMPLE;
    const sdr_crc_backend_t backend = crc32c_backend_selected();
    uint32_t expected = crc32c_update(0xFFFFFFFFU, data, bytes, backend) ^
                        0xFFFFFFFFU;

    memset(&options, 0, sizeof(options));
    memset(&peer, 0, sizeof(peer));
    memset(&result, 0, sizeof(result));
    options.source_mode = use_retry_cache ? SOURCE_STDIN : SOURCE_SDR;
    options.samples_per_session = sample_count;
    options.sample_rate_hz = IQ_DEFAULT_SAMPLE_RATE_HZ;
    options.bandwidth_hz = IQ_DEFAULT_BANDWIDTH_HZ;
    options.send_batch = 16U;
    options.window_crc = 1;
    options.dry_run = 1;
    options.test_drop_data_packet = drop_first;
    options.test_corrupt_end_crc = corrupt_end;
    if (corrupt_end != 0)
    {
        expected ^= 1U;
    }
    if (use_precompute != 0)
    {
        if (!send_crc_precompute_window(data, bytes, &precompute))
        {
            fprintf(stderr, "cached window CRC precompute failed\n");
            return 0;
        }
        if ((precompute.accumulator ^ 0xFFFFFFFFU) !=
            (expected ^ ((corrupt_end != 0) ? 1U : 0U)))
        {
            fprintf(stderr, "cached window CRC precompute mismatch\n");
            return 0;
        }
        precomputed = &precompute;
    }
    if (!send_session_once(INVALID_SOCKET_HANDLE, &peer, &options, 77U,
                           2420000000ULL, 0U, NULL,
                           use_retry_cache ? NULL : data,
                           use_retry_cache ? data : NULL, precomputed,
                           &result))
    {
        fprintf(stderr, "cached window sender failed\n");
        return 0;
    }
    if ((result.crc32c != expected) ||
        (result.payload_bytes != bytes) ||
        (result.data_packets !=
         ((bytes + IQ_DATA_BYTES - 1U) / IQ_DATA_BYTES)))
    {
        fprintf(stderr,
                "cached window CRC mismatch retry=%d drop=%d corrupt=%d precompute=%d "
                "actual=0x%08" PRIX32 " expected=0x%08" PRIX32 "\n",
                use_retry_cache, drop_first, corrupt_end, use_precompute,
                result.crc32c, expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    uint8_t data[4096];
    uint32_t length;
    uint32_t index;
    const uint8_t vector[] = "123456789";
    const uint32_t expected = 0xE3069283U;

    for (index = 0U; index < (uint32_t)sizeof(data); ++index)
    {
        data[index] = (uint8_t)((index * 37U) ^ (index >> 3U));
    }
    if ((crc32c_update_nibble(0xFFFFFFFFU, vector,
                               (uint32_t)(sizeof(vector) - 1U)) ^
         0xFFFFFFFFU) != expected)
    {
        fprintf(stderr, "nibble CRC known vector failed\n");
        return 1;
    }
    if ((crc32c_update_slice8(0xFFFFFFFFU, vector,
                              (uint32_t)(sizeof(vector) - 1U)) ^
         0xFFFFFFFFU) != expected)
    {
        fprintf(stderr, "slice8 CRC known vector failed\n");
        return 1;
    }
    for (length = 0U; length <= (uint32_t)sizeof(data); ++length)
    {
        const uint32_t nibble = crc32c_update_nibble(
            0xFFFFFFFFU, data, length);
        const uint32_t slice8 = crc32c_update_slice8(
            0xFFFFFFFFU, data, length);
        if (nibble != slice8)
        {
            fprintf(stderr, "CRC backend mismatch at length=%" PRIu32
                    " nibble=0x%08" PRIX32 " slice8=0x%08" PRIX32 "\n",
                    length, nibble, slice8);
            return 1;
        }
    }
    if (!verify_cached_window_crc(data, 1000U, 0, 0, 0, 0) ||
        !verify_cached_window_crc(data, 1000U, 1, 0, 0, 0) ||
        !verify_cached_window_crc(data, 1000U, 0, 1, 0, 0) ||
        !verify_cached_window_crc(data, 1000U, 0, 0, 1, 0) ||
        !verify_cached_window_crc(data, 1000U, 0, 0, 0, 1) ||
        !verify_cached_window_crc(data, 1000U, 1, 1, 1, 1))
    {
        return 1;
    }
    printf("sdr sender CRC nibble/slice8 equivalence passed\n");
    return 0;
}
