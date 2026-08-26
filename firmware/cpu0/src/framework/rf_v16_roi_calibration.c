#include "rf_v16_roi_calibration.h"

/* Generated from exact board-input fixed-point calibration. */
const rf_v16_class_config_t
    g_rf_v16_class_configs[RF_V16_CLASS_COUNT] = {
    {{{256, 141, 141, 13, 256, 0}, 26, 0, 9830u, 2u, 2u, 2u, 2u},
     {{51, 384, -26, -128, 128, -256}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(1096), RF_V16_GATE_LINEAR, {0u, 0u, 0u}},
     {{51, 384, -26, -128, 128, -256}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(1616), RF_V16_GATE_LINEAR, {0u, 0u, 0u}}},
    {{{256, 141, 141, 13, 256, 0}, 36, 0, 9830u, 3u, 3u, 2u, 2u},
     {{51, 384, -26, -128, 128, -256}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(-436), RF_V16_GATE_LINEAR, {0u, 0u, 0u}},
     {{0, 0, 0, 0, 0, 0}, {-154, 461, 1050, 0, 0u, 0u, RF_V16_RULE_MIN_FREQUENCY_EDGE | RF_V16_RULE_MAX_TEXTURE | RF_V16_RULE_MAX_BURSTINESS, 0u}, INT32_C(0), RF_V16_GATE_COMPOSITE, {0u, 0u, 0u}}},
    {{{256, 0, 0, 0, 0, 0}, 13, 0, 9830u, 1u, 1u, 2u, 2u},
     {{51, 384, -26, -128, 128, -256}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(-339), RF_V16_GATE_LINEAR, {0u, 0u, 0u}},
     {{51, 384, -26, -128, 128, -256}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(-409), RF_V16_GATE_LINEAR, {0u, 0u, 0u}}},
    {{{205, 256, 256, 0, 192, 0}, 36, 0, 9830u, 3u, 3u, 2u, 2u},
     {{205, 205, 205, 0, 192, 0}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(2689), RF_V16_GATE_LINEAR, {0u, 0u, 0u}},
     {{230, 141, 141, 26, 192, 38}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(2994), RF_V16_GATE_LINEAR, {0u, 0u, 0u}}},
    {{{256, 0, 0, 0, 0, 0}, 20, 0, 9830u, 2u, 2u, 2u, 2u},
     {{205, 205, 205, 0, 192, 0}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(2950), RF_V16_GATE_LINEAR, {0u, 0u, 0u}},
     {{256, 0, 0, 0, 256, 0}, {0, 0, 0, 0, 0u, 0u, 0u, 0u}, INT32_C(3957), RF_V16_GATE_LINEAR, {0u, 0u, 0u}}}
};
