#ifndef RA8P1_SYSTEM_PROTOCOL_H
#define RA8P1_SYSTEM_PROTOCOL_H

#include <stdint.h>

#define RA8P1_SYSTEM_PROTOCOL_MAGIC   (0x52414951UL) /* RAIQ */
#define RA8P1_SYSTEM_PROTOCOL_VERSION (3U)

#define RA8P1_CENTER_2420_HZ          (2420000000ULL)
#define RA8P1_CENTER_2464_HZ          (2464000000ULL)
#define RA8P1_CENTER_5760_HZ          (5760000000ULL)
#define RA8P1_CENTER_5816_HZ          (5816000000ULL)
#define RA8P1_CENTER_COUNT             (4U)
#define RA8P1_PROBABILITY_ONE_Q15      (32767U)
#define RA8P1_PROBABILITY_HALF_Q15     (16384U)

#define RA8P1_SDR_TARGET_PAYLOAD_MIN_MBPS_X1000     (1000UL)
#define RA8P1_SDR_TARGET_PAYLOAD_MAX_MBPS_X1000     (940000UL)
#define RA8P1_SDR_TARGET_PAYLOAD_DEFAULT_MBPS_X1000 (390000UL)

#define RA8P1_SDR_TEST_FAULT_CRC32C                    (1UL << 0)
#define RA8P1_SDR_TEST_FAULT_DROP_DATA_PACKET          (1UL << 1)
#define RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_REQUEST      (1UL << 2)
#define RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_ACK_RESPONSE (1UL << 3)
#define RA8P1_SDR_TEST_FAULT_ALL \
    (RA8P1_SDR_TEST_FAULT_CRC32C | \
     RA8P1_SDR_TEST_FAULT_DROP_DATA_PACKET | \
     RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_REQUEST | \
     RA8P1_SDR_TEST_FAULT_IGNORE_FIRST_ACK_RESPONSE)

static inline uint64_t ra8p1_center_frequency_hz(uint32_t index)
{
    static const uint64_t centers[RA8P1_CENTER_COUNT] =
    {
        RA8P1_CENTER_2420_HZ,
        RA8P1_CENTER_2464_HZ,
        RA8P1_CENTER_5760_HZ,
        RA8P1_CENTER_5816_HZ
    };
    return (index < RA8P1_CENTER_COUNT) ? centers[index] : 0ULL;
}

static inline int32_t ra8p1_center_index_from_hz(uint64_t center_hz)
{
    uint32_t index;
    for (index = 0U; index < RA8P1_CENTER_COUNT; ++index)
    {
        if (ra8p1_center_frequency_hz(index) == center_hz)
        {
            return (int32_t)index;
        }
    }
    return -1;
}

#define RA8P1_RF_CHANNEL_A_MASK       (1UL << 0)
#define RA8P1_RF_CHANNEL_B_MASK       (1UL << 1)
#define RA8P1_RF_CHANNEL_MASK_ALL     (RA8P1_RF_CHANNEL_A_MASK | RA8P1_RF_CHANNEL_B_MASK)

#define RA8P1_SYSTEM_FLAG_FFT_STATUS_MASK (0xFFUL)
#define RA8P1_SYSTEM_FLAG_CHANNEL_B_SEEN  (1UL << 8)
#define RA8P1_SYSTEM_FLAG_NPU_NOT_READY   (1UL << 9)
#define RA8P1_SYSTEM_FLAG_SYNTHETIC       (1UL << 10)
#define RA8P1_SYSTEM_FLAG_REAL_STREAM     (1UL << 11)
#define RA8P1_SYSTEM_FLAG_MODEL_PLACEHOLDER (1UL << 12)
#define RA8P1_SYSTEM_FLAG_PREPROCESS_INVALID (1UL << 13)

#define RA8P1_MODEL_FLAG_PLACEHOLDER          (1UL << 0)
#define RA8P1_MODEL_FLAG_MASK_32X32           (1UL << 1)
#define RA8P1_MODEL_FLAG_PREPROCESS_PLACEHOLDER (1UL << 2)
#define RA8P1_MODEL_FLAG_NOISE_FLOOR_ZERO     (1UL << 3)
#define RA8P1_MODEL_FLAG_NO_ACCURACY_CLAIM    (1UL << 4)
#define RA8P1_MODEL_FLAG_TRAINED_INT8          (1UL << 5)
#define RA8P1_MODEL_FLAG_CENTER_HEATMAP        (1UL << 6)
#define RA8P1_MODEL_FLAG_ADAPTIVE_BASELINE     (1UL << 7)
#define RA8P1_MODEL_FLAG_FIVE_CLASS_FUSED_UI   (1UL << 8)
#define RA8P1_MODEL_FLAG_CONSERVATIVE_ALERT_GUARD (1UL << 9)
#define RA8P1_MODEL_FLAG_DUAL_NPU_MODELS       (1UL << 10)
#define RA8P1_MODEL_FLAG_VIDEO_VISIBLE_MASK    (1UL << 11)
#define RA8P1_MODEL_FLAG_PER_CENTER_BASELINE   (1UL << 12)
#define RA8P1_MODEL_FLAG_BASELINE_READY        (1UL << 13)

#define RA8P1_COMMAND_FLAG_START               (1UL << 0)
#define RA8P1_COMMAND_FLAG_STOP                (1UL << 1)
#define RA8P1_COMMAND_FLAG_APPLY_RF            (1UL << 2)
#define RA8P1_COMMAND_FLAG_SCAN_ALL            (1UL << 3)
/* CPU0 owns repeated acquisition.  With SCAN_ALL this repeats the four-center
 * sweep; without SCAN_ALL it repeats requested_center_hz.  Omitting this flag
 * keeps the existing finite single-window/four-window campaign semantics. */
