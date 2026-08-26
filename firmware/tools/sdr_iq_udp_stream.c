/*
 * Host-side SDR cache sender for the RA8P1 IQSC UDP/5003 protocol.
 *
 * Input is RX1 S16 little-endian I,Q (four bytes per complex sample).  The
 * SDR captures a complete 60 MSPS session into its DDR before this program
 * sends it; pacing therefore controls Ethernet burst rate, not RF time.
 */

#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../shared/analysis_contract.h"
#include "../shared/iq_protocol.h"

#ifdef _WIN32
#include <malloc.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define close_socket closesocket
typedef SOCKET socket_handle_t;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/udp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#define close_socket close
typedef int socket_handle_t;
#define INVALID_SOCKET_HANDLE (-1)
#endif

#include "sdr_adapter.h"
#include "sdr_capture_bridge.h"
#include "sdr_scan_bridge.h"

#define IQ_PORT                       5003U
#define IQ_ACK_PORT                   5002U
#define IQ_MAGIC                       0x5149504BU
#define IQ_FORMAT_S16                  1U
#define IQ_HEADER_BYTES                32U
#define IQSC_MAGIC                     0x49515343U
#define IQSC_VERSION                   2U
#define IQSC_BYTES                     68U
#define IQ_UDP_BYTES                   1472U
#define IQ_DATA_BYTES                  (IQ_UDP_BYTES - IQ_HEADER_BYTES)
#define IQ_BYTES_PER_SAMPLE            4U
#define IQ_SAMPLES_PER_PACKET          (IQ_DATA_BYTES / IQ_BYTES_PER_SAMPLE)
#define IQ_DEFAULT_SAMPLE_RATE_HZ      RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ
#define IQ_DEFAULT_BANDWIDTH_HZ        RA8P1_ANALYSIS_BANDWIDTH_HZ
#define IQ_DEFAULT_SAMPLES_PER_SESSION ((uint32_t) RA8P1_ANALYSIS_TOTAL_SAMPLES)
#define IQ_MODEL_WINDOW_SAMPLES        RA8P1_ANALYSIS_TILE_SAMPLES
#define IQ_MODEL_STRIDE_SAMPLES        RA8P1_ANALYSIS_TILE_STRIDE_SAMPLES
#define IQ_FLAG_SYNTHETIC              (1U << 0)
#define IQ_FLAG_STREAM_START           (1U << 3)
#define IQ_FLAG_STREAM_END             (1U << 4)
#define IQ_FLAG_VALID_BITS_12          (1U << 5)
#define IQ_FLAG_WINDOW_CRC             ((uint32_t) RA8P1_IQ_FLAG_WINDOW_CRC)
#define IQ_CENTER_COUNT                4U
#define IQ_SDR_CAPTURE_CHUNK_SAMPLES   IQ_DEFAULT_SAMPLES_PER_SESSION
#define IQ_SDR_CAPTURE_TIMEOUT_MS      500U
#define IQ_SDR_ADAPTER_ENV             "RA8P1_SDR_ADAPTER"
#define IQ_SDR_UDP_GSO_ENV             "RA8P1_SDR_UDP_GSO"
#define IQ_SDR_UDP_NO_CHECK_ENV        "RA8P1_SDR_UDP_NO_CHECK"
#define IQ_RATE_SWEEP_MAX              16U
#define IQ_SEND_BATCH_MAX               64U
#define IQ_SEND_BATCH_DEFAULT            1U
#define IQ_UDP_GSO_MAX_BATCH             32U
#define IQ_DEFAULT_SOCKET_SNDBUF_BYTES (4U * 1024U * 1024U)
#define IQ_DEFAULT_ACK_TIMEOUT_MS       1000U
#define IQ_DEFAULT_ACK_RETRIES          2U
#define IQ_ACK_REQUEST_MAGIC            ((uint32_t) RA8P1_IQ_ACK_REQUEST_MAGIC)
#define IQ_ACK_RESPONSE_MAGIC           ((uint32_t) RA8P1_IQ_ACK_RESPONSE_MAGIC)
#define IQ_ACK_VERSION                  ((uint16_t) RA8P1_IQ_ACK_VERSION)
#define IQ_ACK_FLAG_ACTIVE              ((uint32_t) RA8P1_IQ_ACK_FLAG_ACTIVE)
#define IQ_ACK_FLAG_COMPLETE            ((uint32_t) RA8P1_IQ_ACK_FLAG_COMPLETE)
#define IQ_ACK_FLAG_CRC_PRESENT         ((uint32_t) RA8P1_IQ_ACK_FLAG_CRC_PRESENT)
#define IQ_ACK_FLAG_CRC_VALID           ((uint32_t) RA8P1_IQ_ACK_FLAG_CRC_VALID)
#define IQ_ACK_FLAG_SEQUENCE_ERROR      ((uint32_t) RA8P1_IQ_ACK_FLAG_SEQUENCE_ERROR)
#define IQ_ACK_FLAG_RING_DROP           ((uint32_t) RA8P1_IQ_ACK_FLAG_RING_DROP)
#define IQ_ACK_FLAGS_ALL                (IQ_ACK_FLAG_ACTIVE | IQ_ACK_FLAG_COMPLETE | \
                                         IQ_ACK_FLAG_CRC_PRESENT | IQ_ACK_FLAG_CRC_VALID | \
                                         IQ_ACK_FLAG_SEQUENCE_ERROR | IQ_ACK_FLAG_RING_DROP)
#define IQ_ACK_STATUS_OK                ((uint32_t) RA8P1_IQ_ACK_STATUS_OK)
#define IQ_ACK_STATUS_ACTIVE            ((uint32_t) RA8P1_IQ_ACK_STATUS_ACTIVE)

typedef char iq_ack_request_host_size_must_be_16[
    (sizeof(ra8p1_iq_ack_request_t) == 16U) ? 1 : -1];
typedef char iq_ack_response_host_size_must_be_72[
    (sizeof(ra8p1_iq_ack_response_t) == 72U) ? 1 : -1];

#ifdef RA8P1_SDR_ADAPTER_STATIC
extern int32_t ra8p1_sdr_adapter_get_api_v1(
    uint32_t requested_abi, ra8p1_sdr_adapter_api_t *api);
#endif

typedef enum e_source_mode
{
    SOURCE_FILE,
    SOURCE_STDIN,
    SOURCE_SYNTHETIC,
    SOURCE_SDR
} source_mode_t;

typedef struct st_options
{
    const char *target_ip;
    const char *file_path;
    const char *sdr_library_path;
    source_mode_t source_mode;
    uint64_t centers[IQ_CENTER_COUNT];
    uint32_t center_indices[IQ_CENTER_COUNT];
    uint32_t center_count;
    uint32_t sessions;
    uint32_t samples_per_session;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    uint32_t rate_mbps;
    uint32_t rate_sweep[IQ_RATE_SWEEP_MAX];
    uint32_t rate_count;
    uint32_t session_id;
    uint32_t capture_chunk_samples;
    uint32_t capture_timeout_ms;
    uint32_t socket_sndbuf_bytes;
    uint32_t send_batch;
    uint32_t ack_timeout_ms;
    uint32_t ack_retries;
    int udp_no_check;
    int udp_gso;
    int window_crc;
    int ack_enabled;
    int dry_run;
    int test_corrupt_end_crc;
    int test_drop_data_packet;
} options_t;

typedef struct st_iq_ack_result
{
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
} iq_ack_result_t;

#if defined(__linux__) && !defined(UDP_SEGMENT)
#define UDP_SEGMENT 103
#endif

#if defined(__linux__) && !defined(SOL_UDP)
#define SOL_UDP 17
#endif

/*
 * Payload pacing is deliberately deadline based.  A relative sleep after a
 * send accumulates scheduler and syscall jitter; a late wake then causes the
 * following packets to be emitted back-to-back while the sender catches up.
 * The epoch/bit counter below represents the ideal elapsed wire time.  Small
 * deadline misses preserve that epoch so syscall and clock-quantization jitter
 * cannot accumulate into every batch.  Only a longer scheduler stall starts a
 * new epoch, which bounds the resulting catch-up burst.
 */
#define IQ_PACER_REBASE_LATE_US 750ULL

typedef struct st_payload_pacer
{
    uint32_t rate_mbps;
    uint64_t epoch_us;
    uint64_t deadline_us;
    uint64_t paced_bits;
    uint32_t late_rebases;
    uint64_t max_late_us;
} payload_pacer_t;

static uint64_t monotonic_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t seconds;
    uint64_t remainder;
    if (frequency.QuadPart == 0)
    {
        if (QueryPerformanceFrequency(&frequency) == 0)
        {
            return (uint64_t) GetTickCount64() * 1000ULL;
        }
    }
    if (QueryPerformanceCounter(&counter) == 0)
    {
        return (uint64_t) GetTickCount64() * 1000ULL;
    }
    seconds = (uint64_t) counter.QuadPart / (uint64_t) frequency.QuadPart;
    remainder = (uint64_t) counter.QuadPart % (uint64_t) frequency.QuadPart;
    return seconds * 1000000ULL +
           (remainder * 1000000ULL) / (uint64_t) frequency.QuadPart;
#else
    struct timespec now;
    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t) now.tv_sec * 1000000ULL) + ((uint64_t) now.tv_nsec / 1000ULL);
#endif
}

static void sdr_adapter_unload_module(ra8p1_sdr_adapter_runtime_t *runtime)
{
    if (runtime->module == NULL)
    {
        return;
    }
#ifdef _WIN32
    (void) FreeLibrary((HMODULE) runtime->module);
#else
    (void) dlclose(runtime->module);
#endif
    runtime->module = NULL;
}

static int sdr_adapter_load_symbol(ra8p1_sdr_adapter_runtime_t *runtime,
                                   ra8p1_sdr_adapter_get_api_fn *get_api)
{
#ifdef _WIN32
    FARPROC symbol = GetProcAddress((HMODULE) runtime->module,
                                    RA8P1_SDR_ADAPTER_GET_API_SYMBOL);
    if (symbol == NULL)
    {
        fprintf(stderr, "SDR adapter is missing %s (GetLastError=%lu)\n",
                RA8P1_SDR_ADAPTER_GET_API_SYMBOL, (unsigned long) GetLastError());
        return 0;
    }
    memcpy(get_api, &symbol, sizeof(*get_api));
#else
    void *symbol;
    const char *error;
    (void) dlerror();
    symbol = dlsym(runtime->module, RA8P1_SDR_ADAPTER_GET_API_SYMBOL);
    error = dlerror();
    if ((error != NULL) || (symbol == NULL))
    {
        fprintf(stderr, "SDR adapter is missing %s: %s\n",
                RA8P1_SDR_ADAPTER_GET_API_SYMBOL,
                error != NULL ? error : "symbol not found");
        return 0;
    }
    memcpy(get_api, &symbol, sizeof(*get_api));
#endif
    return 1;
}

