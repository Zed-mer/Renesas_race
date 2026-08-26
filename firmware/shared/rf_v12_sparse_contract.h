#ifndef RF_V12_SPARSE_CONTRACT_H
#define RF_V12_SPARSE_CONTRACT_H

#include <stdint.h>

/* V12 is a sparse single-tile V2+V3 ABI. It does not use 100 ms completion. */
#define RF_V12_ABI_MAGIC 0x53323156u
#define RF_V12_ABI_VERSION_MAJOR 12u
#define RF_V12_ABI_VERSION_MINOR 0u

#define RF_V12_CENTER_2420_HZ UINT64_C(2420000000)
#define RF_V12_CENTER_2464_HZ UINT64_C(2464000000)
#define RF_V12_CENTER_5760_HZ UINT64_C(5760000000)
#define RF_V12_CENTER_5816_HZ UINT64_C(5816000000)
#define RF_V12_CENTER_COUNT 4u

#define RF_V12_SAMPLE_RATE_HZ 60000000u
#define RF_V12_TILE_SAMPLES 590336u
#define RF_V12_FFT_POINTS 1024u
#define RF_V12_HOP_SAMPLES 512u
#define RF_V12_RAW_STFT_FRAMES 1152u
#define RF_V12_STFT_EDGE_CROP_FRAMES 1u
#define RF_V12_TIME_POOL_FRAMES 10u
#define RF_V12_RELIABLE_BANDWIDTH_HZ 56000000u

#define RF_V12_FEATURE_FREQUENCY_BINS 204u
#define RF_V12_FEATURE_TIME_BINS 115u
#define RF_V12_FEATURE_CHANNELS 4u
#define RF_V12_FEATURE_BYTES 93840u
#define RF_V12_INPUT_SCALE 0.14275820553302765f
#define RF_V12_INPUT_ZERO_POINT 0
#define RF_V12_C0_CLIP_MIN (-8.0f)
#define RF_V12_C1_CLIP_MIN 0.0f
#define RF_V12_C2_CLIP_MIN (-12.0f)
#define RF_V12_C3_CLIP_MIN (-22.0f)
#define RF_V12_C0_CLIP_MAX 32.0f
#define RF_V12_C1_CLIP_MAX 12.0f
#define RF_V12_C2_CLIP_MAX 12.0f
#define RF_V12_C3_CLIP_MAX 22.0f
#define RF_V12_C0_MEAN 3.624701738357544f
#define RF_V12_C1_MEAN 3.0793359270f
#define RF_V12_C2_MEAN 0.000020333360225777142f
#define RF_V12_C3_MEAN 0.000061713853845931f
#define RF_V12_C0_STD 4.285704135894775f
#define RF_V12_C1_STD 0.9367200732f
#define RF_V12_C2_STD 0.9811630249023438f
#define RF_V12_C3_STD 1.2086801528930664f

#define RF_V12_HEATMAP_FREQUENCY_BINS 102u
#define RF_V12_HEATMAP_TIME_BINS 58u
#define RF_V12_HEATMAP_BYTES 5916u
#define RF_V12_DTCM_HEATMAP_COUNT 5u
#define RF_V12_DTCM_HEATMAP_BYTES 29580u
#define RF_V12_VELA_TENSOR_REGION 1u

#define RF_V21_SHARED_ARENA_BYTES 192176u
#define RF_V12_SHARED_ARENA_BYTES RF_V21_SHARED_ARENA_BYTES
#define RF_V12_SHARED_ARENA_HARD_LIMIT_BYTES 198256u
#define RF_V21_NONVIDEO_ARENA_INPUT_OFFSET 98336u
#define RF_V21_V20_VIDEO_ARENA_INPUT_OFFSET 47328u
#define RF_V21_ARENA_INPUT_OFFSET RF_V21_NONVIDEO_ARENA_INPUT_OFFSET
#define RF_V12_MAX_BOXES_PER_TILE 4u
#define RF_V12_PER_CLASS_PEAK_TOP_K 6u
#define RF_V12_PREFILTER_GLOBAL_TOP_K 32u
#define RF_V12_NMS_IOU_Q15 9830u
#define RF_V12_MIN_RELIABLE_FREQUENCY_FRACTION_Q15 16384u

#define RF_V12_CENTER_MASK_2420 (1u << 0)
#define RF_V12_CENTER_MASK_2464 (1u << 1)
#define RF_V12_CENTER_MASK_5760 (1u << 2)
#define RF_V12_CENTER_MASK_5816 (1u << 3)
#define RF_V12_CLASS_CENTER_MASK_DJI_CONTROL 0x0fu
#define RF_V12_CLASS_CENTER_MASK_DJI_VIDEO 0x0fu
#define RF_V12_CLASS_CENTER_MASK_AT9S 0x03u
#define RF_V12_CLASS_CENTER_MASK_T12 0x03u
#define RF_V12_CLASS_CENTER_MASK_XIAOBAWANG 0x03u

