#ifndef RF_V12_PREPROCESS_H
#define RF_V12_PREPROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "rf_v12_sparse_contract.h"

#define RF_V12_PREPROCESS_FEATURE_CELLS \
    (RF_V12_FEATURE_FREQUENCY_BINS * RF_V12_FEATURE_TIME_BINS)
#define RF_V12_PREPROCESS_BACKGROUND_WINDOWS (5U)

typedef enum e_rf_v12_preprocess_result
{
    RF_V12_PREPROCESS_INVALID = 0,
    RF_V12_PREPROCESS_BACKGROUND_NOT_READY = 1,
    RF_V12_PREPROCESS_READY = 2
} rf_v12_preprocess_result_t;

/* One instance belongs to one concurrently active STFT tile. The feature
 * staging buffer is supplied by the caller so it can remain in CPU0 SDRAM and
 * stay immutable across the sequential V2/V3 invokes. */
typedef struct st_rf_v12_preprocess_tile
{
    uint64_t frame_weighted_power[RF_V12_FEATURE_FREQUENCY_BINS];
    uint64_t pool_weighted_power_sum[RF_V12_FEATURE_FREQUENCY_BINS];
    uint64_t pool_weighted_power_max[RF_V12_FEATURE_FREQUENCY_BINS];
    float c0_db[RF_V12_PREPROCESS_FEATURE_CELLS];
    int8_t *feature_staging;
    uint32_t raw_frame_index;
    uint16_t time_bin;
    uint8_t pool_frame_count;
    uint8_t frame_open;
} rf_v12_preprocess_tile_t;

/* One instance belongs to one capture center. calibration[0] is overwritten
 * with the frozen cell-wise Q50 after the fifth complete valid tile. */
typedef struct st_rf_v12_preprocess_background
{
    float calibration[RF_V12_PREPROCESS_BACKGROUND_WINDOWS]
                     [RF_V12_PREPROCESS_FEATURE_CELLS];
    int16_t gain_db_q8;
    uint16_t generation;
    uint8_t gain_valid;
    uint8_t calibration_count;
    uint8_t ready;
    uint8_t reset_pending;
} rf_v12_preprocess_background_t;

typedef struct st_rf_v12_preprocess_finalize_info
{
    rf_v12_preprocess_result_t result;
    uint16_t background_generation;
    uint8_t background_became_ready;
    uint8_t background_reset;
} rf_v12_preprocess_finalize_info_t;

void rf_v12_preprocess_tile_reset(rf_v12_preprocess_tile_t *tile,
                                  int8_t *feature_staging);

/* raw_frame_index is 0..1151. shifted_frequency_bin is ordered from -Fs/2 to
 * +Fs/2, matching an FFT-shifted complex spectrum. Power must be linear. */
bool rf_v12_preprocess_frame_begin(rf_v12_preprocess_tile_t *tile,
                                   uint32_t raw_frame_index);
void rf_v12_preprocess_power_bin(rf_v12_preprocess_tile_t *tile,
                                 uint32_t shifted_frequency_bin,
                                 uint32_t linear_power);
bool rf_v12_preprocess_frame_end(rf_v12_preprocess_tile_t *tile,
                                 float linear_power_scale);
bool rf_v12_preprocess_frame(
    rf_v12_preprocess_tile_t *tile,
    const uint32_t fftshift_power[RF_V12_FFT_POINTS],
    uint32_t raw_frame_index,
    float linear_power_scale);

void rf_v12_preprocess_background_init(
    rf_v12_preprocess_background_t *background);
bool rf_v12_preprocess_background_set_gain(
    rf_v12_preprocess_background_t *background,
    int16_t gain_db_q8);

/* capture_valid=false never trains the background and never emits model
 * input. A fifth calibration tile freezes Q50 but remains BACKGROUND_NOT_READY;
 * inference begins with the following complete valid tile. */
rf_v12_preprocess_finalize_info_t rf_v12_preprocess_finalize(
    rf_v12_preprocess_tile_t *tile,
    rf_v12_preprocess_background_t *background,
    bool capture_valid);

/* Synthetic compute proof only: estimate one per-frequency Q50 from this
 * tile. Production IQ must use rf_v12_preprocess_finalize() instead. */
bool rf_v12_preprocess_finalize_synthetic(
    rf_v12_preprocess_tile_t *tile);

bool rf_v12_preprocess_tile_complete(const rf_v12_preprocess_tile_t *tile);
const float *rf_v12_preprocess_background_relative_c0(
    const rf_v12_preprocess_tile_t *tile);

/* Exposed for byte-parity tests and for guarded scalar fallbacks. */
int32_t rf_v12_preprocess_round_to_nearest_even(float value);
int8_t rf_v12_preprocess_quantize(float value, uint32_t channel);

#endif
