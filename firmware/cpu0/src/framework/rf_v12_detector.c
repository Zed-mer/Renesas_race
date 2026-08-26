#include "rf_v12_detector.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "npu_runner.h"
#include "rf_v13_activity_fusion.h"
#include "rf_v16_roi_calibration.h"
#include "rf_v17_subbin_calibration.h"
#include "rf_v18_source_gate.h"
#include "rf_v20_video_postprocess.h"
#include "rf_v26_partition_guard.h"
#include "rf_v31_detection_contract.h"

#define RF_V12_CANDIDATES_PER_CLASS (RF_V12_PER_CLASS_PEAK_TOP_K)
#define RF_V12_GLOBAL_CANDIDATES (RF_V12_PREFILTER_GLOBAL_TOP_K)
#define RF_V32_MAX_ROIS_PER_TILE (4U)
#define RF_V32_WIDTH_TRACK_COUNT (4U)
#define RF_V32_WIDTH_TRACK_MATCH_HZ INT32_C(5000000)
#define RF_V32_WIDTH_TRACK_EXPIRE_TILES UINT32_C(16)

typedef struct st_rf_v12_candidate
{
    rf_v12_visible_event_t event;
    int32_t center_frequency_offset_hz;
    int32_t center_sample;
    uint32_t bandwidth_hz;
    uint16_t state_confidence_q15;
    uint8_t state_roi_decision;
    uint8_t state_quality_tier;
    int8_t raw_logit;
} rf_v12_candidate_t;

typedef struct st_rf_v12_class_contract
{
    float scale;
    int16_t zero_point;
    int16_t threshold;
    uint32_t bandwidth_hz;
    uint32_t duration_samples;
    uint8_t center_mask;
} rf_v12_class_contract_t;

typedef struct st_rf_v32_detector_track
{
    rf_v32_width_track_t state;
    int32_t center_frequency_offset_hz;
    uint32_t last_tile_sequence;
    uint8_t center_slot;
    uint8_t active;
    uint16_t reserved;
} rf_v32_detector_track_t;

static const rf_v12_class_contract_t g_rf_v12_class_contract[RF_V12_CLASS_COUNT] =
{
    {RF_V31_DJI_CONTROL_OUTPUT_SCALE,
     RF_V31_DJI_CONTROL_OUTPUT_ZERO_POINT,
     RF_V31_DJI_CONTROL_THRESHOLD_Q,
     RF_V31_DJI_CONTROL_BANDWIDTH_HZ,
     RF_V31_DJI_CONTROL_DURATION_SAMPLES,
     RF_V31_CLASS_CENTER_MASK_DJI_CONTROL},
    {RF_V31_DJI_VIDEO_OUTPUT_SCALE,
     RF_V31_DJI_VIDEO_OUTPUT_ZERO_POINT,
     RF_V31_DJI_VIDEO_THRESHOLD_Q,
     RF_V31_DJI_VIDEO_10M_BANDWIDTH_HZ,
     RF_V31_DJI_VIDEO_DURATION_SAMPLES,
     RF_V31_CLASS_CENTER_MASK_DJI_VIDEO},
    {RF_V31_AT9S_OUTPUT_SCALE,
     RF_V31_AT9S_OUTPUT_ZERO_POINT,
     RF_V31_AT9S_THRESHOLD_Q,
     RF_V31_AT9S_BANDWIDTH_HZ,
     RF_V31_AT9S_DURATION_SAMPLES,
     RF_V31_CLASS_CENTER_MASK_AT9S},
    {RF_V31_T12_OUTPUT_SCALE,
     RF_V31_T12_OUTPUT_ZERO_POINT,
     RF_V31_T12_THRESHOLD_Q,
     RF_V31_T12_BANDWIDTH_HZ,
     RF_V31_T12_DURATION_SAMPLES,
     RF_V31_CLASS_CENTER_MASK_T12},
    {RF_V31_XIAOBAWANG_OUTPUT_SCALE,
     RF_V31_XIAOBAWANG_OUTPUT_ZERO_POINT,
     RF_V31_XIAOBAWANG_THRESHOLD_Q,
     RF_V31_XIAOBAWANG_BANDWIDTH_HZ,
     RF_V31_XIAOBAWANG_DURATION_SAMPLES,
     RF_V31_CLASS_CENTER_MASK_XIAOBAWANG}
};

