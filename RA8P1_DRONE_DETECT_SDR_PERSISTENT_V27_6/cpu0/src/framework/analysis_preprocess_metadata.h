#ifndef ANALYSIS_PREPROCESS_METADATA_H
#define ANALYSIS_PREPROCESS_METADATA_H

/* Placeholder-only preprocessing metadata.  Zero noise floor is permitted by
 * the numerical contract when no session calibration exists.  Identity
 * normalization and 0.5 thresholds are not trained values, so every result
 * carrying this metadata must retain NO_ACCURACY_CLAIM. */
#define ANALYSIS_PREPROCESS_METADATA_VERSION (1U)
#define ANALYSIS_PREPROCESS_CALIBRATED       (0U)
#define ANALYSIS_NOISE_FLOOR_IS_ZERO         (1U)
#define ANALYSIS_MASK_THRESHOLD              (0.5F)

static const float g_analysis_feature_mean[3] = {0.0F, 0.0F, 0.0F};
static const float g_analysis_feature_std[3] = {1.0F, 1.0F, 1.0F};
static const float g_analysis_class_threshold[4] = {0.5F, 0.5F, 0.5F, 0.5F};

#endif
