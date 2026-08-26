#ifndef RA8P1_SDR_ADAPTER_H
#define RA8P1_SDR_ADAPTER_H

/*
 * Stable boundary between the host-side cache sender and the 7020/AD936X
 * vendor implementation.  The vendor library is deliberately not linked
 * into the sender: its sdr_handle_t contains platform-private pointers and
 * cannot be safely reconstructed by a generic host program.
 */

#include <stddef.h>
#include <stdint.h>

#define RA8P1_SDR_ADAPTER_ABI_VERSION       (1U)
#define RA8P1_SDR_ADAPTER_GET_API_SYMBOL    "ra8p1_sdr_adapter_get_api_v1"
#define RA8P1_SDR_ADAPTER_SAMPLE_BYTES      (8U)
#define RA8P1_SDR_ADAPTER_RX_CHANNELS       (2U)

/* Capture data supplied by the adapter's rx_capture callback. */
typedef enum e_ra8p1_sdr_capture_format
{
    /*
     * One little-endian 8-byte record per complex sample:
     *   bytes 0..1 = Q1, bytes 2..3 = I1,
     *   bytes 4..5 = Q2, bytes 6..7 = I2.
     * This is the order produced by the documented AXI ADC 2R2T DMA words.
     */
    RA8P1_SDR_CAPTURE_RAW_2R2T_LE = 1U,

    /* Host-endian fields in ra8p1_sdr_iq2_sample_t order. */
    RA8P1_SDR_CAPTURE_NORMALIZED_IQ2 = 2U
} ra8p1_sdr_capture_format_t;

/* The documented normalized representation of one 2R2T sample. */
typedef struct st_ra8p1_sdr_iq2_sample
{
    int16_t rx1_i;
    int16_t rx1_q;
    int16_t rx2_i;
    int16_t rx2_q;
} ra8p1_sdr_iq2_sample_t;

typedef char ra8p1_sdr_iq2_sample_size_must_be_8[
    (sizeof(ra8p1_sdr_iq2_sample_t) == RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ? 1 : -1];

/* Configuration passed to the adapter when it is opened. */
typedef struct st_ra8p1_sdr_adapter_config
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t initial_center_frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t rx_channels;
    uint32_t reserved;
} ra8p1_sdr_adapter_config_t;

typedef int32_t (*ra8p1_sdr_adapter_open_fn)(
    void **context, const ra8p1_sdr_adapter_config_t *config);
typedef int32_t (*ra8p1_sdr_adapter_set_rx_fn)(
    void *context, uint64_t lo_hz, uint32_t sample_rate_hz, uint32_t bandwidth_hz);
typedef int32_t (*ra8p1_sdr_adapter_rx_capture_fn)(
    void *context, void *buffer, uint32_t sample_count, uint32_t timeout_ms);
typedef int32_t (*ra8p1_sdr_adapter_close_fn)(void *context);

/*
 * Optional v1 extension for adapters whose native DMA format is already RX1
 * S16 little-endian I,Q.  It avoids the legacy 8-byte 2R2T staging buffer and
 * writes exactly sample_count * 4 bytes into rx1_iq_le.  Callers must check
 * struct_size before reading this appended field.  The original v1 prefix
 * through close remains ABI compatible with existing adapters.
 */
typedef int32_t (*ra8p1_sdr_adapter_rx1_capture_le_fn)(
    void *context, uint8_t *rx1_iq_le, uint32_t sample_count,
    uint32_t timeout_ms);

#define RA8P1_SDR_ADAPTER_STATUS_VERSION              (1U)
#define RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_SUPPORTED   (1U << 0)
#define RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_READY       (1U << 1)
#define RA8P1_SDR_ADAPTER_STATUS_LAST_TUNE_FASTLOCK   (1U << 2)
#define RA8P1_SDR_ADAPTER_STATUS_FASTLOCK_FALLBACK    (1U << 3)
#define RA8P1_SDR_ADAPTER_STATUS_LAST_CAPTURE_TUNE_GUARDED (1U << 4)

typedef struct st_ra8p1_sdr_adapter_status
{
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t fastlock_profiles;
    uint64_t center_frequency_hz;
    uint64_t tune_start_ns;
    uint64_t tune_complete_ns;
    uint32_t tune_count;
    uint32_t fastlock_recall_count;
    uint32_t fallback_count;
    int32_t last_tune_status;
    /* Optional capture-stage timestamps. They are monotonic nanoseconds and
     * preserve the append-only v1 status prefix. */
    uint64_t capture_prepare_ns;
    uint64_t blocks_ready_ns;
    uint64_t buffer_enable_ns;
    uint64_t block_dequeue_ns;
    uint64_t buffer_disable_ns;
    uint64_t copy_complete_ns;
} ra8p1_sdr_adapter_status_t;

#define RA8P1_SDR_ADAPTER_STATUS_V1_SIZE \
    ((uint32_t)offsetof(ra8p1_sdr_adapter_status_t, capture_prepare_ns))

typedef char ra8p1_sdr_adapter_status_v1_size_must_be_56[
    (RA8P1_SDR_ADAPTER_STATUS_V1_SIZE == 56U) ? 1 : -1];
typedef char ra8p1_sdr_adapter_status_size_must_be_104[
    (sizeof(ra8p1_sdr_adapter_status_t) == 104U) ? 1 : -1];

typedef int32_t (*ra8p1_sdr_adapter_get_status_fn)(
    void *context, ra8p1_sdr_adapter_status_t *status);

/*
 * Function table exported by the vendor shim.  A plugin must set struct_size
 * to the number of bytes it initialized.  Future ABI revisions can append
 * fields without changing the v1 prefix.
 */
typedef struct st_ra8p1_sdr_adapter_api
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capture_format;
    uint32_t sample_bytes;
    const char *name;
    ra8p1_sdr_adapter_open_fn open;
    ra8p1_sdr_adapter_set_rx_fn set_rx;
    ra8p1_sdr_adapter_rx_capture_fn rx_capture;
    ra8p1_sdr_adapter_close_fn close;
    ra8p1_sdr_adapter_rx1_capture_le_fn rx1_capture_le;
    ra8p1_sdr_adapter_get_status_fn get_status;
} ra8p1_sdr_adapter_api_t;

#define RA8P1_SDR_ADAPTER_V1_CORE_SIZE \
    (offsetof(ra8p1_sdr_adapter_api_t, close) + \
     sizeof(((ra8p1_sdr_adapter_api_t *) 0)->close))
#define RA8P1_SDR_ADAPTER_V1_RX1_LE_SIZE \
    (offsetof(ra8p1_sdr_adapter_api_t, rx1_capture_le) + \
     sizeof(((ra8p1_sdr_adapter_api_t *) 0)->rx1_capture_le))
#define RA8P1_SDR_ADAPTER_V1_STATUS_SIZE \
    (offsetof(ra8p1_sdr_adapter_api_t, get_status) + \
     sizeof(((ra8p1_sdr_adapter_api_t *) 0)->get_status))

typedef int32_t (*ra8p1_sdr_adapter_get_api_fn)(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api);

/*
 * The following helpers are implemented by sdr_iq_udp_stream.c.  Keeping the
 * loader out of the ABI header lets a vendor shim include this file without
 * pulling in Windows or POSIX dynamic-loader headers.
 */
typedef struct st_ra8p1_sdr_adapter_runtime
{
    void *module;
    ra8p1_sdr_adapter_api_t api;
    void *context;
    uint32_t loaded;
} ra8p1_sdr_adapter_runtime_t;

#endif /* RA8P1_SDR_ADAPTER_H */