static rf_v32_detector_track_t g_rf_v32_width_tracks[RF_V32_WIDTH_TRACK_COUNT];

static uint32_t rf_v12_heatmap_index(uint32_t frequency, uint32_t time)
{
    return (frequency * RF_V12_HEATMAP_TIME_BINS) + time;
}

static float rf_v12_probability(uint8_t class_id, int8_t value)
{
    const rf_v12_class_contract_t *contract =
        &g_rf_v12_class_contract[class_id];
    const float logit = ((float)value - (float)contract->zero_point) *
                        contract->scale;
    return 1.0F / (1.0F + expf(-logit));
}

static uint16_t rf_v12_probability_q15(float probability)
{
    int32_t value;

    if (!(probability > 0.0F))
    {
        return 0U;
    }
    if (probability >= 1.0F)
    {
        return RF_V12_CONFIDENCE_Q15_ONE;
    }
    value = (int32_t)(probability * (float)RF_V12_CONFIDENCE_Q15_ONE + 0.5F);
    return (uint16_t)value;
}

uint8_t rf_v12_class_to_object(uint8_t class_id)
{
    switch (class_id)
    {
        case RF_V12_CLASS_DJI_CONTROL:
        case RF_V12_CLASS_DJI_VIDEO:
            return 0U;
        case RF_V12_CLASS_AT9S:
            return 1U;
        case RF_V12_CLASS_T12:
            return 2U;
        case RF_V12_CLASS_XIAOBAWANG:
            return 3U;
        default:
            return UINT8_MAX;
    }
}

static int64_t rf_v12_round_divide(int64_t numerator, int64_t denominator)
{
    if (numerator >= 0)
    {
        return (numerator + (denominator / 2)) / denominator;
    }
    return -(((-numerator) + (denominator / 2)) / denominator);
}

static bool rf_v12_candidate_from_physical_center(
    uint8_t class_id,
    int32_t center_frequency,
    int32_t center_sample,
    uint16_t confidence_q15,
    int8_t raw_logit,
    uint32_t bandwidth_hz,
    uint8_t extra_flags,
    rf_v12_candidate_t *candidate)
{
    int64_t frequency_low;
    int64_t frequency_high;
    int64_t visible_low;
    int64_t visible_high;
    int64_t time_low;
    int64_t time_high;
    int64_t visible_time_low;
    int64_t visible_time_high;
    uint8_t flags = extra_flags;

    if ((candidate == NULL) || (class_id >= RF_V12_CLASS_COUNT))
    {
        return false;
    }

    frequency_low = center_frequency - ((int64_t)bandwidth_hz / 2LL);
    frequency_high = frequency_low + (int64_t)bandwidth_hz;
    visible_low = (frequency_low < -28000000LL) ? -28000000LL : frequency_low;
    visible_high = (frequency_high > 28000000LL) ? 28000000LL : frequency_high;
    if ((visible_high <= visible_low) ||
        (((visible_high - visible_low) *
          (int64_t)RF_V12_CONFIDENCE_Q15_ONE) <
         ((int64_t)bandwidth_hz *
          (int64_t)RF_V12_MIN_RELIABLE_FREQUENCY_FRACTION_Q15)))
    {
        return false;
    }
    if ((visible_low != frequency_low) || (visible_high != frequency_high))
    {
        flags |= RF_V12_EVENT_FREQUENCY_CLIPPED;
    }

    time_low = center_sample -
               ((int64_t)g_rf_v12_class_contract[class_id].duration_samples / 2LL);
    time_high = time_low +
                (int64_t)g_rf_v12_class_contract[class_id].duration_samples;
    visible_time_low = (time_low < 0LL) ? 0LL : time_low;
    visible_time_high =
        (time_high > (int64_t)RF_V12_TILE_SAMPLES) ?
        (int64_t)RF_V12_TILE_SAMPLES : time_high;
    if (visible_time_high <= visible_time_low)
    {
        return false;
    }
    if ((visible_time_low != time_low) || (visible_time_high != time_high))
    {
        flags |= RF_V12_EVENT_TIME_CLIPPED;
    }
    if ((class_id == RF_V12_CLASS_DJI_VIDEO) &&
        (bandwidth_hz == RF_V31_DJI_VIDEO_20M_BANDWIDTH_HZ))
    {
        flags |= RF_V12_EVENT_VIDEO_20MHZ;
    }

    memset(candidate, 0, sizeof(*candidate));
    candidate->event.frequency_low_offset_hz = (int32_t)visible_low;
    candidate->event.frequency_high_offset_hz = (int32_t)visible_high;
    candidate->event.visible_start_sample = (uint32_t)visible_time_low;
    candidate->event.visible_end_sample = (uint32_t)visible_time_high;
    candidate->event.confidence_q15 = confidence_q15;
    candidate->event.class_id = class_id;
    candidate->event.flags = flags;
    candidate->center_frequency_offset_hz = center_frequency;
    candidate->center_sample = center_sample;
    candidate->bandwidth_hz = bandwidth_hz;
    candidate->state_confidence_q15 = confidence_q15;
    candidate->state_roi_decision = RF_V13_ROI_UNKNOWN;
    candidate->raw_logit = raw_logit;
    return true;
}

