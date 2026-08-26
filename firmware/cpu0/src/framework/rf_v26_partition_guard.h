#ifndef RF_V26_PARTITION_GUARD_H
#define RF_V26_PARTITION_GUARD_H

#include <stdint.h>

#define RF_V26_GUARD_FEATURE_COUNT 18u

typedef struct {
    int32_t weights_q20[RF_V26_GUARD_FEATURE_COUNT];
    int32_t intercept_q20;
    int32_t threshold_logit_q20;
} rf_v26_guard_model_t;

extern const rf_v26_guard_model_t g_rf_v26_guard_2g4;
extern const rf_v26_guard_model_t g_rf_v26_guard_5g8;

int64_t rf_v26_guard_score_q20(
    const rf_v26_guard_model_t *model,
    const int32_t raw_features[RF_V26_GUARD_FEATURE_COUNT]);

/* Returns 1 when the fixed-point CPU guard accepts the event. */
int rf_v26_guard_accept(
    const rf_v26_guard_model_t *model,
    const int32_t raw_features[RF_V26_GUARD_FEATURE_COUNT]);

#endif
