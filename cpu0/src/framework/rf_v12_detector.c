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

#define RF_V12_CANDIDATES_PER_CLASS (RF_V12_PER_CLASS_PEAK_TOP_K)
#define RF_V12_GLOBAL_CANDIDATES (RF_V12_PREFILTER_GLOBAL_TOP_K)
#define RF_V12_VIDEO_COMPONENT_LIMIT (6U)
#define RF_V12_VIDEO_FAR_VALUES_MAX \
    (RF_V12_FEATURE_FREQUENCY_BINS * RF_V12_FEATURE_TIME_BINS)
#define RF_V12_ONE_DB (1.0F)
#define RF_V12_WORKSPACE __attribute__((section(".sdram_noinit"), aligned(32), used))

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

static const rf_v12_class_contract_t g_rf_v12_class_contract[RF_V12_CLASS_COUNT] =
{
    {RF_V21_NONVIDEO_DJI_CONTROL_SCALE,
     RF_V21_NONVIDEO_DJI_CONTROL_ZERO_POINT,
     RF_V21_NONVIDEO_DJI_CONTROL_THRESHOLD,
     RF_V12_DJI_CONTROL_BANDWIDTH_HZ,
     RF_V12_DJI_CONTROL_DURATION_SAMPLES, RF_V12_CLASS_CENTER_MASK_DJI_CONTROL},
    {RF_V20_V3_VIDEO_SCALE, RF_V20_V3_VIDEO_ZERO_POINT,
     RF_V20_V3_VIDEO_THRESHOLD, RF_V12_DJI_VIDEO_10M_BANDWIDTH_HZ,
     RF_V12_DJI_VIDEO_DURATION_SAMPLES, RF_V12_CLASS_CENTER_MASK_DJI_VIDEO},
    {RF_V21_NONVIDEO_AT9S_SCALE, RF_V21_NONVIDEO_AT9S_ZERO_POINT,
     RF_V21_NONVIDEO_AT9S_THRESHOLD, RF_V12_AT9S_BANDWIDTH_HZ,
     RF_V12_AT9S_DURATION_SAMPLES, RF_V12_CLASS_CENTER_MASK_AT9S},
    {RF_V21_NONVIDEO_T12_SCALE, RF_V21_NONVIDEO_T12_ZERO_POINT,
     RF_V21_NONVIDEO_T12_THRESHOLD, RF_V12_T12_BANDWIDTH_HZ,
     RF_V12_T12_DURATION_SAMPLES, RF_V12_CLASS_CENTER_MASK_T12},
    {RF_V21_NONVIDEO_XIAOBAWANG_SCALE,
     RF_V21_NONVIDEO_XIAOBAWANG_ZERO_POINT,
     RF_V21_NONVIDEO_XIAOBAWANG_THRESHOLD,
     RF_V12_XIAOBAWANG_BANDWIDTH_HZ,
     RF_V12_XIAOBAWANG_DURATION_SAMPLES, RF_V12_CLASS_CENTER_MASK_XIAOBAWANG}
};

static uint8_t g_component_visited[(RF_V12_HEATMAP_BYTES + 7U) / 8U]
    RF_V12_WORKSPACE;
static uint16_t g_component_queue[RF_V12_HEATMAP_BYTES] RF_V12_WORKSPACE;
static float g_video_far_values[RF_V12_VIDEO_FAR_VALUES_MAX] RF_V12_WORKSPACE;

static const uint16_t g_rf_v19_minimum_confidence_q15[RF_V12_CLASS_COUNT] =
{
    12132U, 0U, 0U, 8697U, 11113U
};

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
        (bandwidth_hz == RF_V12_DJI_VIDEO_20M_BANDWIDTH_HZ))
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
    insert = *count;
    if (insert > capacity)
    {
        insert = capacity;
    }
    for (uint32_t i = 0U; i < *count && i < capacity; ++i)
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
    int64_t frequency_low =
        (left->frequency_low_offset_hz > right->frequency_low_offset_hz) ?
        left->frequency_low_offset_hz : right->frequency_low_offset_hz;
    int64_t frequency_high =
        (left->frequency_high_offset_hz < right->frequency_high_offset_hz) ?
        left->frequency_high_offset_hz : right->frequency_high_offset_hz;
    uint32_t time_low = (left->visible_start_sample > right->visible_start_sample) ?
                        left->visible_start_sample : right->visible_start_sample;
    uint32_t time_high = (left->visible_end_sample < right->visible_end_sample) ?
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