static bool rf_v12_candidate_from_center(uint8_t class_id,
                                         int64_t frequency_center_q1,
                                         int64_t time_center_q1,
                                         int8_t raw_logit,
                                         uint32_t bandwidth_hz,
                                         rf_v12_candidate_t *candidate)
{
    const int32_t center_frequency = (int32_t)rf_v12_round_divide(
        (-60000000LL * 102LL) + (frequency_center_q1 * 60000000LL),
        204LL);
    const int32_t center_sample = (int32_t)rf_v12_round_divide(
        time_center_q1 * (int64_t)RF_V12_TILE_SAMPLES,
        116LL);
    const uint16_t confidence_q15 = rf_v12_probability_q15(
        rf_v12_probability(class_id, raw_logit));

    return rf_v12_candidate_from_physical_center(
        class_id,
        center_frequency,
        center_sample,
        confidence_q15,
        raw_logit,
        bandwidth_hz,
        0U,
        candidate);
}

static void rf_v12_candidate_insert(rf_v12_candidate_t *candidates,
                                    uint32_t *count,
                                    uint32_t capacity,
                                    const rf_v12_candidate_t *candidate)
{
    uint32_t insert;
    uint32_t limit;

    if ((candidates == NULL) || (count == NULL) || (candidate == NULL) ||
        (capacity == 0U))
    {
        return;
    }
    insert = (*count < capacity) ? *count : capacity;
    for (uint32_t i = 0U; (i < *count) && (i < capacity); ++i)
    {
        if ((candidate->event.confidence_q15 >
             candidates[i].event.confidence_q15) ||
            ((candidate->event.confidence_q15 ==
              candidates[i].event.confidence_q15) &&
             (candidate->event.class_id < candidates[i].event.class_id)))
        {
            insert = i;
            break;
        }
    }
    if (insert >= capacity)
    {
        return;
    }
    limit = (*count < capacity) ? *count : (capacity - 1U);
    while (limit > insert)
    {
        candidates[limit] = candidates[limit - 1U];
        --limit;
    }
    candidates[insert] = *candidate;
    if (*count < capacity)
    {
        (*count)++;
    }
}