static int sdr_adapter_open(ra8p1_sdr_adapter_runtime_t *runtime,
                            const char *library_path, uint64_t initial_center_hz,
                            uint32_t sample_rate_hz, uint32_t bandwidth_hz)
{
    ra8p1_sdr_adapter_get_api_fn get_api = NULL;
    ra8p1_sdr_adapter_config_t config;
    int32_t status;
    size_t required_size = RA8P1_SDR_ADAPTER_V1_CORE_SIZE;

    memset(runtime, 0, sizeof(*runtime));
    if (library_path == NULL)
    {
#ifdef RA8P1_SDR_ADAPTER_STATIC
        get_api = ra8p1_sdr_adapter_get_api_v1;
#else
        fprintf(stderr,
                "--sdr requires a build with RA8P1_SDR_ADAPTER_STATIC and a direct vendor adapter\n");
        return 0;
#endif
    }
    else
    {
#ifdef _WIN32
        runtime->module = (void *) LoadLibraryA(library_path);
        if (runtime->module == NULL)
        {
            fprintf(stderr, "cannot load SDR adapter %s (GetLastError=%lu)\n",
                    library_path, (unsigned long) GetLastError());
            return 0;
        }
#else
        runtime->module = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
        if (runtime->module == NULL)
        {
            fprintf(stderr, "cannot load SDR adapter %s: %s\n", library_path, dlerror());
            return 0;
        }
#endif
        if (!sdr_adapter_load_symbol(runtime, &get_api))
        {
            sdr_adapter_unload_module(runtime);
            return 0;
        }
    }
    memset(&runtime->api, 0, sizeof(runtime->api));
    runtime->api.struct_size = (uint32_t) sizeof(runtime->api);
    runtime->api.abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    status = get_api(RA8P1_SDR_ADAPTER_ABI_VERSION, &runtime->api);
    if (status != 0)
    {
        fprintf(stderr, "SDR adapter API query failed: status=%" PRId32 "\n", status);
        sdr_adapter_unload_module(runtime);
        return 0;
    }
    if ((runtime->api.abi_version != RA8P1_SDR_ADAPTER_ABI_VERSION) ||
        ((size_t) runtime->api.struct_size < required_size) ||
        (runtime->api.sample_bytes != RA8P1_SDR_ADAPTER_SAMPLE_BYTES) ||
        ((runtime->api.capture_format != RA8P1_SDR_CAPTURE_RAW_2R2T_LE) &&
         (runtime->api.capture_format != RA8P1_SDR_CAPTURE_NORMALIZED_IQ2)) ||
        (runtime->api.open == NULL) || (runtime->api.set_rx == NULL) ||
        (runtime->api.rx_capture == NULL) || (runtime->api.close == NULL))
    {
        fprintf(stderr,
                "SDR adapter ABI is incompatible: abi=%" PRIu32
                " size=%" PRIu32 " format=%" PRIu32 " sample_bytes=%" PRIu32 "\n",
                runtime->api.abi_version, runtime->api.struct_size,
                runtime->api.capture_format, runtime->api.sample_bytes);
        sdr_adapter_unload_module(runtime);
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = (uint32_t) sizeof(config);
    config.abi_version = RA8P1_SDR_ADAPTER_ABI_VERSION;
    config.initial_center_frequency_hz = initial_center_hz;
    config.sample_rate_hz = sample_rate_hz;
    config.bandwidth_hz = bandwidth_hz;
    config.rx_channels = RA8P1_SDR_ADAPTER_RX_CHANNELS;
    status = runtime->api.open(&runtime->context, &config);
    if ((status != 0) || (runtime->context == NULL))
    {
        fprintf(stderr, "SDR adapter open failed: status=%" PRId32 " context=%p\n",
                status, runtime->context);
        runtime->context = NULL;
        sdr_adapter_unload_module(runtime);
        return 0;
    }
    runtime->loaded = 1U;
    fprintf(stderr, "SDR adapter loaded: name=%s format=%" PRIu32 " path=%s\n",
            runtime->api.name != NULL ? runtime->api.name : "unnamed",
            runtime->api.capture_format, library_path != NULL ? library_path : "static");
    return 1;
}

static int sdr_adapter_close(ra8p1_sdr_adapter_runtime_t *runtime)
{
    int result = 1;
    if ((runtime->loaded != 0U) && (runtime->api.close != NULL))
    {
        int32_t status = runtime->api.close(runtime->context);
        if (status != 0)
        {
            fprintf(stderr, "SDR adapter close failed: status=%" PRId32 "\n", status);
            result = 0;
        }
    }
    runtime->context = NULL;
    runtime->loaded = 0U;
    sdr_adapter_unload_module(runtime);
    memset(&runtime->api, 0, sizeof(runtime->api));
    return result;
}

static void *sdr_aligned_allocate(size_t alignment, size_t bytes)
{
#ifdef _WIN32
    return _aligned_malloc(bytes, alignment);
#else
    void *buffer = NULL;
    if (posix_memalign(&buffer, alignment, bytes) != 0)
    {
        return NULL;
    }
    return buffer;
#endif
}

static void sdr_aligned_free(void *buffer)
{
#ifdef _WIN32
    _aligned_free(buffer);
#else
    free(buffer);
#endif
}

static void put_u16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t) value;
    buffer[offset + 1U] = (uint8_t) (value >> 8U);
}

static void put_u32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
    buffer[offset] = (uint8_t) value;
    buffer[offset + 1U] = (uint8_t) (value >> 8U);
    buffer[offset + 2U] = (uint8_t) (value >> 16U);
    buffer[offset + 3U] = (uint8_t) (value >> 24U);
}

static void put_u64(uint8_t *buffer, uint32_t offset, uint64_t value)
{
    put_u32(buffer, offset, (uint32_t) value);
    put_u32(buffer, offset + 4U, (uint32_t) (value >> 32U));
}

static void put_s16(uint8_t *buffer, uint32_t offset, int16_t value)
{
    put_u16(buffer, offset, (uint16_t) value);
}

static uint16_t get_u16(const uint8_t *buffer, uint32_t offset)
{
    return (uint16_t)buffer[offset] |
           (uint16_t)((uint16_t)buffer[offset + 1U] << 8U);
}

static uint32_t get_u32(const uint8_t *buffer, uint32_t offset)
{
    return (uint32_t)buffer[offset] |
           ((uint32_t)buffer[offset + 1U] << 8U) |
           ((uint32_t)buffer[offset + 2U] << 16U) |
           ((uint32_t)buffer[offset + 3U] << 24U);
}

static uint64_t get_u64(const uint8_t *buffer, uint32_t offset)
{
    return (uint64_t)get_u32(buffer, offset) |
           ((uint64_t)get_u32(buffer, offset + 4U) << 32U);
}

/* Reflected Castagnoli CRC32C, kept nibble-wise so the optional integrity
 * path needs only a tiny table and remains independent of host CRC intrinsics. */
static const uint32_t g_crc32c_nibble_table[16] =
{
    0x00000000U, 0x105EC76FU, 0x20BD8EDEU, 0x30E349B1U,
    0x417B1DBCU, 0x5125DAD3U, 0x61C69362U, 0x7198540DU,
    0x82F63B78U, 0x92A8FC17U, 0xA24BB5A6U, 0xB21572C9U,
    0xC38D26C4U, 0xD3D3E1ABU, 0xE330A81AU, 0xF36E6F75U
};

typedef enum e_sdr_crc_backend
{
    SDR_CRC_BACKEND_NIBBLE = 0U,
    SDR_CRC_BACKEND_SLICE8 = 1U
} sdr_crc_backend_t;

/*
 * The nibble implementation above is the compatibility baseline.  The
 * optional table below is deliberately initialized at runtime so the source
 * stays auditable and the default image does not carry a second 8 KiB table
 * unless the slicing backend is explicitly selected.
 */
static uint32_t g_crc32c_slice8_table[8][256];
static uint32_t g_crc32c_slice8_initialized;
static sdr_crc_backend_t g_sdr_crc_backend = SDR_CRC_BACKEND_NIBBLE;
static uint32_t g_sdr_crc_backend_selected;

static uint32_t crc32c_update_nibble(uint32_t crc, const uint8_t *data,
                                     uint32_t length)
{
    while ((data != NULL) && (length != 0U))
    {
        crc ^= *data++;
        crc = (crc >> 4U) ^ g_crc32c_nibble_table[crc & 0x0FU];
        crc = (crc >> 4U) ^ g_crc32c_nibble_table[crc & 0x0FU];
        length--;
    }
    return crc;
}

static void crc32c_slice8_init(void)
{
    uint32_t index;
    uint32_t table_index;

    if (g_crc32c_slice8_initialized != 0U)
    {
        return;
    }
    for (index = 0U; index < 256U; ++index)
    {
        uint32_t value = index;
        for (table_index = 0U; table_index < 8U; ++table_index)
        {
            value = ((value & 1U) != 0U) ?
                ((value >> 1U) ^ 0x82F63B78U) : (value >> 1U);
        }
        g_crc32c_slice8_table[0][index] = value;
    }
    for (table_index = 1U; table_index < 8U; ++table_index)
    {
        for (index = 0U; index < 256U; ++index)
        {
            const uint32_t previous =
                g_crc32c_slice8_table[table_index - 1U][index];
            g_crc32c_slice8_table[table_index][index] =
                (previous >> 8U) ^
                g_crc32c_slice8_table[0][previous & 0xFFU];
        }
    }
    g_crc32c_slice8_initialized = 1U;
}

static uint32_t crc32c_update_slice8(uint32_t crc, const uint8_t *data,
                                     uint32_t length)
{
    crc32c_slice8_init();
    while ((data != NULL) && (length >= 8U))
    {
        const uint32_t word = get_u32(data, 0U) ^ crc;
        crc = g_crc32c_slice8_table[7][word & 0xFFU] ^
              g_crc32c_slice8_table[6][(word >> 8U) & 0xFFU] ^
              g_crc32c_slice8_table[5][(word >> 16U) & 0xFFU] ^
              g_crc32c_slice8_table[4][(word >> 24U) & 0xFFU] ^
              g_crc32c_slice8_table[3][data[4]] ^
              g_crc32c_slice8_table[2][data[5]] ^
              g_crc32c_slice8_table[1][data[6]] ^
              g_crc32c_slice8_table[0][data[7]];
        data += 8U;
        length -= 8U;
    }
    return crc32c_update_nibble(crc, data, length);
}

static sdr_crc_backend_t crc32c_backend_selected(void)
{
    const char *value;

    if (g_sdr_crc_backend_selected != 0U)
    {
        return g_sdr_crc_backend;
    }
    value = getenv("RA8P1_SDR_CRC_BACKEND");
    if ((value != NULL) &&
        ((strcmp(value, "slice8") == 0) ||
         (strcmp(value, "slicing8") == 0) ||
         (strcmp(value, "SLICE8") == 0) ||
         (strcmp(value, "SLICING8") == 0)))
    {
        g_sdr_crc_backend = SDR_CRC_BACKEND_SLICE8;
        crc32c_slice8_init();
    }
    else
    {
        g_sdr_crc_backend = SDR_CRC_BACKEND_NIBBLE;
    }
    g_sdr_crc_backend_selected = 1U;
    return g_sdr_crc_backend;
}

static const char *crc32c_backend_name(sdr_crc_backend_t backend)
{
    return (backend == SDR_CRC_BACKEND_SLICE8) ? "slice8" : "nibble";
}

