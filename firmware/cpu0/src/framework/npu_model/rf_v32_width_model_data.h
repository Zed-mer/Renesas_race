#ifndef RF_V32_WIDTH_MODEL_DATA_H
#define RF_V32_WIDTH_MODEL_DATA_H

#include <stdint.h>

#include "rf_v31_model_data.h"

#define RF_V32_WIDTH_COMMAND_BYTES 20092u
#define RF_V32_WIDTH_WEIGHT_BYTES 10144u
#define RF_V32_WIDTH_SCRATCH_BYTES 29952u
#define RF_V32_WIDTH_INPUT_BYTES 11520u
#define RF_V32_WIDTH_OUTPUT_BYTES 1u
#define RF_V32_WIDTH_INPUT_OFFSET 9216u
#define RF_V32_WIDTH_OUTPUT_OFFSET 32u

extern const rf_v31_model_blob_t g_rf_v32_width_model;

#endif