static bool rf_v12_events_overlap(const rf_v12_visible_event_t *left,
                                   const rf_v12_visible_event_t *right)
{
    const int64_t frequency_low =
        (left->frequency_low_offset_hz > right->frequency_low_offset_hz) ?
        left->frequency_low_offset_hz : right->frequency_low_offset_hz;
    const int64_t frequency_high =
        (left->frequency_high_offset_hz < right->frequency_high_offset_hz) ?
        left->frequency_high_offset_hz : right->frequency_high_offset_hz;
    const uint32_t time_low =
        (left->visible_start_sample > right->visible_start_sample) ?
        left->visible_start_sample : right->visible_start_sample;
    const uint32_t time_high =
        (left->visible_end_sample < right->visible_end_sample) ?
        left->visible_end_sample : right->visible_end_sample;
    uint64_t intersection;
    uint64_t left_area;
    uint64_t right_area;
    uint64_t union_area;

    if ((frequency_high <= frequency_low) || (time_high <= time_low))
    {
        return false;
    }
    intersection = (uint64_t)(frequency_high - frequency_low) *
                   (uint64_t)(time_high - time_low);
    left_area = (uint64_t)(left->frequency_high_offset_hz -
                           left->frequency_low_offset_hz) *
                (uint64_t)(left->visible_end_sample - left->visible_start_sample);
    right_area = (uint64_t)(right->frequency_high_offset_hz -
                            right->frequency_low_offset_hz) *
                 (uint64_t)(right->visible_end_sample - right->visible_start_sample);
    union_area = left_area + right_area - intersection;
    return (intersection * (uint64_t)RF_V12_CONFIDENCE_Q15_ONE) >
           (union_area * (uint64_t)RF_V12_NMS_IOU_Q15);
}

static bool rf_v12_local_maximum(const int8_t *heatmap,
                                 uint32_t frequency,
                                 uint32_t time)
{
    const uint32_t index = rf_v12_heatmap_index(frequency, time);
    const int32_t value = heatmap[index];

    for (int32_t df = -1; df <= 1; ++df)
    {
        for (int32_t dt = -1; dt <= 1; ++dt)
        {
            int32_t neighbour_frequency;
            int32_t neighbour_time;
            uint32_t neighbour;

            if ((df == 0) && (dt == 0))
            {
                continue;
            }
            neighbour_frequency = (int32_t)frequency + df;
            neighbour_time = (int32_t)time + dt;
            if ((neighbour_frequency < 0) ||
                (neighbour_frequency >= (int32_t)RF_V12_HEATMAP_FREQUENCY_BINS) ||
                (neighbour_time < 0) ||
                (neighbour_time >= (int32_t)RF_V12_HEATMAP_TIME_BINS))
            {
                continue;
            }
            neighbour = rf_v12_heatmap_index((uint32_t)neighbour_frequency,
                                              (uint32_t)neighbour_time);
            if ((heatmap[neighbour] > value) ||
                ((heatmap[neighbour] == value) && (neighbour < index)))
            {
                return false;
            }
        }
    }
    return true;
}

static uint32_t rf_v31_decode_class(uint8_t class_id,
                                    uint32_t center_index,
                                    rf_v12_candidate_t *output)
{
    const rf_v12_class_contract_t *contract =
        &g_rf_v12_class_contract[class_id];
    const int8_t *heatmap = npu_runner_heatmap(class_id);
    rf_v12_candidate_t top[RF_V12_CANDIDATES_PER_CLASS];
    const uint32_t top_capacity =
        (class_id == RF_V12_CLASS_DJI_VIDEO) ?
        RF_V32_MAX_ROIS_PER_TILE : RF_V12_CANDIDATES_PER_CLASS;
    uint32_t top_count = 0U;
    uint32_t output_count = 0U;

    if ((output == NULL) || (heatmap == NULL) ||
        (center_index >= RF_V12_CENTER_COUNT) ||
        ((contract->center_mask & (1U << center_index)) == 0U))
    {
        return 0U;
    }
    memset(top, 0, sizeof(top));
    for (uint32_t frequency = 0U;
         frequency < RF_V12_HEATMAP_FREQUENCY_BINS;
         ++frequency)
    {
        for (uint32_t time = 0U; time < RF_V12_HEATMAP_TIME_BINS; ++time)
        {
            const int8_t value = heatmap[rf_v12_heatmap_index(frequency, time)];
            rf_v12_candidate_t candidate;

            if (((int32_t)value < contract->threshold) ||
                !rf_v12_local_maximum(heatmap, frequency, time) ||
                !rf_v12_candidate_from_center(
                    class_id,
                    (int64_t)(2U * frequency + 1U),
                    (int64_t)(2U * time + 1U),
                    value,
                    contract->bandwidth_hz,
                    &candidate))
            {
                continue;
            }
            rf_v12_candidate_insert(top,
                                    &top_count,
                                    top_capacity,
                                    &candidate);
        }
    }
    for (uint32_t i = 0U; i < top_count; ++i)
    {
        bool suppressed = false;

        for (uint32_t kept = 0U; kept < output_count; ++kept)
        {
            if (rf_v12_events_overlap(&top[i].event, &output[kept].event))
            {
                suppressed = true;
                break;
            }
        }
        if (!suppressed)
        {
            output[output_count++] = top[i];
        }
    }
    return output_count;
}