static uint32_t rf_v21_decode_nonvideo_class(uint8_t class_id,
                                             uint32_t center_index,
                                             rf_v12_candidate_t *output)
{
    const rf_v12_class_contract_t *contract =
        &g_rf_v12_class_contract[class_id];
    const int8_t *heatmap = npu_runner_heatmap(class_id);
    rf_v12_candidate_t top[RF_V12_CANDIDATES_PER_CLASS];
    uint32_t top_count = 0U;
    uint32_t output_count = 0U;

    if ((heatmap == NULL) ||
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
            rf_v12_candidate_insert(top, &top_count,
                                    RF_V12_CANDIDATES_PER_CLASS,
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

static bool rf_v12_visited(uint32_t index)
{
    return (g_component_visited[index >> 3U] &
            (uint8_t)(1U << (index & 7U))) != 0U;
}

static void rf_v12_visit(uint32_t index)
{
    g_component_visited[index >> 3U] |=
        (uint8_t)(1U << (index & 7U));
}

static float rf_v12_quickselect(float *values, uint32_t count, uint32_t target)
{
    uint32_t left = 0U;
    uint32_t right = count - 1U;
    while (left < right)
    {
        float pivot = values[(left + right) / 2U];
        uint32_t i = left;
        uint32_t j = right;
        while (i <= j)
        {
            while (values[i] < pivot) ++i;
            while (values[j] > pivot)
            {
                if (j == 0U) break;
                --j;
            }
            if (i <= j)
            {
                float temporary = values[i];
                values[i] = values[j];
                values[j] = temporary;
                ++i;
                if (j == 0U) break;
                --j;
            }
        }
        if (target <= j)
        {
            right = j;
        }
        else if (target >= i)
        {
            left = i;
        }
        else
        {
            break;
        }
    }
    return values[target];
}

static float rf_v12_median(float *values, uint32_t count)
{
    const uint32_t upper = count / 2U;
    float upper_value;
    if ((values == NULL) || (count == 0U))
    {
        return 0.0F;
    }
    upper_value = rf_v12_quickselect(values, count, upper);
    if ((count & 1U) != 0U)
    {
        return upper_value;
    }
    return 0.5F *
           (upper_value + rf_v12_quickselect(values, count, upper - 1U));
}

static bool rf_v12_video_near_reliable_edge(uint32_t minimum_frequency,
                                             uint32_t maximum_frequency)
{
    const int64_t lower_edge_hz = rf_v12_round_divide(
        (-30000000LL * RF_V12_HEATMAP_FREQUENCY_BINS) +
        ((int64_t)minimum_frequency * 60000000LL),
        RF_V12_HEATMAP_FREQUENCY_BINS);
    const int64_t upper_edge_hz = rf_v12_round_divide(
        (-30000000LL * RF_V12_HEATMAP_FREQUENCY_BINS) +
        ((int64_t)(maximum_frequency + 1U) * 60000000LL),
        RF_V12_HEATMAP_FREQUENCY_BINS);
    const int64_t snap_hz = rf_v12_round_divide(
        (int64_t)RF_V12_VIDEO_COMPONENT_EDGE_SNAP_ROWS * 60000000LL,
        RF_V12_HEATMAP_FREQUENCY_BINS);

    return (lower_edge_hz <= (-28000000LL + snap_hz)) ||
           (upper_edge_hz >= (28000000LL - snap_hz));
}

static uint32_t rf_v12_video_bandwidth(const float *c0,
                                        uint32_t minimum_frequency,
                                        uint32_t maximum_frequency,
                                        uint32_t minimum_time,
                                        uint32_t maximum_time)
{
    int64_t component_center_q1 =
        (int64_t)minimum_frequency + (int64_t)maximum_frequency + 1LL;
    int64_t component_center_hz = rf_v12_round_divide(
        (-60000000LL * 102LL) + component_center_q1 * 60000000LL,
        204LL);
    uint32_t input_time_low = minimum_time * 2U;
    uint32_t input_time_high = ((maximum_time + 1U) * 2U) - 1U;
    uint32_t far_count = 0U;
    uint32_t shoulder_count = 0U;
    uint32_t occupied_count = 0U;
    float far_median;

    if (rf_v12_video_near_reliable_edge(minimum_frequency,
                                         maximum_frequency) ||
        (c0 == NULL))
    {
        return RF_V12_DJI_VIDEO_20M_BANDWIDTH_HZ;
    }
    input_time_low = (input_time_low > RF_V12_VIDEO_PROFILE_TIME_RADIUS) ?
                     input_time_low - RF_V12_VIDEO_PROFILE_TIME_RADIUS : 0U;
    input_time_high += RF_V12_VIDEO_PROFILE_TIME_RADIUS;
    if (input_time_high >= RF_V12_FEATURE_TIME_BINS)
    {
        input_time_high = RF_V12_FEATURE_TIME_BINS - 1U;
    }

    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        int64_t frequency_hz = rf_v12_round_divide(
            (-60000000LL * 204LL) +
            (int64_t)(2U * frequency + 1U) * 60000000LL,
            408LL);
        uint64_t distance = (frequency_hz >= component_center_hz) ?
                            (uint64_t)(frequency_hz - component_center_hz) :
                            (uint64_t)(component_center_hz - frequency_hz);
        for (uint32_t time = input_time_low; time <= input_time_high; ++time)
        {
            const float value =
                c0[(frequency * RF_V12_FEATURE_TIME_BINS) + time];
            if ((distance >= 11000000ULL) && (distance <= 14000000ULL) &&
                (far_count < RF_V12_VIDEO_FAR_VALUES_MAX))
            {
                g_video_far_values[far_count++] = value;
            }
        }
    }
    if (far_count == 0U)
    {
        return RF_V12_DJI_VIDEO_10M_BANDWIDTH_HZ;
    }
    far_median = rf_v12_median(g_video_far_values, far_count);

    for (uint32_t frequency = 0U;
         frequency < RF_V12_FEATURE_FREQUENCY_BINS;
         ++frequency)
    {
        int64_t frequency_hz = rf_v12_round_divide(
            (-60000000LL * 204LL) +
            (int64_t)(2U * frequency + 1U) * 60000000LL,
            408LL);
        uint64_t distance = (frequency_hz >= component_center_hz) ?
                            (uint64_t)(frequency_hz - component_center_hz) :
                            (uint64_t)(component_center_hz - frequency_hz);
        if ((distance < 5500000ULL) || (distance > 9500000ULL))
        {
            continue;
        }
        for (uint32_t time = input_time_low; time <= input_time_high; ++time)
        {
            const float value =
                c0[(frequency * RF_V12_FEATURE_TIME_BINS) + time];
            shoulder_count++;
            if (value >= (far_median + RF_V12_ONE_DB))
            {
                occupied_count++;
            }
        }
    }
    return ((shoulder_count != 0U) &&
            ((occupied_count * RF_V12_CONFIDENCE_Q15_ONE) >=
             (shoulder_count * RF_V12_VIDEO_OCCUPIED_FRACTION_Q15))) ?
           RF_V12_DJI_VIDEO_20M_BANDWIDTH_HZ :
           RF_V12_DJI_VIDEO_10M_BANDWIDTH_HZ;
}

static uint32_t rf_v12_decode_video(const float *c0,
                                    rf_v12_candidate_t *output)
{
    const int8_t *heatmap = npu_runner_heatmap(RF_V12_CLASS_DJI_VIDEO);
    rf_v12_candidate_t top[RF_V12_VIDEO_COMPONENT_LIMIT];
    uint32_t top_count = 0U;
    uint32_t output_count = 0U;
    if (heatmap == NULL)
    {
        return 0U;
    }
    memset(g_component_visited, 0, sizeof(g_component_visited));
    memset(top, 0, sizeof(top));

    for (uint32_t start = 0U; start < RF_V12_HEATMAP_BYTES; ++start)
    {
        uint32_t head = 0U;
        uint32_t tail = 0U;
        uint32_t minimum_frequency = RF_V12_HEATMAP_FREQUENCY_BINS;
        uint32_t maximum_frequency = 0U;
        uint32_t minimum_time = RF_V12_HEATMAP_TIME_BINS;
        uint32_t maximum_time = 0U;
        uint32_t histogram[256] = {0U};
        uint32_t percentile_target;
        uint32_t percentile_value = 0U;
        uint32_t bandwidth;
        rf_v12_candidate_t candidate;

        if (rf_v12_visited(start) ||
            !rf_v20_video_heatmap_accepts(heatmap[start]))
        {
            continue;
        }
        rf_v12_visit(start);
        g_component_queue[tail++] = (uint16_t)start;
        while (head < tail)
        {
            uint32_t index = g_component_queue[head++];
            uint32_t frequency = index / RF_V12_HEATMAP_TIME_BINS;
            uint32_t time = index % RF_V12_HEATMAP_TIME_BINS;
            uint8_t histogram_index = (uint8_t)heatmap[index] + 128U;
            histogram[histogram_index]++;
            if (frequency < minimum_frequency) minimum_frequency = frequency;
            if (frequency > maximum_frequency) maximum_frequency = frequency;
            if (time < minimum_time) minimum_time = time;
            if (time > maximum_time) maximum_time = time;

            for (int32_t df = -1; df <= 1; ++df)
            {
                for (int32_t dt = -1; dt <= 1; ++dt)
                {
                    int32_t nf;
                    int32_t nt;
                    uint32_t neighbour;
                    if ((df == 0) && (dt == 0)) continue;
                    nf = (int32_t)frequency + df;
                    nt = (int32_t)time + dt;
                    if ((nf < 0) ||
                        (nf >= (int32_t)RF_V12_HEATMAP_FREQUENCY_BINS) ||
                        (nt < 0) ||
                        (nt >= (int32_t)RF_V12_HEATMAP_TIME_BINS))
                    {
                        continue;
                    }
                    neighbour = rf_v12_heatmap_index((uint32_t)nf,
                                                      (uint32_t)nt);
                    if (!rf_v12_visited(neighbour) &&
                        rf_v20_video_heatmap_accepts(heatmap[neighbour]))
                    {
                        rf_v12_visit(neighbour);
                        g_component_queue[tail++] = (uint16_t)neighbour;
                    }
                }
            }
        }

        if ((tail < RF_V12_VIDEO_COMPONENT_MIN_AREA) ||
            (((maximum_frequency - minimum_frequency) + 1U) <
             RF_V12_VIDEO_COMPONENT_MIN_FREQUENCY_HEIGHT) ||
            ((minimum_time == maximum_time) &&
             (minimum_time != 0U) &&
             (maximum_time != (RF_V12_HEATMAP_TIME_BINS - 1U))))
        {
            continue;
        }

        percentile_target = ((tail * 9U) + 9U) / 10U;
        {
            uint32_t cumulative = 0U;
            for (uint32_t value = 0U; value < 256U; ++value)
            {
                cumulative += histogram[value];
                if (cumulative >= percentile_target)
                {
                    percentile_value = value;
                    break;
                }
            }
        }
        bandwidth = rf_v12_video_bandwidth(c0,
                                            minimum_frequency,
                                            maximum_frequency,
                                            minimum_time,
                                            maximum_time);
        if (rf_v12_candidate_from_center(
                RF_V12_CLASS_DJI_VIDEO,
                (int64_t)minimum_frequency + (int64_t)maximum_frequency + 1LL,
                (int64_t)minimum_time + (int64_t)maximum_time + 1LL,
                (int8_t)(percentile_value - 128U),
                bandwidth,
                &candidate))
        {
            rf_v12_candidate_insert(top, &top_count,
                                    RF_V12_VIDEO_COMPONENT_LIMIT,
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

static uint16_t rf_v20_video_state_confidence_q15(int8_t raw_logit)
{
    static const uint16_t tier_confidence_q15[4] =
    {
        0U, 18022U, 24575U, 29490U
    };
    return tier_confidence_q15[rf_v20_video_score_tier(raw_logit)];
}

static bool rf_v21_refine_candidate(const rf_v12_detector_input_t *input,
                                    rf_v12_candidate_t *candidate)
{
    rf_v16_candidate_t source;
    rf_v16_postprocess_result_t roi;
    rf_v12_candidate_t refined;
    int32_t center_frequency;
    int32_t center_sample;
    uint32_t bandwidth_hz;
    uint16_t state_confidence_q15;
    uint8_t state_roi_decision;
    uint8_t state_quality_tier;
    uint8_t extra_flags = 0U;

    if ((input == NULL) || (candidate == NULL))
    {
        return false;
    }
    /* Host decoder tests can omit features to exercise the sparse proposal
     * contract in isolation. Production valid tiles always provide them. */
    if (input->model_input == NULL)
    {
        return true;
    }

    memset(&source, 0, sizeof(source));
    source.center_frequency_offset_hz =
        candidate->center_frequency_offset_hz;
    source.center_sample = candidate->center_sample;
    source.confidence_q15 = candidate->event.confidence_q15;
    source.class_id = candidate->event.class_id;
    source.event_flags = candidate->event.flags;
    if (!rf_v17_postprocess_candidate(input->model_input,
                                      RF_V12_FEATURE_BYTES,
                                      &source,
                                      g_rf_v16_class_configs,
                                      g_rf_v17_subbin_configs,
                                      &roi) ||
        (roi.display_accept == 0U))
    {
        return false;
    }

    if ((candidate->event.class_id != RF_V12_CLASS_DJI_VIDEO) &&
        (candidate->event.confidence_q15 <
         g_rf_v19_minimum_confidence_q15[candidate->event.class_id]))
    {
        return false;
    }

    center_frequency = candidate->center_frequency_offset_hz +
                       roi.frequency_shift_hz;
    center_sample = candidate->center_sample + roi.time_shift_samples;
    bandwidth_hz = candidate->bandwidth_hz;
    state_confidence_q15 = candidate->event.confidence_q15;
    state_roi_decision = roi.state_roi_decision;

    if (candidate->event.class_id == RF_V12_CLASS_DJI_VIDEO)
    {
        rf_v20_video_event_t video_event;
        rf_v20_video_postprocess_result_t video;

        if (roi.display_score_q8 < RF_V20_VIDEO_DISPLAY_SCORE_Q8)
        {
            return false;
        }
        video_event.center_frequency_offset_hz = center_frequency;
        video_event.center_sample = center_sample;
        video_event.bandwidth_hz = (int32_t)bandwidth_hz;
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
        state_confidence_q15 =
            rf_v20_video_state_confidence_q15(candidate->raw_logit);
        if (state_confidence_q15 == 0U)
        {
            state_roi_decision = RF_V13_ROI_FAIL;
        }
    }
    if (state_roi_decision == RF_V13_ROI_FAIL)
    {
        extra_flags |= RF_V12_EVENT_NEEDS_REVIEW;
    }
    state_quality_tier = rf_v18_state_quality_tier(
        candidate->event.class_id,
        RF_V18_SOURCE_PRIMARY,
        state_confidence_q15,
        state_roi_decision,
        &roi.statistics);

    if (!rf_v12_candidate_from_physical_center(
            candidate->event.class_id,
            center_frequency,
            center_sample,
            candidate->event.confidence_q15,
            candidate->raw_logit,
            bandwidth_hz,
            extra_flags,
            &refined))
    {
        return false;
    }
    refined.state_confidence_q15 = state_confidence_q15;
    refined.state_roi_decision = state_roi_decision;
    refined.state_quality_tier = state_quality_tier;
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
        const int32_t threshold = contract->threshold;
        if ((heatmap == NULL) ||
            (center_index >= RF_V12_CENTER_COUNT) ||
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
                    threshold)
                {
                    const uint32_t x =
                        (frequency * RA8P1_DISPLAY_MASK_WIDTH) /
                        RF_V12_HEATMAP_FREQUENCY_BINS;
                    const uint32_t y =
                        (time * RA8P1_DISPLAY_MASK_HEIGHT) /
                        RF_V12_HEATMAP_TIME_BINS;
                    const uint32_t index = y * RA8P1_DISPLAY_MASK_WIDTH + x;
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
    result->tile.background_generation = input->background_generation;
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
        uint32_t count;
        if (class_id == RF_V12_CLASS_DJI_VIDEO)
        {
            count = rf_v12_decode_video(input->background_relative_c0,
                                        per_class);
        }
        else
        {
            count = rf_v21_decode_nonvideo_class(class_id,
                                                 input->center_index,
                                                 per_class);
        }
        for (uint32_t i = 0U; i < count; ++i)
        {
            if (rf_v21_refine_candidate(input, &per_class[i]))
            {
                rf_v12_candidate_insert(candidates, &candidate_count,
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
        uint8_t object_id =
            rf_v12_class_to_object(candidates[i].event.class_id);
        candidates[i].event.track_id = (input->tile_sequence << 3U) | i;
        result->tile.events[i] = candidates[i].event;
        result->state_confidence_q15[i] =
            candidates[i].state_confidence_q15;
        result->state_roi_decision[i] = candidates[i].state_roi_decision;
        result->state_quality_tier[i] = candidates[i].state_quality_tier;
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