static uint32_t crc32c_update(uint32_t crc, const uint8_t *data,
                              uint32_t length, sdr_crc_backend_t backend)
{
    return (backend == SDR_CRC_BACKEND_SLICE8) ?
        crc32c_update_slice8(crc, data, length) :
        crc32c_update_nibble(crc, data, length);
}

static uint64_t crc32c_thread_cpu_us(void)
{
#ifdef _WIN32
    return monotonic_us();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
    {
        return monotonic_us();
    }
    return ((uint64_t) now.tv_sec * 1000000ULL) +
           ((uint64_t) now.tv_nsec / 1000ULL);
#endif
}

static uint32_t crc32c_timing_enabled(void)
{
    const char *value = getenv("RA8P1_SDR_CRC_TRACE");
    if ((value == NULL) || (value[0] == '\0') ||
        (strcmp(value, "0") == 0) ||
        (strcmp(value, "off") == 0) ||
        (strcmp(value, "OFF") == 0) ||
        (strcmp(value, "false") == 0) ||
        (strcmp(value, "FALSE") == 0))
    {
        return 0U;
    }
    return 1U;
}

static uint32_t crc32c_update_timed(uint32_t crc, const uint8_t *data,
                                    uint32_t length, sdr_crc_backend_t backend,
                                    uint32_t timing_enabled,
                                    uint64_t *timing_us)
{
    uint64_t started_us;
    uint64_t finished_us;
    uint32_t result;

    if ((timing_enabled == 0U) || (timing_us == NULL))
    {
        return crc32c_update(crc, data, length, backend);
    }
    started_us = crc32c_thread_cpu_us();
    result = crc32c_update(crc, data, length, backend);
    finished_us = crc32c_thread_cpu_us();
    if (finished_us >= started_us)
    {
        *timing_us += finished_us - started_us;
    }
    return result;
}

static void sleep_ms(uint32_t milliseconds)
{
    if (milliseconds == 0U)
    {
        return;
    }
#ifdef _WIN32
    Sleep(milliseconds);
#else
    {
        struct timespec request;
        request.tv_sec = (time_t)(milliseconds / 1000U);
        request.tv_nsec = (long)((milliseconds % 1000U) * 1000000U);
        (void)nanosleep(&request, NULL);
    }
#endif
}

static int socket_wait_readable(socket_handle_t socket_fd, uint32_t timeout_ms)
{
    fd_set read_set;
    struct timeval timeout;
    int result;

    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);
    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_usec = (long)((timeout_ms % 1000U) * 1000U);
#ifdef _WIN32
    result = select(0, &read_set, NULL, NULL, &timeout);
#else
    result = select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
#endif
    return result > 0;
}

static int send_iq_ack_request(socket_handle_t socket_fd,
                               uint32_t request_id,
                               uint32_t session_id)
{
    uint8_t request[sizeof(ra8p1_iq_ack_request_t)];
    int sent;

    memset(request, 0, sizeof(request));
    put_u32(request, 0U, IQ_ACK_REQUEST_MAGIC);
    put_u16(request, 4U, IQ_ACK_VERSION);
    put_u16(request, 6U, (uint16_t)sizeof(request));
    put_u32(request, 8U, request_id);
    put_u32(request, 12U, session_id);
    sent = send(socket_fd, (const char *)request, (int)sizeof(request), 0);
    return sent == (int)sizeof(request);
}

static int parse_iq_ack_response(const uint8_t *response,
                                 uint32_t response_bytes,
                                 uint32_t request_id,
                                 uint32_t session_id,
                                 iq_ack_result_t *result)
{
    if ((response == NULL) || (result == NULL) ||
        (response_bytes < sizeof(ra8p1_iq_ack_response_t)) ||
        (get_u32(response, 0U) != IQ_ACK_RESPONSE_MAGIC) ||
        (get_u16(response, 4U) != IQ_ACK_VERSION) ||
        (get_u16(response, 6U) != sizeof(ra8p1_iq_ack_response_t)) ||
        (get_u32(response, 8U) != request_id) ||
        (get_u32(response, 12U) != session_id))
    {
        return 0;
    }
    memset(result, 0, sizeof(*result));
    result->status = get_u32(response, 16U);
    result->flags = get_u32(response, 20U);
    result->packets = get_u32(response, 24U);
    result->payload_bytes = get_u64(response, 28U);
    result->sequence_gaps = get_u32(response, 36U);
    result->reordered = get_u32(response, 40U);
    result->invalid = get_u32(response, 44U);
    result->ring_full_drops = get_u32(response, 48U);
    result->ring_oversize_drops = get_u32(response, 52U);
    result->ring_free = get_u32(response, 56U);
    result->crc32c = get_u32(response, 60U);
    result->expected_crc32c = get_u32(response, 64U);
    result->crc_errors = get_u32(response, 68U);
    return 1;
}

static int iq_ack_is_complete(socket_handle_t ack_socket,
                               const options_t *options,
                               uint32_t session_id,
                               uint32_t request_id,
                               uint32_t expected_packets,
                               uint64_t expected_payload_bytes,
                               uint32_t expected_crc32c)
{
    const uint64_t deadline = monotonic_us() +
                              (uint64_t)options->ack_timeout_ms * 1000ULL;
    uint8_t response[sizeof(ra8p1_iq_ack_response_t) + 32U];
    int completion_seen = 0;

    while (monotonic_us() < deadline)
    {
        iq_ack_result_t result;
        uint64_t now;
        uint64_t remaining_us;
        uint32_t wait_ms;
        int received;

        if (!send_iq_ack_request(ack_socket, request_id, session_id))
        {
            fprintf(stderr, "ACK request send failed for session=%" PRIu32 "\n",
                    session_id);
            return 0;
        }
        now = monotonic_us();
        if (now >= deadline)
        {
            break;
        }
        remaining_us = deadline - now;
        wait_ms = (uint32_t)((remaining_us + 999ULL) / 1000ULL);
        if (wait_ms > 20U)
        {
            wait_ms = 20U;
        }
        if (!socket_wait_readable(ack_socket, wait_ms))
        {
            continue;
        }
        received = recv(ack_socket, (char *)response, (int)sizeof(response), 0);
        if (received <= 0)
        {
            continue;
        }
        if (!parse_iq_ack_response(response, (uint32_t)received,
                                   request_id, session_id, &result))
        {
            /* Ignore stale/malformed datagrams; the deadline bounds polling. */
            continue;
        }
        if ((result.status == IQ_ACK_STATUS_ACTIVE) ||
            ((result.flags & IQ_ACK_FLAG_ACTIVE) != 0U))
        {
            sleep_ms(2U);
            continue;
        }

        if ((result.status != IQ_ACK_STATUS_OK) ||
            ((result.flags & ~IQ_ACK_FLAGS_ALL) != 0U) ||
            ((result.flags & IQ_ACK_FLAG_ACTIVE) != 0U) ||
            ((result.flags & IQ_ACK_FLAG_COMPLETE) == 0U) ||
            ((result.flags & (IQ_ACK_FLAG_SEQUENCE_ERROR | IQ_ACK_FLAG_RING_DROP)) != 0U) ||
            (result.packets != expected_packets) ||
            (result.payload_bytes != expected_payload_bytes) ||
            (result.sequence_gaps != 0U) || (result.reordered != 0U) ||
            (result.invalid != 0U) || (result.ring_full_drops != 0U) ||
            (result.ring_oversize_drops != 0U) || (result.ring_free > 4096U) ||
            ((result.flags & (IQ_ACK_FLAG_CRC_PRESENT | IQ_ACK_FLAG_CRC_VALID)) !=
             (IQ_ACK_FLAG_CRC_PRESENT | IQ_ACK_FLAG_CRC_VALID)) ||
            (result.crc_errors != 0U) || (result.crc32c != expected_crc32c) ||
            (result.expected_crc32c != expected_crc32c))
        {
            fprintf(stderr,
                    "ACK rejected session=%" PRIu32 " status=%" PRIu32
                    " flags=0x%08" PRIX32 " packets=%" PRIu32 "/%" PRIu32
                    " bytes=%" PRIu64 "/%" PRIu64 " gaps=%" PRIu32
                    " reorder=%" PRIu32 " invalid=%" PRIu32
                    " ring_drop=%" PRIu32 "/%" PRIu32 " ring_free=%" PRIu32
                    " crc=0x%08" PRIX32 "/0x%08" PRIX32 " errors=%" PRIu32 "\n",
                    session_id, result.status, result.flags,
                    result.packets, expected_packets, result.payload_bytes,
                    expected_payload_bytes, result.sequence_gaps, result.reordered,
                    result.invalid, result.ring_full_drops, result.ring_oversize_drops,
                    result.ring_free, result.crc32c, result.expected_crc32c,
                    result.crc_errors);
            return 0;
        }
        completion_seen = 1;
        if (result.ring_free != 4096U)
        {
            sleep_ms(2U);
            continue;
        }
        printf("ack session=%" PRIu32 " packets=%" PRIu32 " bytes=%" PRIu64
               " ring_free=%" PRIu32 " crc32c=0x%08" PRIX32 "\n",
               session_id, result.packets, result.payload_bytes,
               result.ring_free, result.crc32c);
        return 1;
    }
    fprintf(stderr, "ACK timeout session=%" PRIu32 " request=%" PRIu32
            " after %" PRIu32 " ms\n", session_id, request_id,
            options->ack_timeout_ms);
    if (completion_seen)
    {
        fprintf(stderr, "ACK complete but ring credit did not recover for session=%" PRIu32 "\n",
                session_id);
        return -1;
    }
    return 0;
}

static int iq_ack_wait_ring_drain(socket_handle_t ack_socket,
                                  const options_t *options,
                                  uint32_t session_id,
                                  uint32_t request_id)
{
    uint32_t drain_timeout_ms = options->ack_timeout_ms * 10U;
    uint64_t deadline;
    uint8_t response[sizeof(ra8p1_iq_ack_response_t) + 32U];

    if ((drain_timeout_ms < 100U) ||
        (options->ack_timeout_ms > (UINT32_MAX / 10U)))
    {
        drain_timeout_ms = 100U;
    }
    if (drain_timeout_ms > 5000U)
    {
        drain_timeout_ms = 5000U;
    }
    deadline = monotonic_us() + (uint64_t)drain_timeout_ms * 1000ULL;
    while (monotonic_us() < deadline)
    {
        iq_ack_result_t result;
        int received;
        if (!send_iq_ack_request(ack_socket, request_id, session_id))
        {
            return 0;
        }
        if (!socket_wait_readable(ack_socket, 20U))
        {
            continue;
        }
        received = recv(ack_socket, (char *)response, (int)sizeof(response), 0);
        if ((received <= 0) ||
            !parse_iq_ack_response(response, (uint32_t)received,
                                   request_id, session_id, &result))
        {
            continue;
        }
        if (result.ring_free == 4096U)
        {
            fprintf(stderr, "ACK retry credit ready session=%" PRIu32
                    " ring_free=%" PRIu32 "\n", session_id, result.ring_free);
            return 1;
        }
        if (result.ring_free > 4096U)
        {
            fprintf(stderr, "ACK ring credit invalid session=%" PRIu32
                    " ring_free=%" PRIu32 "\n", session_id, result.ring_free);
            return 0;
        }
        sleep_ms(2U);
    }
    fprintf(stderr, "ACK retry blocked: session=%" PRIu32
            " ring did not fully drain within %" PRIu32 " ms\n",
            session_id, drain_timeout_ms);
    return 0;
}