static int32_t rf_v12_abs_i32(int32_t value)
{
    return (value < 0) ? -value : value;
}

static bool rf_v32_track_expired(const rf_v32_detector_track_t *track,
                                 uint32_t tile_sequence)
{
    const uint32_t age = tile_sequence - track->last_tile_sequence;

    return (track->active != 0U) && (age < UINT32_C(0x80000000)) &&
           (age > RF_V32_WIDTH_TRACK_EXPIRE_TILES);
}

static rf_v32_width_track_t *rf_v32_track_for_proposal(
    uint8_t center_slot,
    int32_t center_frequency_offset_hz,
    uint32_t tile_sequence)
{
    rf_v32_detector_track_t *selected = NULL;
    int32_t selected_distance = INT32_MAX;
    uint32_t oldest_age = 0U;

    for (uint32_t i = 0U; i < RF_V32_WIDTH_TRACK_COUNT; ++i)
    {
        rf_v32_detector_track_t *track = &g_rf_v32_width_tracks[i];

        if (rf_v32_track_expired(track, tile_sequence))
        {
            rf_v32_width_track_init(&track->state);
            track->active = 0U;
        }
        if ((track->active != 0U) && (track->center_slot == center_slot))
        {
            const int32_t distance = rf_v12_abs_i32(
                track->center_frequency_offset_hz - center_frequency_offset_hz);

            if ((distance <= RF_V32_WIDTH_TRACK_MATCH_HZ) &&
                (distance < selected_distance))
            {
                selected = track;
                selected_distance = distance;
            }
        }
    }

    if (selected == NULL)
    {
        for (uint32_t i = 0U; i < RF_V32_WIDTH_TRACK_COUNT; ++i)
        {
            if (g_rf_v32_width_tracks[i].active == 0U)
            {
                selected = &g_rf_v32_width_tracks[i];
                break;
            }
        }
    }
    if (selected == NULL)
    {
        selected = &g_rf_v32_width_tracks[0];
        for (uint32_t i = 0U; i < RF_V32_WIDTH_TRACK_COUNT; ++i)
        {
            const uint32_t age =
                tile_sequence - g_rf_v32_width_tracks[i].last_tile_sequence;

            if ((age < UINT32_C(0x80000000)) && (age >= oldest_age))
            {
                selected = &g_rf_v32_width_tracks[i];
                oldest_age = age;
            }
        }
    }

    if ((selected->active == 0U) ||
        (selected->center_slot != center_slot) ||
        (rf_v12_abs_i32(selected->center_frequency_offset_hz -
                        center_frequency_offset_hz) >
         RF_V32_WIDTH_TRACK_MATCH_HZ))
    {
        rf_v32_width_track_init(&selected->state);
    }
    selected->active = 1U;
    selected->center_slot = center_slot;
    selected->center_frequency_offset_hz = center_frequency_offset_hz;
    selected->last_tile_sequence = tile_sequence;
    return &selected->state;
}