#define RA8P1_COMMAND_FLAG_SCAN_CONTINUOUS     (1UL << 4)

typedef enum e_ra8p1_iq_format
{
    RA8P1_IQ_FORMAT_UNKNOWN = 0,
    RA8P1_IQ_FORMAT_S16_LE_INTERLEAVED = 1,
    RA8P1_IQ_FORMAT_S8_INTERLEAVED = 2
} ra8p1_iq_format_t;

typedef enum e_ra8p1_pipeline_state
{
    RA8P1_PIPELINE_STOPPED = 0,
    RA8P1_PIPELINE_WAITING_FOR_IQ = 1,
    RA8P1_PIPELINE_RUNNING = 2,
    RA8P1_PIPELINE_OVERLOAD = 3,
    RA8P1_PIPELINE_ERROR = 4
} ra8p1_pipeline_state_t;

typedef enum e_ra8p1_command_status
{
    RA8P1_COMMAND_NONE = 0,
    RA8P1_COMMAND_ACCEPTED_PENDING_EXTERNAL_APPLY = 1,
    RA8P1_COMMAND_REJECTED = 2,
    RA8P1_COMMAND_APPLIED = 3
} ra8p1_command_status_t;

typedef enum e_ra8p1_command_action
{
    RA8P1_COMMAND_ACTION_NONE = 0U,
    RA8P1_COMMAND_ACTION_START = 1U,
    RA8P1_COMMAND_ACTION_STOP = 2U
} ra8p1_command_action_t;

typedef enum e_ra8p1_command_reason
{
    RA8P1_COMMAND_REASON_NONE = 0U,
    RA8P1_COMMAND_REASON_INVALID_FORMAT = 1U,
    RA8P1_COMMAND_REASON_INVALID_RATE = 2U,
    RA8P1_COMMAND_REASON_INVALID_CHANNEL = 3U,
    RA8P1_COMMAND_REASON_INVALID_WINDOW = 4U,
    RA8P1_COMMAND_REASON_INVALID_BANDWIDTH = 5U,
    RA8P1_COMMAND_REASON_WAITING_STREAM_START = 6U,
    RA8P1_COMMAND_REASON_STREAM_MISMATCH = 7U,
    RA8P1_COMMAND_REASON_APPLIED = 8U,
    RA8P1_COMMAND_REASON_STOPPED = 9U,
    RA8P1_COMMAND_REASON_SDR_CONTROL_BUSY = 10U,
    RA8P1_COMMAND_REASON_SDR_CONTROL_SEND_FAILED = 11U,
    RA8P1_COMMAND_REASON_SDR_CONTROL_TIMEOUT = 12U,
    RA8P1_COMMAND_REASON_SDR_CONTROL_ERROR = 13U
} ra8p1_command_reason_t;

typedef struct st_ra8p1_system_telemetry
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t pipeline_state;
    uint32_t ethernet_link_mbps;
    uint32_t iq_payload_mbps_x1000;
    uint32_t iq_format;
    uint32_t iq_sample_rate_hz;
    uint32_t ingress_packets;
    uint32_t ingress_drops;
    uint32_t ring_high_watermark;
    uint32_t processed_blocks;
    uint32_t fft_frames;
    uint32_t inference_count;
    uint32_t inference_cycles;
    uint32_t result_class;
    int32_t  result_score_q15;
    uint32_t cpu0_load_permille;
    uint32_t cpu1_load_permille;
    uint32_t display_fps_millihz;
    uint32_t flags;
    uint32_t fft_size;
    uint32_t fft_peak_bin;
    uint32_t fft_peak_power_q16;
    uint32_t fft_capture_interval_samples;
    uint32_t channel_mask;
    uint32_t channel_a_fft_frames;
    uint32_t channel_b_fft_frames;
    uint32_t channel_b_peak_bin;
    uint32_t channel_b_peak_power_q16;
    uint32_t command_sequence;
    uint32_t command_status;
    uint32_t command_reason;
    uint32_t applied_session_id;
    uint32_t model_revision;
    uint32_t model_flags;
} ra8p1_system_telemetry_t;

typedef struct st_ra8p1_ui_command
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t requested_center_hz_low;
    uint32_t requested_center_hz_high;
    uint32_t requested_center_b_hz_low;
    uint32_t requested_center_b_hz_high;
    uint32_t requested_sample_rate_hz;
    uint32_t requested_bandwidth_hz;
    int32_t  requested_gain_mdB;
    uint32_t requested_iq_format;
    uint32_t flags;
    uint32_t requested_channel_mask;
    uint32_t requested_fft_interval_samples;
    uint32_t action;
    uint32_t target_payload_mbps_x1000;
    uint32_t test_fault_flags;
} ra8p1_ui_command_t;

static inline uint64_t ra8p1_ui_command_center_a_hz(
    const ra8p1_ui_command_t *command)
{
    return (command == 0) ? 0ULL :
        (((uint64_t)command->requested_center_hz_high << 32U) |
         command->requested_center_hz_low);
}

static inline uint64_t ra8p1_ui_command_center_b_hz(
    const ra8p1_ui_command_t *command)
{
    return (command == 0) ? 0ULL :
        (((uint64_t)command->requested_center_b_hz_high << 32U) |
         command->requested_center_b_hz_low);
}

typedef char ra8p1_telemetry_size_must_be_144[(sizeof(ra8p1_system_telemetry_t) == 144U) ? 1 : -1];
typedef char ra8p1_ui_command_size_must_be_68[(sizeof(ra8p1_ui_command_t) == 68U) ? 1 : -1];

#endif