static int sdr_capture_session(ra8p1_sdr_adapter_runtime_t *runtime,
                               const options_t *options, uint64_t center_hz,
                               uint8_t *rx1_payload)
{
    void *capture_chunk = NULL;
    const size_t chunk_bytes = (size_t) options->capture_chunk_samples *
                               RA8P1_SDR_ADAPTER_SAMPLE_BYTES;
    const size_t rx1_bytes = (size_t) options->samples_per_session *
                             IQ_BYTES_PER_SAMPLE;
    ra8p1_sdr_capture_request_t request;
    uint32_t captured = 0U;
    uint64_t started_us;
    int32_t status;

    if ((runtime->loaded == 0U) || (runtime->api.set_rx == NULL) ||
        (runtime->api.rx_capture == NULL))
    {
        fprintf(stderr, "SDR adapter is not open\n");
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.center_frequency_hz = center_hz;
    request.sample_rate_hz = options->sample_rate_hz;
    request.bandwidth_hz = options->bandwidth_hz;
    request.total_samples = options->samples_per_session;
    request.chunk_samples = options->capture_chunk_samples;
    request.timeout_ms = options->capture_timeout_ms;
    started_us = monotonic_us();
    if ((runtime->api.struct_size >= RA8P1_SDR_ADAPTER_V1_RX1_LE_SIZE) &&
        (runtime->api.rx1_capture_le != NULL))
    {
        status = runtime->api.set_rx(runtime->context, center_hz,
                                     options->sample_rate_hz,
                                     options->bandwidth_hz);
        if (status == 0)
        {
            status = runtime->api.rx1_capture_le(
                runtime->context, rx1_payload, options->samples_per_session,
                options->capture_timeout_ms);
            if (status == 0)
            {
                captured = options->samples_per_session;
            }
        }
    }
    else
    {
        capture_chunk = sdr_aligned_allocate(64U, chunk_bytes);
        if (capture_chunk == NULL)
        {
            fprintf(stderr, "cannot allocate %zu-byte aligned SDR DMA staging buffer\n",
                    chunk_bytes);
            return 0;
        }
        status = ra8p1_sdr_capture_rx1_cached(
            &runtime->api, runtime->context, &request,
            capture_chunk, chunk_bytes, rx1_payload, rx1_bytes, &captured);
        sdr_aligned_free(capture_chunk);
    }
    if (status != 0)
    {
        fprintf(stderr, "SDR capture failed for center=%" PRIu64
                " Hz at sample=%" PRIu32 ": status=%" PRId32 "\n",
                center_hz, captured, status);
        return 0;
    }
    printf("captured center_hz=%" PRIu64 " samples=%" PRIu32
           " bytes=%" PRIu32 " chunks=%" PRIu32 " capture_ms=%" PRIu64 "\n",
           center_hz, captured, captured * IQ_BYTES_PER_SAMPLE,
           (captured + options->capture_chunk_samples - 1U) /
               options->capture_chunk_samples,
           (uint64_t) ((monotonic_us() - started_us) / 1000ULL));
    return captured == options->samples_per_session;
}

static int parse_u32(const char *text, uint32_t *result)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if ((text[0] == '\0') || (end == NULL) || (*end != '\0') || (value > UINT32_MAX))
    {
        return 0;
    }
    *result = (uint32_t) value;
    return 1;
}

static int sender_env_enabled(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL)
    {
        return 0;
    }
    return (strcmp(value, "1") == 0) ||
           (strcmp(value, "true") == 0) ||
           (strcmp(value, "TRUE") == 0) ||
           (strcmp(value, "yes") == 0) ||
           (strcmp(value, "on") == 0);
}

static int official_center_index(uint64_t center_hz, uint32_t *center_index)
{
    uint32_t index;
    for (index = 0U; index < IQ_CENTER_COUNT; index++)
    {
        if (center_hz == ra8p1_sdr_scan_center_hz(index))
        {
            *center_index = index;
            return 1;
        }
    }
    return 0;
}

static int parse_rate_sweep(const char *text, options_t *options)
{
    char copy[192];
    char *item;
    char *next;
    uint32_t count = 0U;
    if (strlen(text) >= sizeof(copy))
    {
        return 0;
    }
    strcpy(copy, text);
    item = copy;
    while (item != NULL)
    {
        uint32_t value;
        uint32_t previous;
        next = strchr(item, ',');
        if (next != NULL)
        {
            *next++ = '\0';
        }
        if ((count >= IQ_RATE_SWEEP_MAX) || !parse_u32(item, &value))
        {
            return 0;
        }
        for (previous = 0U; previous < count; previous++)
        {
            if (options->rate_sweep[previous] == value)
            {
                return 0;
            }
        }
        options->rate_sweep[count++] = value;
        item = next;
    }
    options->rate_count = count;
    return count != 0U;
}

static int parse_centers(const char *text, options_t *options)
{
    char copy[128];
    char *item;
    char *next;
    uint32_t count = 0U;
    if (strlen(text) >= sizeof(copy))
    {
        return 0;
    }
    strcpy(copy, text);
    item = copy;
    while (item != NULL)
    {
        char *end = NULL;
        unsigned long long value;
        uint32_t official_index;
        uint32_t previous;
        next = strchr(item, ',');
        if (next != NULL)
        {
            *next++ = '\0';
        }
        value = strtoull(item, &end, 0);
        if ((item[0] == '\0') || (end == NULL) || (*end != '\0') ||
            (value == 0ULL) || (count >= IQ_CENTER_COUNT) ||
            !official_center_index((uint64_t) value, &official_index))
        {
            return 0;
        }
        for (previous = 0U; previous < count; previous++)
        {
            if (options->center_indices[previous] == official_index)
            {
                return 0;
            }
        }
        options->centers[count++] = (uint64_t) value;
        options->center_indices[count - 1U] = official_index;
        item = next;
    }
    options->center_count = count;
    return count != 0U;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <ra-ip> [--file <rx1_iq.s16>|--stdin|--synthetic|--sdr|--sdr-lib <adapter>] "
            "[--centers 2420000000,2464000000,5760000000,5816000000] "
            "[--sessions N] [--samples 6000000] "
            "[--rate-mbps N|--rate-sweep 80,160,...,800] "
            "[--capture-chunk-samples N] [--capture-timeout-ms N] "
            "[--session-id N] [--sndbuf-bytes N] [--send-batch 1..64] "
            "[--udp-gso|--no-udp-gso] [--udp-no-check] "
            "[--window-crc] [--ack] [--ack-timeout-ms N] [--ack-retries N] "
            "[--dry-run]\n",
            program);
}

static int parse_options(int argc, char **argv, options_t *options)
{
    int index;
    int source_selected = 0;
    int rate_selected = 0;
    const char *adapter_from_environment;
    memset(options, 0, sizeof(*options));
    options->source_mode = SOURCE_STDIN;
    options->sessions = 1U;
    options->samples_per_session = IQ_DEFAULT_SAMPLES_PER_SESSION;
    options->sample_rate_hz = IQ_DEFAULT_SAMPLE_RATE_HZ;
    options->bandwidth_hz = IQ_DEFAULT_BANDWIDTH_HZ;
    /* 320 Mbps leaves measured headroom on the 1 GbE IQSC path while
     * avoiding an unnecessarily bursty open-loop sender default. */
    options->rate_mbps = 320U;
    options->rate_sweep[0] = options->rate_mbps;
    options->rate_count = 1U;
    options->session_id = 1U;
    options->capture_chunk_samples = IQ_SDR_CAPTURE_CHUNK_SAMPLES;
    options->capture_timeout_ms = IQ_SDR_CAPTURE_TIMEOUT_MS;
    options->socket_sndbuf_bytes = IQ_DEFAULT_SOCKET_SNDBUF_BYTES;
    options->send_batch = IQ_SEND_BATCH_DEFAULT;
    options->ack_timeout_ms = IQ_DEFAULT_ACK_TIMEOUT_MS;
    options->ack_retries = IQ_DEFAULT_ACK_RETRIES;
    options->udp_no_check = 0;
    options->udp_gso = sender_env_enabled(IQ_SDR_UDP_GSO_ENV);
    (void) parse_centers("2420000000,2464000000,5760000000,5816000000", options);
    if (argc < 2)
    {
        return 0;
    }
    options->target_ip = argv[1];
    for (index = 2; index < argc; index++)
    {
        const char *argument = argv[index];
        if (strcmp(argument, "--file") == 0 && ++index < argc)
        {
            if (source_selected != 0)
            {
                return 0;
            }
            options->file_path = argv[index];
            options->source_mode = SOURCE_FILE;
            source_selected = 1;
        }
        else if (strcmp(argument, "--stdin") == 0)
        {
            if (source_selected != 0)
            {
                return 0;
            }
            options->source_mode = SOURCE_STDIN;
            options->file_path = NULL;
            source_selected = 1;
        }
        else if (strcmp(argument, "--synthetic") == 0)
        {
            if (source_selected != 0)
            {
                return 0;
            }
            options->source_mode = SOURCE_SYNTHETIC;
            options->file_path = NULL;
            source_selected = 1;
        }
        else if (strcmp(argument, "--sdr-lib") == 0 && ++index < argc &&
                 argv[index][0] != '\0')
        {
            if (source_selected != 0)
            {
                return 0;
            }
            options->sdr_library_path = argv[index];
            options->source_mode = SOURCE_SDR;
            source_selected = 1;
        }
        else if (strcmp(argument, "--sdr") == 0)
        {
            if (source_selected != 0)
            {
                return 0;
            }
            options->sdr_library_path = NULL;
            options->source_mode = SOURCE_SDR;
            source_selected = 1;
        }
        else if (strcmp(argument, "--centers") == 0 && ++index < argc &&
                 parse_centers(argv[index], options))
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--sessions") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->sessions) && options->sessions != 0U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--samples") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->samples_per_session) &&
                 options->samples_per_session != 0U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--rate-mbps") == 0 && ++index < argc &&
                 rate_selected == 0 && parse_u32(argv[index], &options->rate_mbps))
        {
            options->rate_sweep[0] = options->rate_mbps;
            options->rate_count = 1U;
            rate_selected = 1;
        }
        else if (strcmp(argument, "--rate-sweep") == 0 && ++index < argc &&
                 rate_selected == 0 && parse_rate_sweep(argv[index], options))
        {
            rate_selected = 1;
        }
        else if (strcmp(argument, "--session-id") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->session_id) && options->session_id != 0U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--capture-timeout-ms") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->capture_timeout_ms) &&
                 options->capture_timeout_ms != 0U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--sndbuf-bytes") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->socket_sndbuf_bytes) &&
                 options->socket_sndbuf_bytes >= 65536U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--send-batch") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->send_batch) &&
                 options->send_batch >= 1U &&
                 options->send_batch <= IQ_SEND_BATCH_MAX)
        {
            /* Bound the microburst independently of average pacing. */
        }
        else if (strcmp(argument, "--udp-no-check") == 0)
        {
            options->udp_no_check = 1;
        }
        else if (strcmp(argument, "--udp-gso") == 0)
        {
            options->udp_gso = 1;
        }
        else if (strcmp(argument, "--no-udp-gso") == 0)
        {
            options->udp_gso = 0;
        }
        else if (strcmp(argument, "--window-crc") == 0)
        {
            options->window_crc = 1;
        }
        else if (strcmp(argument, "--ack") == 0)
        {
            options->ack_enabled = 1;
        }
        else if (strcmp(argument, "--ack-timeout-ms") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->ack_timeout_ms) &&
                 options->ack_timeout_ms != 0U && options->ack_timeout_ms <= 60000U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--ack-retries") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->ack_retries) &&
                 options->ack_retries <= 16U)
        {
            /* Parsed. */
        }
        else if (strcmp(argument, "--capture-chunk-samples") == 0 && ++index < argc &&
                 parse_u32(argv[index], &options->capture_chunk_samples) &&
                 options->capture_chunk_samples != 0U &&
                 options->capture_chunk_samples <= options->samples_per_session)
        {
            /* Formal default is one complete 6M-sample DMA capture. */
        }
        else if (strcmp(argument, "--dry-run") == 0)
        {
            options->dry_run = 1;
        }
        else
        {
            return 0;
        }
    }
    if (source_selected == 0)
    {
        adapter_from_environment = getenv(IQ_SDR_ADAPTER_ENV);
        if ((adapter_from_environment != NULL) && (adapter_from_environment[0] != '\0'))
        {
            options->sdr_library_path = adapter_from_environment;
            options->source_mode = SOURCE_SDR;
        }
    }
    if ((options->sample_rate_hz != IQ_DEFAULT_SAMPLE_RATE_HZ) ||
        (options->bandwidth_hz != IQ_DEFAULT_BANDWIDTH_HZ) ||
        ((options->samples_per_session != IQ_DEFAULT_SAMPLES_PER_SESSION) &&
         (options->samples_per_session != IQ_MODEL_WINDOW_SAMPLES)))
    {
        fprintf(stderr,
                "formal sender requires 60 MSPS, 56 MHz, and either "
                "590,336 or 6,000,000 samples/session\n");
        return 0;
    }
    if (options->ack_enabled != 0)
    {
        options->window_crc = 1;
    }
    return 1;
}