static void rf_v31_build_video_guard_features(
    const rf_v12_candidate_t *candidate,
    const rf_v16_postprocess_result_t *roi,
    const rf_v20_video_postprocess_result_t *video,
    uint32_t center_index,
    uint8_t event_flags,
    int32_t features[RF_V26_GUARD_FEATURE_COUNT])
{
    const int32_t left = video->width.left_shoulder_contrast_codes;
    const int32_t right = video->width.right_shoulder_contrast_codes;

    memset(features, 0,
           sizeof(int32_t) * RF_V26_GUARD_FEATURE_COUNT);
    features[0] = candidate->event.confidence_q15;
    features[1] = roi->display_score_q8;
    features[2] = roi->statistics.burstiness_q8;
    features[3] = roi->statistics.contrast_q8;
    features[4] = roi->statistics.frequency_edge_q8;
    features[5] = roi->statistics.time_edge_q8;
    features[6] = roi->statistics.occupancy_q15;
    features[7] = roi->statistics.texture_q8;
    features[8] = roi->statistics.visible_fraction_q15;
    features[9] = video->width.current_10mhz_contrast_codes;
    features[10] = video->width.best_20mhz_contrast_codes;
    features[11] = (left < right) ? left : right;
    features[12] = rf_v12_abs_i32(left - right);
    features[13] = (center_index >= 2U) ? 1 : 0;
    features[14] =
        (video->event.bandwidth_hz == RF_V20_VIDEO_20MHZ_HZ) ? 1 : 0;
    features[15] = (center_index == 1U) ? 1 : 0;
    features[16] =
        ((event_flags & RF_V12_EVENT_FREQUENCY_CLIPPED) != 0U) ? 1 : 0;
    features[17] =
        ((event_flags & RF_V12_EVENT_TIME_CLIPPED) != 0U) ? 1 : 0;
}

