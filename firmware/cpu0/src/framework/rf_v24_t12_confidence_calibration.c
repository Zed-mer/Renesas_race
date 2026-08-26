#include "rf_v24_t12_confidence_calibration.h"

#include <stddef.h>
#include <stdint.h>

typedef struct rf_v24_t12_knot {
    uint16_t raw_q15;
    uint16_t calibrated_q15;
} rf_v24_t12_knot_t;

static const rf_v24_t12_knot_t g_rf_v24_t12_knots[30] = {
    {UINT16_C(0), UINT16_C(21)},
    {UINT16_C(1382), UINT16_C(21)},
    {UINT16_C(2145), UINT16_C(21)},
    {UINT16_C(2824), UINT16_C(45)},
    {UINT16_C(3238), UINT16_C(74)},
    {UINT16_C(4015), UINT16_C(74)},
    {UINT16_C(5117), UINT16_C(91)},
    {UINT16_C(5763), UINT16_C(275)},
    {UINT16_C(6042), UINT16_C(275)},
    {UINT16_C(6470), UINT16_C(275)},
    {UINT16_C(7002), UINT16_C(336)},
    {UINT16_C(7664), UINT16_C(419)},
    {UINT16_C(8225), UINT16_C(771)},
    {UINT16_C(8761), UINT16_C(771)},
    {UINT16_C(9376), UINT16_C(883)},
    {UINT16_C(9805), UINT16_C(2048)},
    {UINT16_C(10200), UINT16_C(2074)},
    {UINT16_C(10795), UINT16_C(2192)},
    {UINT16_C(11775), UINT16_C(3277)},
    {UINT16_C(12829), UINT16_C(5461)},
    {UINT16_C(13825), UINT16_C(7243)},
    {UINT16_C(14572), UINT16_C(7909)},
    {UINT16_C(15341), UINT16_C(12983)},
    {UINT16_C(16383), UINT16_C(13534)},
    {UINT16_C(17414), UINT16_C(20316)},
    {UINT16_C(18252), UINT16_C(22794)},
    {UINT16_C(20211), UINT16_C(29994)},
    {UINT16_C(22074), UINT16_C(30719)},
    {UINT16_C(26166), UINT16_C(32727)},
    {UINT16_C(32767), UINT16_C(32727)}
};

uint16_t rf_v24_t12_calibrate_confidence_q15(uint16_t raw_q15)
{
    size_t index;
    if (raw_q15 <= g_rf_v24_t12_knots[0].raw_q15) {
        return g_rf_v24_t12_knots[0].calibrated_q15;
    }
    for (index = 1u; index < 30u; ++index) {
        if (raw_q15 <= g_rf_v24_t12_knots[index].raw_q15) {
            uint32_t x0 = g_rf_v24_t12_knots[index - 1u].raw_q15;
            uint32_t x1 = g_rf_v24_t12_knots[index].raw_q15;
            int32_t y0 = g_rf_v24_t12_knots[index - 1u].calibrated_q15;
            int32_t y1 = g_rf_v24_t12_knots[index].calibrated_q15;
            uint32_t width = x1 - x0;
            if (width == 0u) {
                return (uint16_t)y1;
            }
            return (uint16_t)(
                y0 +
                (int32_t)(((int64_t)(raw_q15 - x0) * (y1 - y0) +
                           (int64_t)width / 2) /
                          width));
        }
    }
    return g_rf_v24_t12_knots[29].calibrated_q15;
}