static void configure_udp_socket(socket_handle_t socket_fd, const options_t *options)
{
    if ((socket_fd == INVALID_SOCKET_HANDLE) || (options == NULL))
    {
        return;
    }
#ifdef SO_SNDBUF
    {
        int requested = (options->socket_sndbuf_bytes > (uint32_t)INT32_MAX) ?
                        INT32_MAX : (int)options->socket_sndbuf_bytes;
#ifdef _WIN32
        const int option_length = (int)sizeof(requested);
#else
        const socklen_t option_length = (socklen_t)sizeof(requested);
#endif
        if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF,
                       (const char *)&requested, option_length) != 0)
        {
            fprintf(stderr, "warning: SO_SNDBUF=%d was rejected\n", requested);
        }
    }
#endif
#if defined(SO_NO_CHECK) && !defined(_WIN32)
    if (options->udp_no_check != 0)
    {
        int no_check = 1;
        if (setsockopt(socket_fd, SOL_SOCKET, SO_NO_CHECK,
                       &no_check, sizeof(no_check)) != 0)
        {
            fprintf(stderr, "warning: SO_NO_CHECK was rejected; UDP checksum remains enabled\n");
        }
        else
        {
            fprintf(stderr, "UDP checksum disabled by --udp-no-check (controlled link only)\n");
        }
    }
#else
    if (options->udp_no_check != 0)
    {
        fprintf(stderr, "warning: --udp-no-check is unavailable on this platform\n");
    }
#endif
}

static void make_header(uint8_t *packet, uint32_t sequence, uint32_t length,
                        uint32_t flags, uint64_t sample_index, uint32_t session_id)
{
    put_u32(packet, 0U, IQ_MAGIC);
    put_u32(packet, 4U, sequence);
    put_u32(packet, 8U, length);
    put_u32(packet, 12U, flags);
    put_u64(packet, 16U, sample_index);
    put_u32(packet, 24U, session_id);
    put_u32(packet, 28U, IQ_FORMAT_S16);
}

static void make_config(uint8_t *config, const options_t *options, uint32_t session_id,
                        uint64_t center_hz, uint32_t center_index, uint32_t flags)
{
    memset(config, 0, IQSC_BYTES);
    put_u32(config, 0U, IQSC_MAGIC);
    put_u16(config, 4U, IQSC_VERSION);
    put_u16(config, 6U, IQSC_BYTES);
    put_u32(config, 8U, session_id);
    put_u32(config, 12U, options->sample_rate_hz);
    put_u32(config, 16U, options->sample_rate_hz);
    put_u64(config, 20U, center_hz);
    put_u32(config, 28U, options->bandwidth_hz);
    put_u32(config, 32U, IQ_MODEL_WINDOW_SAMPLES);
    put_u32(config, 36U, options->samples_per_session);
    put_u32(config, 40U, 0U);
    put_u32(config, 44U, IQ_MODEL_STRIDE_SAMPLES);
    put_u32(config, 48U, IQ_FORMAT_S16);
    put_u32(config, 52U, 12U);
    put_u32(config, 56U, 1U); /* RX1 only. */
    put_u32(config, 60U, flags);
    put_u32(config, 64U, center_index);
}

static int read_payload(FILE *input, uint8_t *payload, uint32_t wanted, uint32_t *actual)
{
    size_t got = fread(payload, 1U, wanted, input);
    if ((got % IQ_BYTES_PER_SAMPLE) != 0U)
    {
        fprintf(stderr, "input ends with a non-IQ byte tail\n");
        return 0;
    }
    *actual = (uint32_t) got;
    return 1;
}

static void fill_synthetic(uint8_t *payload, uint32_t bytes, uint64_t sample_index)
{
    uint32_t offset;
    for (offset = 0U; offset < bytes; offset += IQ_BYTES_PER_SAMPLE)
    {
        uint32_t n = (uint32_t)(sample_index + offset / IQ_BYTES_PER_SAMPLE);
        put_s16(payload, offset, (int16_t) (((n * 97U) & 0x0FFFU) - 2048));
        put_s16(payload, offset + 2U, (int16_t) (((n * 193U) & 0x0FFFU) - 2048));
    }
}

static int load_payload(const options_t *options,
                        FILE *input,
                        const uint8_t *cached_rx1,
                        uint64_t byte_offset,
                        uint64_t sample_index,
                        uint8_t *payload,
                        uint32_t wanted)
{
    uint32_t actual = wanted;
    if (options->source_mode == SOURCE_SYNTHETIC)
    {
        fill_synthetic(payload, actual, sample_index);
    }
    else if (options->source_mode == SOURCE_SDR)
    {
        memcpy(payload, cached_rx1 + byte_offset, actual);
    }
    else if (!read_payload(input, payload, wanted, &actual) || actual != wanted)
    {
        return 0;
    }
    return 1;
}

static int send_packet(socket_handle_t socket_fd, const struct sockaddr_in *peer,
                       const uint8_t *packet, uint32_t length, int dry_run)
{
    (void)peer;
    if (dry_run)
    {
        return 1;
    }
    if (send(socket_fd, (const char *) packet, (int) length, 0) != (int) length)
    {
        perror("send");
        return 0;
    }
    return 1;
}

#if defined(__linux__)
typedef enum e_udp_gso_send_status
{
    UDP_GSO_SEND_FAILED = -1,
    UDP_GSO_SEND_NOT_ELIGIBLE = 0,
    UDP_GSO_SEND_COMPLETE = 1,
    UDP_GSO_SEND_UNSUPPORTED = 2
} udp_gso_send_status_t;

static int udp_gso_fallback_error(int error_code)
{
    return (error_code == EINVAL) ||
           (error_code == ENOPROTOOPT) ||
           (error_code == EOPNOTSUPP) ||
           (error_code == ENOSYS) ||
           (error_code == EMSGSIZE);
}

/*
 * UDP_SEGMENT treats the concatenated sendmsg iovec stream as a sequence of
 * fixed-size UDP payloads.  Keeping each IQSC header adjacent to its IQ data
 * in that stream preserves the existing per-datagram sequence/session fields
 * without copying a completed SDR window into another 2.4 MB cache.
 */