static bool rf_v31_refine_candidate(const rf_v12_detector_input_t *input,
                                    rf_v12_candidate_t *candidate)
{
    rf_v16_candidate_t source;
    rf_v16_postprocess_result_t roi;
    rf_v12_candidate_t refined;
    int32_t center_frequency;
    int32_t center_sample;
    uint32_t bandwidth_hz;

    if ((input == NULL) || (candidate == NULL))
    {
        return false;
    }
    if (input->model_input == NULL)
    {
        if (candidate->event.class_id == RF_V12_CLASS_DJI_VIDEO)
        {
            return false;
        }
        candidate->state_roi_decision = RF_V13_ROI_PASS;
        candidate->state_quality_tier = RF_V18_QUALITY_STRONG;
        return true;
    }

    memset(&source, 0, sizeof(source));
    source.center_frequency_offset_hz = candidate->center_frequency_offset_hz;
    source.center_sample = candidate->center_sample;
    source.confidence_q15 = candidate->event.confidence_q15;
    source.class_id = candidate->event.class_id;
    source.event_flags = candidate->event.flags;
    if (!rf_v17_refine_candidate(input->model_input,
                                 RF_V12_FEATURE_BYTES,
                                 &source,
                                 g_rf_v16_class_configs,
                                 g_rf_v17_subbin_configs,
                                 &roi))
    {
        return false;
    }

    center_frequency = candidate->center_frequency_offset_hz +
                       roi.frequency_shift_hz;
    center_sample = candidate->center_sample + roi.time_shift_samples;
    bandwidth_hz = candidate->bandwidth_hz;

    if (candidate->event.class_id == RF_V12_CLASS_DJI_VIDEO)
    {
        rf_v20_video_event_t video_event;
        rf_v20_video_postprocess_result_t video;
        rf_v12_candidate_t guard_geometry;
        int32_t guard_features[RF_V26_GUARD_FEATURE_COUNT];
        int32_t selected_width = 0;
        rf_v32_width_track_t *width_track;
        const rf_v26_guard_model_t *guard_model =
            (input->center_index >= 2U) ?
            &g_rf_v26_guard_5g8 : &g_rf_v26_guard_2g4;

        video_event.center_frequency_offset_hz = center_frequency;
        video_event.center_sample = center_sample;
        video_event.bandwidth_hz = RF_V20_VIDEO_10MHZ_HZ;
        if (!rf_v20_video_postprocess(input->model_input,
                                      RF_V12_FEATURE_BYTES,
                                      input->center_frequency_hz,
                                      &video_event,
                                      &video))
        {
            return false;
        }
        center_frequency = video.event.center_frequency_offset_hz;
        center_sample = video.event.center_sample;
        bandwidth_hz = (uint32_t)video.event.bandwidth_hz;
        if (!rf_v12_candidate_from_physical_center(
                candidate->event.class_id,
                center_frequency,
                center_sample,
                candidate->event.confidence_q15,
                candidate->raw_logit,
                bandwidth_hz,
                0U,
                &guard_geometry))
        {
            return false;
        }
        rf_v31_build_video_guard_features(candidate,
                                          &roi,
                                          &video,
                                          input->center_index,
                                          guard_geometry.event.flags,
                                          guard_features);
        if (!rf_v26_guard_accept(guard_model, guard_features))
        {
            return false;
        }

        width_track = rf_v32_track_for_proposal(
            (uint8_t)input->center_index,
            center_frequency,
            input->tile_sequence);
        if (!npu_runner_classify_video_width(input->model_input,
                                             RF_V12_FEATURE_BYTES,
                                             center_frequency,
                                             center_sample,
                                             width_track,
                                             &selected_width) ||
            ((selected_width != RF_V20_VIDEO_10MHZ_HZ) &&
             (selected_width != RF_V20_VIDEO_20MHZ_HZ)))
        {
            return false;
        }
        bandwidth_hz = (uint32_t)selected_width;
    }

    if (!rf_v12_candidate_from_physical_center(
            candidate->event.class_id,
            center_frequency,
            center_sample,
            candidate->event.confidence_q15,
            candidate->raw_logit,
            bandwidth_hz,
            0U,
            &refined))
    {
        return false;
    }
    refined.state_confidence_q15 = candidate->event.confidence_q15;
    refined.state_roi_decision = RF_V13_ROI_PASS;
    refined.state_quality_tier = RF_V18_QUALITY_STRONG;
    *candidate = refined;
    return true;
}

static void rf_v12_build_display_mask(uint8_t *mask, uint32_t center_index)
{
    memset(mask, 0, RA8P1_DISPLAY_MASK_BYTES);
    for (uint32_t class_id = 0U; class_id < RF_V12_CLASS_COUNT; ++class_id)
    {
        const int8_t *heatmap = npu_runner_heatmap(class_id);
        const rf_v12_class_contract_t *contract =
            &g_rf_v12_class_contract[class_id];

        if ((heatmap == NULL) || (center_index >= RF_V12_CENTER_COUNT) ||
            ((contract->center_mask & (1U << center_index)) == 0U))
        {
            continue;
        }
        for (uint32_t frequency = 0U;
             frequency < RF_V12_HEATMAP_FREQUENCY_BINS;
             ++frequency)
        {
            for (uint32_t time = 0U; time < RF_V12_HEATMAP_TIME_BINS; ++time)
            {
                if ((int32_t)heatmap[rf_v12_heatmap_index(frequency, time)] >=
                    contract->threshold)
                {
                    const uint32_t x =
                        (frequency * RA8P1_DISPLAY_MASK_WIDTH) /
                        RF_V12_HEATMAP_FREQUENCY_BINS;
                    const uint32_t y =
                        (time * RA8P1_DISPLAY_MASK_HEIGHT) /
                        RF_V12_HEATMAP_TIME_BINS;
                    const uint32_t index =
                        y * RA8P1_DISPLAY_MASK_WIDTH + x;

                    mask[index >> 3U] |= (uint8_t)(1U << (index & 7U));
                }
            }
        }
    }
}

