#include "rf_demo_data.h"

const rf_demo_config_t rf_demo_config = {
    .sample_rate_sps = 60000000U,
    .rf_bandwidth_mhz_x10 = 560U,
    .capture_duration_ms_x10 = 98U,
    .fft_points = 1024U,
    .hop_points = 512U,
};

const rf_demo_channel_t rf_demo_channels[RF_DEMO_CHANNEL_COUNT] = {
    { "CH1", "2.4G-A", "IQSC", 2420U, 0U, -96, -96, 0U, RF_DEMO_SOURCE_CAPTURE, 0U, 0U },
    { "CH2", "2.4G-B", "IQSC", 2464U, 0U, -96, -96, 0U, RF_DEMO_SOURCE_CAPTURE, 0U, 0U },
    { "CH3", "5.8G-A", "IQSC", 5760U, 0U, -96, -96, 0U, RF_DEMO_SOURCE_CAPTURE, 0U, 0U },
    { "CH4", "5.8G-B", "IQSC", 5816U, 0U, -96, -96, 0U, RF_DEMO_SOURCE_CAPTURE, 0U, 0U },
};

const rf_demo_class_t rf_demo_classes[RF_DEMO_CLASS_COUNT] = {
    { "C0", "DJI MINI 3 PRO", "MINI3", 0x35D8D0U, RF_DEMO_STATE_INACTIVE, 0U, 0U },
    { "C1", "XIAOBAWANG", "XIAOBW", 0xF4B84AU, RF_DEMO_STATE_INACTIVE, 0U, 1U },
    { "C2", "AT9S", "AT9S", 0x91D45BU, RF_DEMO_STATE_INACTIVE, 0U, 2U },
    { "C3", "YUNZHUO T12", "T12", 0xE17AC6U, RF_DEMO_STATE_INACTIVE, 0U, 3U },
};

const rf_demo_mask_t rf_demo_masks[RF_DEMO_MASK_COUNT] = {0};
const uint8_t rf_demo_spectrum[RF_DEMO_CHANNEL_COUNT][RF_DEMO_SPECTRUM_BINS] = {0};
const uint8_t rf_demo_waterfall[RF_DEMO_CHANNEL_COUNT]
                               [RF_DEMO_WATERFALL_ROWS]
                               [RF_DEMO_WATERFALL_COLS] = {0};