#define RF_V12_VIDEO_COMPONENT_CONNECTIVITY 8u
#define RF_V12_VIDEO_COMPONENT_MIN_AREA 4u
#define RF_V12_VIDEO_COMPONENT_MIN_FREQUENCY_HEIGHT 2u
#define RF_V12_VIDEO_COMPONENT_EDGE_SNAP_ROWS 6u
#define RF_V12_VIDEO_PROFILE_TIME_RADIUS 7u
#define RF_V12_VIDEO_OCCUPIED_DB_Q8 256u
#define RF_V12_VIDEO_OCCUPIED_FRACTION_Q15 22938u

#define RF_V12_DJI_CONTROL_BANDWIDTH_HZ 2200000u
#define RF_V12_DJI_CONTROL_DURATION_SAMPLES 31200u
#define RF_V12_DJI_VIDEO_10M_BANDWIDTH_HZ 10000000u
#define RF_V12_DJI_VIDEO_20M_BANDWIDTH_HZ 20000000u
#define RF_V12_DJI_VIDEO_DURATION_SAMPLES 66000u
#define RF_V12_AT9S_BANDWIDTH_HZ 8000000u
#define RF_V12_AT9S_DURATION_SAMPLES 123000u
#define RF_V12_T12_BANDWIDTH_HZ 1700000u
#define RF_V12_T12_DURATION_SAMPLES 276000u
#define RF_V12_XIAOBAWANG_BANDWIDTH_HZ 2400000u
#define RF_V12_XIAOBAWANG_DURATION_SAMPLES 123000u

typedef enum rf_v12_class_id {
    RF_V12_CLASS_DJI_CONTROL = 0,
    RF_V12_CLASS_DJI_VIDEO = 1,
    RF_V12_CLASS_AT9S = 2,
    RF_V12_CLASS_T12 = 3,
    RF_V12_CLASS_XIAOBAWANG = 4,
    RF_V12_CLASS_COUNT = 5
} rf_v12_class_id_t;

/* V21 exposes logical classes [4, 1, 3, 0, 2] in physical tensor order. */
typedef enum rf_v21_nonvideo_physical_output {
    RF_V21_NONVIDEO_OUTPUT_XIAOBAWANG = 0,
    RF_V21_NONVIDEO_OUTPUT_VIDEO_IGNORED = 1,
    RF_V21_NONVIDEO_OUTPUT_T12 = 2,
    RF_V21_NONVIDEO_OUTPUT_DJI_CONTROL = 3,
    RF_V21_NONVIDEO_OUTPUT_AT9S = 4,
    RF_V21_NONVIDEO_OUTPUT_COUNT = 5
} rf_v21_nonvideo_physical_output_t;

/* DTCM slots are deliberately not in logical class-id order. */
typedef enum rf_v12_dtcm_heatmap_slot {
    RF_V12_DTCM_SLOT_XIAOBAWANG = 0,
    RF_V12_DTCM_SLOT_T12 = 1,
    RF_V12_DTCM_SLOT_DJI_CONTROL = 2,
    RF_V12_DTCM_SLOT_AT9S = 3,
    RF_V12_DTCM_SLOT_DJI_VIDEO = 4,
    RF_V12_DTCM_SLOT_COUNT = 5
} rf_v12_dtcm_heatmap_slot_t;

enum rf_v21_nonvideo_output_contract {
    RF_V21_NONVIDEO_XIAOBAWANG_OFFSET = 95584u,
    RF_V21_NONVIDEO_VIDEO_IGNORED_OFFSET = 107424u,
    RF_V21_NONVIDEO_T12_OFFSET = 113344u,
    RF_V21_NONVIDEO_DJI_CONTROL_OFFSET = 101504u,
    RF_V21_NONVIDEO_AT9S_OFFSET = 119264u
};

#define RF_V21_NONVIDEO_XIAOBAWANG_SCALE 0.0788859725f
#define RF_V21_NONVIDEO_XIAOBAWANG_ZERO_POINT 114
#define RF_V21_NONVIDEO_XIAOBAWANG_THRESHOLD 100
#define RF_V21_NONVIDEO_VIDEO_IGNORED_SCALE 0.0313725509f
#define RF_V21_NONVIDEO_VIDEO_IGNORED_ZERO_POINT 127
#define RF_V21_NONVIDEO_T12_SCALE 0.0865136981f
#define RF_V21_NONVIDEO_T12_ZERO_POINT 115
#define RF_V21_NONVIDEO_T12_THRESHOLD 109
#define RF_V21_NONVIDEO_DJI_CONTROL_SCALE 0.0630370155f
#define RF_V21_NONVIDEO_DJI_CONTROL_ZERO_POINT 101
#define RF_V21_NONVIDEO_DJI_CONTROL_THRESHOLD 103
#define RF_V21_NONVIDEO_AT9S_SCALE 0.1007100344f
#define RF_V21_NONVIDEO_AT9S_ZERO_POINT 118
#define RF_V21_NONVIDEO_AT9S_THRESHOLD 108

#define RF_V20_V3_VIDEO_OFFSET 0u
#define RF_V20_V3_VIDEO_SCALE 0.1339167506f
#define RF_V20_V3_VIDEO_ZERO_POINT 89
#define RF_V20_V3_VIDEO_THRESHOLD 83