static udp_gso_send_status_t udp_gso_send_batch(
    socket_handle_t socket_fd,
    const struct mmsghdr *messages,
    uint32_t message_count)
{
    struct iovec gso_iov[IQ_SEND_BATCH_MAX * 2U];
    union
    {
        unsigned char bytes[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr alignment;
    } control;
    struct msghdr gso_message;
    struct cmsghdr *control_message;
    uint16_t segment_bytes = (uint16_t)IQ_UDP_BYTES;
    size_t iov_count = 0U;
    size_t expected_bytes = 0U;
    uint32_t message_index;
    ssize_t sent_bytes;

    if ((messages == NULL) || (message_count < 2U) ||
        (message_count > IQ_UDP_GSO_MAX_BATCH))
    {
        return UDP_GSO_SEND_NOT_ELIGIBLE;
    }
    for (message_index = 0U; message_index < message_count; ++message_index)
    {
        const struct msghdr *source = &messages[message_index].msg_hdr;
        size_t packet_bytes = 0U;
        size_t source_iov;
        if ((source->msg_iov == NULL) || (source->msg_iovlen == 0U) ||
            ((iov_count + source->msg_iovlen) >
             (sizeof(gso_iov) / sizeof(gso_iov[0]))))
        {
            return UDP_GSO_SEND_NOT_ELIGIBLE;
        }
        for (source_iov = 0U; source_iov < source->msg_iovlen; ++source_iov)
        {
            if ((source->msg_iov[source_iov].iov_base == NULL) ||
                (source->msg_iov[source_iov].iov_len == 0U))
            {
                return UDP_GSO_SEND_NOT_ELIGIBLE;
            }
            packet_bytes += source->msg_iov[source_iov].iov_len;
            gso_iov[iov_count++] = source->msg_iov[source_iov];
        }
        if (packet_bytes != IQ_UDP_BYTES)
        {
            /* Keep the short final IQ packet on the well-tested sendmmsg
             * path.  This avoids depending on kernel-specific short-tail
             * UDP_SEGMENT behavior. */
            return UDP_GSO_SEND_NOT_ELIGIBLE;
        }
        expected_bytes += packet_bytes;
    }

    memset(&gso_message, 0, sizeof(gso_message));
    memset(&control, 0, sizeof(control));
    gso_message.msg_iov = gso_iov;
    gso_message.msg_iovlen = iov_count;
    gso_message.msg_control = control.bytes;
    gso_message.msg_controllen = sizeof(control);
    control_message = CMSG_FIRSTHDR(&gso_message);
    if (control_message == NULL)
    {
        return UDP_GSO_SEND_NOT_ELIGIBLE;
    }
    control_message->cmsg_level = SOL_UDP;
    control_message->cmsg_type = UDP_SEGMENT;
    control_message->cmsg_len = CMSG_LEN(sizeof(segment_bytes));
    memcpy(CMSG_DATA(control_message), &segment_bytes, sizeof(segment_bytes));

    do
    {
        sent_bytes = sendmsg(socket_fd, &gso_message, 0);
    } while ((sent_bytes < 0) && (errno == EINTR));
    if (sent_bytes == (ssize_t)expected_bytes)
    {
        return UDP_GSO_SEND_COMPLETE;
    }
    if ((sent_bytes < 0) && udp_gso_fallback_error(errno))
    {
        return UDP_GSO_SEND_UNSUPPORTED;
    }
    if (sent_bytes < 0)
    {
        perror("sendmsg UDP_SEGMENT");
    }
    else
    {
        fprintf(stderr,
                "sendmsg UDP_SEGMENT returned %zd of %zu bytes\n",
                sent_bytes, expected_bytes);
    }
    return UDP_GSO_SEND_FAILED;
}
#endif

static void payload_pacer_init(payload_pacer_t *pacer, uint32_t rate_mbps,
                               uint64_t now_us)
{
    memset(pacer, 0, sizeof(*pacer));
    pacer->rate_mbps = rate_mbps;
    pacer->epoch_us = now_us;
    pacer->deadline_us = now_us;
}

static void payload_pacer_rebase_if_late(payload_pacer_t *pacer, uint64_t now_us)
{
    uint64_t late_us;
    if ((pacer->rate_mbps == 0U) || (now_us <= pacer->deadline_us))
    {
        return;
    }
    late_us = now_us - pacer->deadline_us;
    if (late_us > pacer->max_late_us)
    {
        pacer->max_late_us = late_us;
    }
    if (late_us <= IQ_PACER_REBASE_LATE_US)
    {
        return;
    }
    pacer->late_rebases++;
    pacer->epoch_us = now_us;
    pacer->deadline_us = now_us;
    pacer->paced_bits = 0U;
}

static void payload_pacer_wait_before_send(payload_pacer_t *pacer)
{
    uint64_t now_us;
    if (pacer->rate_mbps == 0U)
    {
        return;
    }
    do
    {
        now_us = monotonic_us();
    } while (now_us < pacer->deadline_us);
    payload_pacer_rebase_if_late(pacer, now_us);
}

static void payload_pacer_commit(payload_pacer_t *pacer, uint64_t payload_bytes)
{
    uint64_t target_us;
    if (pacer->rate_mbps == 0U)
    {
        return;
    }
    pacer->paced_bits += payload_bytes * 8ULL;
    target_us = (pacer->paced_bits + pacer->rate_mbps - 1ULL) /
                pacer->rate_mbps;
    pacer->deadline_us = pacer->epoch_us + target_us;
}

static void payload_pacer_finish(const payload_pacer_t *pacer)
{
    if (pacer->rate_mbps == 0U)
    {
        return;
    }
    while (monotonic_us() < pacer->deadline_us)
    {
        /* Account for the final payload's wire-time budget before END. */
    }
}

typedef struct st_send_session_result
{
    uint32_t data_packets;
    uint32_t logical_udp_packets;
    uint64_t payload_bytes;
    uint64_t elapsed_us;
    uint32_t crc32c;
    uint32_t crc_backend;
    uint32_t crc_timing_enabled;
    uint64_t crc_cpu_us;
    uint32_t pacing_rebases;
    uint64_t pacing_max_late_us;
    uint32_t udp_gso_requested;
    uint32_t udp_gso_batches;
    uint32_t udp_gso_packets;
    uint32_t udp_gso_fallbacks;
    uint32_t udp_gso_ineligible_batches;
    uint32_t udp_gso_attempts;
    uint32_t udp_gso_last_errno;
} send_session_result_t;

/*
 * A completed SDR DMA window is immutable until its WINDOW_ACK.  The capture
 * worker can therefore calculate this once while the previous window is on
 * the wire / being analysed, then the sender can publish START immediately
 * after CREDIT.  Keep the accumulator (rather than only the final CRC) so
 * the normal END construction and fault-injection paths remain identical.
 */
typedef struct st_send_crc_precompute
{
    uint32_t valid;
    uint32_t accumulator;
    uint32_t backend;
    uint32_t timing_enabled;
    uint32_t payload_bytes;
    uint64_t cpu_us;
} send_crc_precompute_t;

#if defined(__GNUC__)
#define SDR_STREAM_MAYBE_UNUSED __attribute__((unused))
#else
#define SDR_STREAM_MAYBE_UNUSED
#endif

static int SDR_STREAM_MAYBE_UNUSED send_crc_precompute_window(const uint8_t *payload,
                                      uint32_t payload_bytes,
                                      send_crc_precompute_t *precompute)
{
    sdr_crc_backend_t backend;
    uint32_t timing_enabled;

    if ((payload == NULL) || (payload_bytes == 0U) ||
        (precompute == NULL))
    {
        return 0;
    }
    memset(precompute, 0, sizeof(*precompute));
    backend = crc32c_backend_selected();
    timing_enabled = crc32c_timing_enabled();
    precompute->accumulator = crc32c_update_timed(
        0xFFFFFFFFU, payload, payload_bytes, backend, timing_enabled,
        &precompute->cpu_us);
    precompute->backend = (uint32_t)backend;
    precompute->timing_enabled = timing_enabled;
    precompute->payload_bytes = payload_bytes;
    precompute->valid = 1U;
    return 1;
}

static int send_session_once(socket_handle_t socket_fd, const struct sockaddr_in *peer,
                             const options_t *options, uint32_t session_id,
                             uint64_t center_hz, uint32_t center_index,
                             FILE *stdin_stream, const uint8_t *cached_rx1,
                             const uint8_t *retry_cache,
                             const send_crc_precompute_t *crc_precompute,
                             send_session_result_t *session_result)
{
    uint8_t packet[IQ_UDP_BYTES];
    uint8_t config[IQSC_BYTES];
    FILE *input = stdin_stream;
    uint32_t sequence = 0U;
    uint64_t sample_index = 0U;
    uint64_t payload_bytes = 0U;
    uint64_t started_us;
    payload_pacer_t pacer;
    uint32_t crc_accumulator = 0xFFFFFFFFU;
    sdr_crc_backend_t crc_backend = crc32c_backend_selected();
    uint32_t crc_timing = crc32c_timing_enabled();
    uint64_t crc_cpu_us = 0ULL;
    uint32_t crc_precomputed = 0U;
    uint32_t data_packets = 0U;
    uint32_t udp_gso_batches = 0U;
    uint32_t udp_gso_packets = 0U;
    uint32_t udp_gso_fallbacks = 0U;
    uint32_t udp_gso_ineligible_batches = 0U;
    uint32_t udp_gso_attempts = 0U;
    uint32_t udp_gso_last_errno = 0U;
    uint32_t expected_bytes = options->samples_per_session * IQ_BYTES_PER_SAMPLE;
    uint32_t config_flags = IQ_FLAG_VALID_BITS_12;
#if defined(__linux__)
    int udp_gso_enabled = options->udp_gso;
    uint8_t batch_packets[IQ_SEND_BATCH_MAX][IQ_UDP_BYTES];
    uint8_t batch_headers[IQ_SEND_BATCH_MAX][IQ_HEADER_BYTES];
    struct iovec batch_iov[IQ_SEND_BATCH_MAX][2];
    struct mmsghdr batch_messages[IQ_SEND_BATCH_MAX];
#endif

    if ((options->source_mode == SOURCE_FILE) && (retry_cache == NULL))
    {
        input = fopen(options->file_path, "rb");
        if (input == NULL)
        {
            perror(options->file_path);
            return 0;
        }
    }
    if (options->source_mode == SOURCE_SYNTHETIC)
    {
        config_flags |= IQ_FLAG_SYNTHETIC;
    }
    if (options->window_crc != 0)
    {
        config_flags |= IQ_FLAG_WINDOW_CRC;
    }
    if ((options->source_mode == SOURCE_SDR) && (cached_rx1 == NULL))
    {
        fprintf(stderr, "SDR source has no completed RX1 cache\n");
        goto failed;
    }
    if ((options->window_crc != 0) && (crc_precompute != NULL) &&
        (crc_precompute->valid != 0U) &&
        (crc_precompute->payload_bytes == expected_bytes) &&
        (crc_precompute->backend <= (uint32_t)SDR_CRC_BACKEND_SLICE8))
    {
        crc_accumulator = crc_precompute->accumulator;
        crc_backend = (sdr_crc_backend_t)crc_precompute->backend;
        crc_timing = crc_precompute->timing_enabled;
        crc_cpu_us = crc_precompute->cpu_us;
        crc_precomputed = 1U;
    }
    if (options->window_crc != 0)
    {
        const uint8_t *immutable_cache = (retry_cache != NULL) ?
            retry_cache :
            ((options->source_mode == SOURCE_SDR) ? cached_rx1 : NULL);
        if ((crc_precomputed == 0U) && (immutable_cache != NULL))
        {
            /* The completed SDR/retry window is immutable until ACK.  Hash it
             * before START so CRC work cannot consume the per-GSO-batch wire
             * budget or inflate first-to-last packet time. */
            crc_accumulator = crc32c_update_timed(
                crc_accumulator, immutable_cache, expected_bytes,
                crc_backend, crc_timing, &crc_cpu_us);
            crc_precomputed = 1U;
        }
    }
    make_config(config, options, session_id, center_hz, center_index, config_flags);
    make_header(packet, sequence++, IQSC_BYTES, config_flags | IQ_FLAG_STREAM_START, 0U, session_id);
    memcpy(&packet[IQ_HEADER_BYTES], config, IQSC_BYTES);
    if (!send_packet(socket_fd, peer, packet, IQ_HEADER_BYTES + IQSC_BYTES, options->dry_run))
    {
        goto failed;
    }
    started_us = monotonic_us();
    payload_pacer_init(&pacer, options->rate_mbps, started_us);
    while (payload_bytes < expected_bytes)
    {
#if defined(__linux__)
        uint64_t batch_payload_bytes = 0U;
        uint32_t batch_count = 0U;
        uint32_t sent_count = 0U;
        while ((batch_count < options->send_batch) &&
               ((payload_bytes + batch_payload_bytes) < expected_bytes))
        {
            const uint64_t byte_offset = payload_bytes + batch_payload_bytes;
            uint32_t wanted = (uint32_t)((expected_bytes - byte_offset) > IQ_DATA_BYTES ?
                                         IQ_DATA_BYTES : (expected_bytes - byte_offset));
            const uint8_t *payload;
            const uint32_t packet_sequence = sequence++;
            const int drop_packet =
                (options->test_drop_data_packet != 0) &&
                (data_packets == 0U);
            if (retry_cache != NULL)
            {
                payload = &retry_cache[byte_offset];
            }
            else if (options->source_mode == SOURCE_SDR)
            {
                /* The SDR cache is immutable for the duration of this batch.
                 * Point the second iovec at it and avoid a 2.4 MB/session
                 * memcpy on the Zynq sender. */
                payload = &cached_rx1[byte_offset];
            }
            else
            {
                payload = &batch_packets[batch_count][IQ_HEADER_BYTES];
                if (!load_payload(options, input, cached_rx1, byte_offset, sample_index,
                                  (uint8_t *)payload, wanted))
                {
                    fprintf(stderr,
                            "session %" PRIu32 " requires exactly %" PRIu32
                            " S16 IQ bytes\n",
                            session_id, expected_bytes);
                    goto failed;
                }
            }
            if (((options->source_mode == SOURCE_SDR) || (retry_cache != NULL)) &&
                ((byte_offset + wanted) > ((uint64_t)options->samples_per_session *
                                           IQ_BYTES_PER_SAMPLE)))
            {
                fprintf(stderr, "session %" PRIu32 " SDR cache range is invalid\n", session_id);
                goto failed;
            }
            if (drop_packet)
            {
                /* The datagram is absent from the wire, but it remains part of
                 * the logical window and therefore advances sequence, sample
                 * position, byte count, and the end-to-end CRC. */
                crc_accumulator = ((options->window_crc != 0) &&
                                   (crc_precomputed == 0U)) ?
                    crc32c_update_timed(crc_accumulator, payload, wanted,
                                        crc_backend, crc_timing,
                                        &crc_cpu_us) : crc_accumulator;
                data_packets++;
                batch_payload_bytes += wanted;
                sample_index += wanted / IQ_BYTES_PER_SAMPLE;
                continue;
            }
            memset(&batch_messages[batch_count], 0, sizeof(batch_messages[batch_count]));
            if (retry_cache != NULL)
            {
                /* The retry cache is intentionally copied into the same
                 * contiguous packet layout used by the file path.  A
                 * two-iovec sendmmsg retry can outlive the cache's producer
                 * window on the Zynq image and was observed to produce a
                 * receiver CRC mismatch despite matching byte counts. */
                memcpy(&batch_packets[batch_count][IQ_HEADER_BYTES],
                       payload, wanted);
                make_header(batch_packets[batch_count], packet_sequence, wanted,
                            config_flags, sample_index, session_id);
                batch_iov[batch_count][0].iov_base = batch_packets[batch_count];
                batch_iov[batch_count][0].iov_len = IQ_HEADER_BYTES + wanted;
                batch_messages[batch_count].msg_hdr.msg_iov = batch_iov[batch_count];
                batch_messages[batch_count].msg_hdr.msg_iovlen = 1U;
            }
            else if (options->source_mode == SOURCE_SDR)
            {
                make_header(batch_headers[batch_count], packet_sequence, wanted, config_flags,
                            sample_index, session_id);
                batch_iov[batch_count][0].iov_base = batch_headers[batch_count];
                batch_iov[batch_count][0].iov_len = IQ_HEADER_BYTES;
                batch_iov[batch_count][1].iov_base = (void *)payload;
                batch_iov[batch_count][1].iov_len = wanted;
                batch_messages[batch_count].msg_hdr.msg_iov = batch_iov[batch_count];
                batch_messages[batch_count].msg_hdr.msg_iovlen = 2U;
            }
            else
            {
                make_header(batch_packets[batch_count], packet_sequence, wanted, config_flags,
                            sample_index, session_id);
                batch_iov[batch_count][0].iov_base = batch_packets[batch_count];
                batch_iov[batch_count][0].iov_len = IQ_HEADER_BYTES + wanted;
                batch_messages[batch_count].msg_hdr.msg_iov = batch_iov[batch_count];
                batch_messages[batch_count].msg_hdr.msg_iovlen = 1U;
            }
            crc_accumulator = ((options->window_crc != 0) &&
                               (crc_precomputed == 0U)) ?
                crc32c_update_timed(crc_accumulator, payload, wanted,
                                    crc_backend, crc_timing,
                                    &crc_cpu_us) : crc_accumulator;
            data_packets++;
            batch_payload_bytes += wanted;
            sample_index += wanted / IQ_BYTES_PER_SAMPLE;
            batch_count++;
        }
        /* Pace immediately before the syscall.  If construction or a
         * scheduler stall overshot the deadline, the pacer re-anchors here;
         * it never emits old deadlines as a catch-up microburst. */
        payload_pacer_wait_before_send(&pacer);
        if (!options->dry_run)
        {
            sent_count = 0U;
#if defined(__linux__)
            if (udp_gso_enabled && (batch_count >= 2U))
            {
                udp_gso_send_status_t gso_status;
                ++udp_gso_attempts;
                gso_status = udp_gso_send_batch(socket_fd, batch_messages,
                                                batch_count);
                if (gso_status == UDP_GSO_SEND_COMPLETE)
                {
                    sent_count = batch_count;
                    ++udp_gso_batches;
                    udp_gso_packets += batch_count;
                }
                else if (gso_status == UDP_GSO_SEND_UNSUPPORTED)
                {
                    udp_gso_enabled = 0;
                    ++udp_gso_fallbacks;
                    udp_gso_last_errno = (uint32_t)errno;
                }
                else if (gso_status == UDP_GSO_SEND_NOT_ELIGIBLE)
                {
                    ++udp_gso_ineligible_batches;
                }
                else if (gso_status == UDP_GSO_SEND_FAILED)
                {
                    goto failed;
                }
            }
#endif
            while (sent_count < batch_count)
            {
                int sent = sendmmsg(socket_fd,
                                    &batch_messages[sent_count],
                                    batch_count - sent_count,
                                    0);
                if ((sent < 0) && (errno == EINTR))
                {
                    continue;
                }
                if (sent <= 0)
                {
                    perror("sendmmsg");
                    goto failed;
                }
                sent_count += (uint32_t)sent;
            }
        }
        payload_bytes += batch_payload_bytes;
        payload_pacer_commit(&pacer, batch_payload_bytes);
#else
        uint32_t wanted = (uint32_t)((expected_bytes - payload_bytes) > IQ_DATA_BYTES ?
                                     IQ_DATA_BYTES : (expected_bytes - payload_bytes));
        const int drop_packet =
            (options->test_drop_data_packet != 0) &&
            (data_packets == 0U);
        if (retry_cache != NULL)
        {
            memcpy(&packet[IQ_HEADER_BYTES], retry_cache + payload_bytes, wanted);
        }
        else if (!load_payload(options, input, cached_rx1, payload_bytes, sample_index,
                               &packet[IQ_HEADER_BYTES], wanted))
        {
            fprintf(stderr, "session %" PRIu32 " requires exactly %" PRIu32 " S16 IQ bytes\n",
                    session_id, expected_bytes);
            goto failed;
        }
        make_header(packet, sequence++, wanted, config_flags, sample_index, session_id);
        payload_pacer_wait_before_send(&pacer);
        if (!drop_packet &&
            !send_packet(socket_fd, peer, packet,
                         IQ_HEADER_BYTES + wanted, options->dry_run))
        {
            goto failed;
        }
        if ((options->window_crc != 0) && (crc_precomputed == 0U))
        {
            crc_accumulator = crc32c_update_timed(
                crc_accumulator, &packet[IQ_HEADER_BYTES], wanted,
                crc_backend, crc_timing, &crc_cpu_us);
        }
        data_packets++;
        payload_bytes += wanted;
        sample_index += wanted / IQ_BYTES_PER_SAMPLE;
        payload_pacer_commit(&pacer, wanted);
#endif
    }
    payload_pacer_finish(&pacer);
    if ((options->source_mode == SOURCE_FILE) && (retry_cache == NULL) &&
        (fgetc(input) != EOF || ferror(input) != 0))
    {
        fprintf(stderr, "session %" PRIu32 " input contains bytes after the formal 6,000,000-sample capture\n",
                session_id);
        goto failed;
    }
    {
        const uint32_t end_bytes = IQSC_BYTES +
            ((options->window_crc != 0) ? (uint32_t)sizeof(uint32_t) : 0U);
        const uint32_t final_crc = (options->window_crc != 0) ?
            (crc_accumulator ^ 0xFFFFFFFFU) : 0U;
        const uint32_t transmitted_crc =
            ((options->window_crc != 0) &&
             (options->test_corrupt_end_crc != 0)) ?
            (final_crc ^ 1U) : final_crc;
        make_header(packet, sequence++, end_bytes,
                    config_flags | IQ_FLAG_STREAM_END,
                sample_index, session_id);
        memcpy(&packet[IQ_HEADER_BYTES], config, IQSC_BYTES);
        if (options->window_crc != 0)
        {
            put_u32(packet, IQ_HEADER_BYTES + IQSC_BYTES, transmitted_crc);
        }
        if (!send_packet(socket_fd, peer, packet,
                         IQ_HEADER_BYTES + end_bytes, options->dry_run))
        {
            goto failed;
        }
        if (session_result != NULL)
        {
            session_result->data_packets = data_packets;
            session_result->logical_udp_packets = data_packets + 2U;
            session_result->payload_bytes = payload_bytes;
            session_result->elapsed_us = monotonic_us() - started_us;
            session_result->crc32c = transmitted_crc;
            session_result->crc_backend = (uint32_t)crc_backend;
            session_result->crc_timing_enabled = crc_timing;
            session_result->crc_cpu_us = crc_cpu_us;
            session_result->pacing_rebases = pacer.late_rebases;
            session_result->pacing_max_late_us = pacer.max_late_us;
            session_result->udp_gso_requested = (uint32_t)(options->udp_gso != 0);
            session_result->udp_gso_batches = udp_gso_batches;
            session_result->udp_gso_packets = udp_gso_packets;
            session_result->udp_gso_fallbacks = udp_gso_fallbacks;
            session_result->udp_gso_ineligible_batches =
                udp_gso_ineligible_batches;
            session_result->udp_gso_attempts = udp_gso_attempts;
            session_result->udp_gso_last_errno = udp_gso_last_errno;
        }
    }
    if ((options->source_mode == SOURCE_FILE) && (retry_cache == NULL))
    {
        fclose(input);
    }
    return 1;

failed:
    if ((options->source_mode == SOURCE_FILE) && (retry_cache == NULL) &&
        (input != NULL))
    {
        fclose(input);
    }
    return 0;
}

static int prepare_retry_cache(const options_t *options, FILE *stdin_stream,
                               uint8_t **retry_cache)
{
    FILE *input = stdin_stream;
    uint8_t *cache;
    size_t expected_bytes;
    size_t offset = 0U;

    if ((options == NULL) || (retry_cache == NULL))
    {
        return 0;
    }
    *retry_cache = NULL;
    if ((options->source_mode == SOURCE_FILE) ||
        (options->source_mode == SOURCE_SDR) ||
        (options->source_mode == SOURCE_SYNTHETIC))
    {
        /* A named file is already a durable whole-window cache. Reopen it on
         * each attempt so the proven contiguous file packet path is reused;
         * only non-seekable stdin needs a second in-memory retry cache. */
        return 1;
    }
    expected_bytes = (size_t)options->samples_per_session * IQ_BYTES_PER_SAMPLE;
    cache = (uint8_t *)sdr_aligned_allocate(64U, expected_bytes);
    if (cache == NULL)
    {
        fprintf(stderr, "cannot allocate %zu-byte ACK retry cache\n", expected_bytes);
        return 0;
    }
    if (options->source_mode == SOURCE_FILE)
    {
        input = fopen(options->file_path, "rb");
        if (input == NULL)
        {
            perror(options->file_path);
            sdr_aligned_free(cache);
            return 0;
        }
    }
    while (offset < expected_bytes)
    {
        size_t got = fread(cache + offset, 1U, expected_bytes - offset, input);
        if (got == 0U)
        {
            fprintf(stderr, "ACK retry cache requires exactly %zu IQ bytes\n",
                    expected_bytes);
            if (options->source_mode == SOURCE_FILE) { fclose(input); }
            sdr_aligned_free(cache);
            return 0;
        }
        offset += got;
    }
    if ((fgetc(input) != EOF) || (ferror(input) != 0))
    {
        fprintf(stderr, "ACK retry cache input contains bytes after the session\n");
        if (options->source_mode == SOURCE_FILE) { fclose(input); }
        sdr_aligned_free(cache);
        return 0;
    }
    if (options->source_mode == SOURCE_FILE) { fclose(input); }
    *retry_cache = cache;
    return 1;
}

static int send_session(socket_handle_t socket_fd,
                        socket_handle_t ack_socket,
                        const struct sockaddr_in *peer,
                        const options_t *options, uint32_t session_id,
                        uint64_t center_hz, uint32_t center_index,
                        FILE *stdin_stream, const uint8_t *cached_rx1)
{
    uint8_t *retry_cache = NULL;
    send_session_result_t session_result;
    uint32_t attempt;
    const uint32_t expected_bytes = options->samples_per_session *
                                    IQ_BYTES_PER_SAMPLE;
    const uint32_t expected_packets =
        (expected_bytes + IQ_DATA_BYTES - 1U) / IQ_DATA_BYTES;

    if ((options->ack_enabled != 0) && (options->dry_run == 0) &&
        !prepare_retry_cache(options, stdin_stream, &retry_cache))
    {
        return 0;
    }
    for (attempt = 0U; attempt <= options->ack_retries; attempt++)
    {
        memset(&session_result, 0, sizeof(session_result));
        if (!send_session_once(socket_fd, peer, options, session_id,
                               center_hz, center_index, stdin_stream,
                               cached_rx1, retry_cache, NULL,
                               &session_result))
        {
            if (retry_cache != NULL) { sdr_aligned_free(retry_cache); }
            return 0;
        }
        if ((options->ack_enabled == 0) || (options->dry_run != 0))
        {
            break;
        }
        {
            uint32_t request_id = 0xA8C30000U ^ session_id ^ (attempt + 1U);
            int ack_status = iq_ack_is_complete(
                ack_socket, options, session_id, request_id,
                expected_packets, expected_bytes, session_result.crc32c);
            if (ack_status > 0)
            {
                break;
            }
            if (ack_status < 0)
            {
                if (retry_cache != NULL) { sdr_aligned_free(retry_cache); }
                return 0;
            }
            if (attempt == options->ack_retries)
            {
                fprintf(stderr, "session=%" PRIu32 " exhausted ACK retries\n",
                        session_id);
                if (retry_cache != NULL) { sdr_aligned_free(retry_cache); }
                return 0;
            }
            /* A retry reuses the same session identity.  Do not inject a new
             * START while any packets from the failed attempt remain queued;
             * without a valid ACK this drain query is also the only safe
             * recovery path. */
            if (!iq_ack_wait_ring_drain(ack_socket, options, session_id, request_id))
            {
                fprintf(stderr, "session=%" PRIu32 " cannot safely retry before ring drain\n",
                        session_id);
                if (retry_cache != NULL) { sdr_aligned_free(retry_cache); }
                return 0;
            }
            fprintf(stderr, "retrying session=%" PRIu32
                    " attempt=%" PRIu32 "/%" PRIu32 "\n",
                    session_id, attempt + 2U, options->ack_retries + 1U);
        }
    }
    printf("%s session=%" PRIu32 " center_index=%" PRIu32 " center_hz=%" PRIu64
           " samples=%" PRIu32 " data_packets=%" PRIu32 " udp_packets=%" PRIu32
           " attempts=%" PRIu32 " target_mbps=%" PRIu32
           " send_batch=%" PRIu32
            " payload_mbps_x1000=%" PRIu64 " tiles=%" PRIu32 " stride=%u"
            " pacing_rebases=%" PRIu32 " pacing_max_late_us=%" PRIu64
            " transport=%s gso_requested=%u gso_attempts=%u"
            " gso_batches=%u gso_packets=%u gso_fallbacks=%u"
            " gso_ineligible=%u gso_errno=%u",
           options->dry_run ? "dry-run" : "sent", session_id, center_index, center_hz,
           options->samples_per_session, session_result.data_packets,
           session_result.logical_udp_packets, attempt + 1U, options->rate_mbps,
           options->send_batch,
            session_result.elapsed_us != 0U ?
                (uint64_t)((session_result.payload_bytes * 8000ULL) /
                           session_result.elapsed_us) : 0U,
            1U + (options->samples_per_session - IQ_MODEL_WINDOW_SAMPLES) /
                 IQ_MODEL_STRIDE_SAMPLES,
             IQ_MODEL_STRIDE_SAMPLES, session_result.pacing_rebases,
             session_result.pacing_max_late_us,
             (session_result.udp_gso_batches != 0U) ? "udp_gso" :
             ((session_result.udp_gso_requested != 0U) ? "sendmmsg_gso_fallback" :
              "sendmmsg"),
             session_result.udp_gso_requested,
             session_result.udp_gso_attempts,
             session_result.udp_gso_batches,
             session_result.udp_gso_packets,
             session_result.udp_gso_fallbacks,
             session_result.udp_gso_ineligible_batches,
             session_result.udp_gso_last_errno);
    if (options->window_crc != 0)
    {
        printf(" crc32c=0x%08" PRIX32
               " crc_backend=%s crc_timing=%s crc_cpu_us=%" PRIu64,
               session_result.crc32c,
               crc32c_backend_name(
                   (sdr_crc_backend_t)session_result.crc_backend),
               session_result.crc_timing_enabled != 0U ? "on" : "off",
               session_result.crc_cpu_us);
    }
    if (options->ack_enabled != 0)
    {
        printf(" ack=%s", options->dry_run ? "skipped-dry-run" : "verified");
    }
    putchar('\n');
    if (retry_cache != NULL) { sdr_aligned_free(retry_cache); }
    return 1;
}

#ifndef RA8P1_SDR_IQ_STREAM_EMBEDDED
int main(int argc, char **argv)
{
    options_t options;
    struct sockaddr_in peer;
    struct sockaddr_in ack_peer;
    socket_handle_t socket_fd = INVALID_SOCKET_HANDLE;
    socket_handle_t ack_socket_fd = INVALID_SOCKET_HANDLE;
    ra8p1_sdr_adapter_runtime_t sdr_runtime;
    uint8_t *sdr_cache = NULL;
    size_t sdr_cache_bytes = 0U;
    int sdr_opened = 0;
    int result = 0;
    uint32_t rate_index;
    uint32_t pass;
    uint32_t center;

    memset(&sdr_runtime, 0, sizeof(sdr_runtime));

    if (!parse_options(argc, argv, &options))
    {
        usage(argv[0]);
        return 2;
    }
#ifdef _WIN32
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            fprintf(stderr, "WSAStartup failed\n");
            return 3;
        }
    }