void rf_v12_detector_decode(const rf_v12_detector_input_t *input,
                            rf_v12_detector_result_t *result)
{
    rf_v12_candidate_t candidates[RF_V12_GLOBAL_CANDIDATES];
    uint32_t candidate_count = 0U;

    if ((input == NULL) || (result == NULL))
    {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->best_class_id = UINT8_MAX;
    result->tile.magic = RF_V12_ABI_MAGIC;
    result->tile.abi_version_major = RF_V12_ABI_VERSION_MAJOR;
    result->tile.abi_version_minor = RF_V12_ABI_VERSION_MINOR;
    result->tile.sequence = input->tile_sequence;
    result->tile.round_index = input->round_index;
    result->tile.capture_center_frequency_hz = input->center_frequency_hz;
    result->tile.capture_start_time_us = input->capture_start_time_us;
    result->tile.capture_end_time_us = input->capture_end_time_us;
    result->tile.sample_rate_hz = RF_V12_SAMPLE_RATE_HZ;
    result->tile.tile_samples = RF_V12_TILE_SAMPLES;
    result->tile.center_index = (uint8_t)input->center_index;
    result->tile.round_valid_center_mask =
        (input->center_index < RF_V12_CENTER_COUNT) ?
        (uint8_t)(1U << input->center_index) : 0U;
    result->tile.tile_validity = input->tile_validity;
    result->tile.round_observation = RF_V12_ROUND_INCOMPLETE;
    result->tile.flags = input->tile_flags;
    result->tile.background_generation = 0U;
    result->tile.sdr_gain_db_q8 = input->sdr_gain_db_q8;

    if ((input->tile_validity != RF_V12_TILE_VALID) ||
        (input->tile_flags != 0U) ||
        (input->center_index >= RF_V12_CENTER_COUNT))
    {
        return;
    }

    memset(candidates, 0, sizeof(candidates));
    for (uint8_t class_id = 0U; class_id < RF_V12_CLASS_COUNT; ++class_id)
    {
        rf_v12_candidate_t per_class[RF_V12_CANDIDATES_PER_CLASS];
        const uint32_t count = rf_v31_decode_class(class_id,
                                                   input->center_index,
                                                   per_class);

        for (uint32_t i = 0U; i < count; ++i)
        {
            if (rf_v31_refine_candidate(input, &per_class[i]))
            {
                rf_v12_candidate_insert(candidates,
                                        &candidate_count,
                                        RF_V12_GLOBAL_CANDIDATES,
                                        &per_class[i]);
            }
        }
    }

    if (candidate_count > RF_V12_MAX_BOXES_PER_TILE)
    {
        result->tile.flags |= RF_V12_TILE_RESULT_TRUNCATED;
        candidate_count = RF_V12_MAX_BOXES_PER_TILE;
    }
    result->tile.event_count = (uint16_t)candidate_count;
    result->tile.round_observation = (candidate_count != 0U) ?
        RF_V12_ROUND_TARGET_RF_OBSERVED : RF_V12_ROUND_INCOMPLETE;
    for (uint32_t i = 0U; i < candidate_count; ++i)
    {
        const uint8_t object_id =
            rf_v12_class_to_object(candidates[i].event.class_id);

        candidates[i].event.track_id = (input->tile_sequence << 3U) | i;
        result->tile.events[i] = candidates[i].event;
        result->state_confidence_q15[i] =
            candidates[i].state_confidence_q15;
        result->state_roi_decision[i] =
            candidates[i].state_roi_decision;
        result->state_quality_tier[i] =
            candidates[i].state_quality_tier;
        if ((object_id < RF_V12_OBJECT_COUNT) &&
            (candidates[i].event.confidence_q15 >
             result->object_presence_q15[object_id]))
        {
            result->object_presence_q15[object_id] =
                candidates[i].event.confidence_q15;
        }
    }
    if (candidate_count != 0U)
    {
        result->best_class_id = candidates[0].event.class_id;
        result->best_score_q15 = candidates[0].event.confidence_q15;
    }
    rf_v12_build_display_mask(result->display_mask, input->center_index);
}
