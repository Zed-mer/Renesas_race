#include "rf_v26_partition_guard.h"

#include <stddef.h>

const rf_v26_guard_model_t g_rf_v26_guard_2g4 = {
    {0, 0, 7935, 619, 1334, 821, 50, -9271, 191, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    -14833610,
    -1590006};

const rf_v26_guard_model_t g_rf_v26_guard_5g8 = {
    {0, 0, 117, -125, 1284, -835, 420, -10379, 94, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    -12789716,
    -1590006};

int64_t rf_v26_guard_score_q20(
    const rf_v26_guard_model_t *model,
    const int32_t raw_features[RF_V26_GUARD_FEATURE_COUNT])
{
    int64_t score;
    unsigned index;
    if (model == NULL || raw_features == NULL) {
        return INT64_MIN;
    }
    score = (int64_t)model->intercept_q20;
    for (index = 0u; index < RF_V26_GUARD_FEATURE_COUNT; ++index) {
        score += (int64_t)model->weights_q20[index] *
                 (int64_t)raw_features[index];
    }
    return score;
}

int rf_v26_guard_accept(
    const rf_v26_guard_model_t *model,
    const int32_t raw_features[RF_V26_GUARD_FEATURE_COUNT])
{
    int64_t score = rf_v26_guard_score_q20(model, raw_features);
    return score >= (int64_t)model->threshold_logit_q20 ? 1 : 0;
}