#endif
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(IQ_PORT);
    if (inet_pton(AF_INET, options.target_ip, &peer.sin_addr) != 1)
    {
        fprintf(stderr, "invalid IPv4 destination: %s\n", options.target_ip);
        result = 3;
        goto cleanup;
    }
    if (!options.dry_run)
    {
        socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd == INVALID_SOCKET_HANDLE)
        {
            perror("socket");
            result = 3;
            goto cleanup;
        }
        if (connect(socket_fd, (const struct sockaddr *)&peer, sizeof(peer)) != 0)
        {
            perror("connect");
            result = 3;
            goto cleanup;
        }
        configure_udp_socket(socket_fd, &options);
    }
    if ((options.ack_enabled != 0) && (options.dry_run == 0))
    {
        memset(&ack_peer, 0, sizeof(ack_peer));
        ack_peer.sin_family = AF_INET;
        ack_peer.sin_port = htons(IQ_ACK_PORT);
        ack_peer.sin_addr = peer.sin_addr;
        ack_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ack_socket_fd == INVALID_SOCKET_HANDLE)
        {
            perror("ACK socket");
            result = 3;
            goto cleanup;
        }
        if (connect(ack_socket_fd, (const struct sockaddr *)&ack_peer,
                    sizeof(ack_peer)) != 0)
        {
            perror("ACK connect");
            result = 3;
            goto cleanup;
        }
    }
    if (options.source_mode == SOURCE_SDR)
    {
        sdr_cache_bytes = (size_t) options.samples_per_session * IQ_BYTES_PER_SAMPLE;
        sdr_cache = (uint8_t *) sdr_aligned_allocate(64U, sdr_cache_bytes);
        if (sdr_cache == NULL)
        {
            fprintf(stderr, "cannot allocate %zu-byte RX1 SDR cache\n", sdr_cache_bytes);
            result = 4;
            goto cleanup;
        }
        if (!sdr_adapter_open(&sdr_runtime, options.sdr_library_path,
                              options.centers[0], options.sample_rate_hz,
                              options.bandwidth_hz))
        {
            result = 4;
            goto cleanup;
        }
        sdr_opened = 1;
    }
    for (rate_index = 0U; rate_index < options.rate_count; rate_index++)
    {
        options.rate_mbps = options.rate_sweep[rate_index];
        printf("rate-sweep step=%" PRIu32 "/%" PRIu32 " target_mbps=%" PRIu32 "\n",
               rate_index + 1U, options.rate_count, options.rate_mbps);
        for (pass = 0U; pass < options.sessions; pass++)
        {
            for (center = 0U; center < options.center_count; center++)
            {
                uint64_t session_offset =
                    ((uint64_t) rate_index * options.sessions + pass) *
                    options.center_count + center;
                uint64_t session_id_wide = (uint64_t) options.session_id + session_offset;
                uint32_t session_id;
                if ((session_id_wide == 0ULL) || (session_id_wide > UINT32_MAX))
                {
                    fprintf(stderr, "session ID range overflows uint32 at rate step=%" PRIu32
                            " pass=%" PRIu32 " center=%" PRIu32 "\n",
                            rate_index, pass, center);
                    result = 4;
                    goto cleanup;
                }
                session_id = (uint32_t) session_id_wide;
                if ((options.source_mode == SOURCE_SDR) &&
                    !sdr_capture_session(&sdr_runtime, &options, options.centers[center],
                                         sdr_cache))
                {
                    result = 4;
                    goto cleanup;
                }
                if (!send_session(socket_fd, ack_socket_fd, &peer, &options, session_id,
                                  options.centers[center], options.center_indices[center],
                                  stdin, sdr_cache))
                {
                    result = 4;
                    goto cleanup;
                }
            }
        }
    }
    result = 0;

cleanup:
    if (sdr_opened != 0)
    {
        if (!sdr_adapter_close(&sdr_runtime) && (result == 0))
        {
            result = 4;
        }
    }
    if (sdr_cache != NULL)
    {
        sdr_aligned_free(sdr_cache);
    }
    if (socket_fd != INVALID_SOCKET_HANDLE) { close_socket(socket_fd); }
    if (ack_socket_fd != INVALID_SOCKET_HANDLE) { close_socket(ack_socket_fd); }
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
#endif