typedef enum rf_v12_tile_validity {
    RF_V12_TILE_INVALID = 0,
    RF_V12_TILE_BACKGROUND_NOT_READY = 1,
    RF_V12_TILE_VALID = 2
} rf_v12_tile_validity_t;

typedef enum rf_v12_round_observation {
    RF_V12_ROUND_INCOMPLETE = 0,
    RF_V12_ROUND_NO_TARGET_RF_OBSERVED = 1,
    RF_V12_ROUND_TARGET_RF_OBSERVED = 2
} rf_v12_round_observation_t;

typedef enum rf_v12_activity_state {
    RF_V12_ACTIVITY_UNKNOWN = 0,
    RF_V12_ACTIVITY_CANDIDATE = 1,
    RF_V12_ACTIVITY_RF_ACTIVE_HELD = 2,
    RF_V12_ACTIVITY_RF_ACTIVE_CONFIRMED = 3,
    RF_V12_ACTIVITY_STALE = 4
} rf_v12_activity_state_t;

enum rf_v12_event_flags {
    RF_V12_EVENT_TIME_CLIPPED = 1u << 0,
    RF_V12_EVENT_FREQUENCY_CLIPPED = 1u << 1,
    RF_V12_EVENT_VIDEO_20MHZ = 1u << 2,
    RF_V12_EVENT_BANDWIDTH_AMBIGUOUS = 1u << 3,
    RF_V12_EVENT_NEEDS_REVIEW = 1u << 4
};

enum rf_v12_tile_flags {
    RF_V12_TILE_CRC_ERROR = 1u << 0,
    RF_V12_TILE_PACKET_GAP = 1u << 1,
    RF_V12_TILE_RETUNE_UNLOCKED = 1u << 2,
    RF_V12_TILE_RING_OVERFLOW = 1u << 3,
    RF_V12_TILE_ADC_SATURATION = 1u << 4,
    RF_V12_TILE_CAPTURE_TIMEOUT = 1u << 5,
    RF_V12_TILE_BACKGROUND_RESET = 1u << 6,
    RF_V12_TILE_RESULT_TRUNCATED = 1u << 7
};

typedef struct rf_v12_visible_event {
    uint32_t track_id;
    int32_t frequency_low_offset_hz;
    int32_t frequency_high_offset_hz;
    uint32_t visible_start_sample;
    uint32_t visible_end_sample;
    uint16_t confidence_q15;
    uint8_t class_id;
    uint8_t flags;
} rf_v12_visible_event_t;

/* Time and frequency bounds are half-open: [start, end). */
#define RF_V12_EVENT_BOUNDS_HALF_OPEN 1u
/* confidence_q15 = round(clamp(probability, 0, 1) * 32767). */
#define RF_V12_CONFIDENCE_Q15_ONE 32767u

/* One payload describes one valid sparse tile, not a continuous 100 ms dwell. */
typedef struct rf_v12_tile_payload {
    uint32_t magic;
    uint16_t abi_version_major;
    uint16_t abi_version_minor;
    uint32_t sequence;
    uint32_t round_index;
    uint64_t capture_center_frequency_hz;
    uint64_t capture_start_time_us;
    uint64_t capture_end_time_us;
    uint32_t sample_rate_hz;
    uint32_t tile_samples;
    uint16_t event_count;
    uint8_t center_index;
    uint8_t round_valid_center_mask;
    uint8_t tile_validity;
    uint8_t round_observation;
    uint8_t flags;
    uint8_t reserved_state;
    uint16_t background_generation;
    int16_t sdr_gain_db_q8;
    rf_v12_visible_event_t events[RF_V12_MAX_BOXES_PER_TILE];
    uint8_t reserved[356];
} rf_v12_tile_payload_t;

/* CPU1 owns this state. It is not produced by the CPU0 tile detector. */
typedef struct rf_v12_activity_summary {
    uint32_t last_complete_round_index;
    uint64_t last_target_time_us;
    uint64_t last_complete_round_time_us;
    uint8_t activity_state;
    uint8_t reserved[7];
} rf_v12_activity_summary_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(rf_v12_visible_event_t) == 24u,
               "rf_v12_visible_event_t ABI changed");
_Static_assert(sizeof(rf_v12_tile_payload_t) == 512u,
               "rf_v12_tile_payload_t must fit one 512-byte slot");
#endif

/* NO_TARGET_RF_OBSERVED requires mask 0x0f, four valid/background-ready tiles,
 * and no target in the complete round. A single empty tile cannot set it. */
#define RF_V12_NO_RF_OBSERVED_MEANS_DEVICE_OFF 0u
#define RF_V12_COMPLETE_ROUND_CENTER_MASK 0x0fu
#define RF_V12_ALLOW_CROSS_RETUNE_STITCH 0u
#define RF_V12_ALLOW_SYNTHETIC_CYCLE_BOX 0u
#define RF_V12_STATE_FUSION_IMPLEMENTATION_INCLUDED 0u

#endif
