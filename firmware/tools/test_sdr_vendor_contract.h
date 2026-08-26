#ifndef TEST_SDR_VENDOR_CONTRACT_H
#define TEST_SDR_VENDOR_CONTRACT_H

/* Compile-only fixture matching the API contract in sdr_api.md. */

#include <stdint.h>

typedef enum e_sdr_status
{
    SDR_OK = 0,
    SDR_EINVAL = -1,
    SDR_EIO = -2,
    SDR_ETIMEOUT = -3,
    SDR_ENOMEM = -4,
    SDR_ESTATE = -5
} sdr_status_t;

typedef enum e_sdr_gain_mode
{
    SDR_GAIN_MGC = 0,
    SDR_GAIN_FAST_ATTACK = 1,
    SDR_GAIN_SLOW_ATTACK = 2,
    SDR_GAIN_HYBRID = 3
} sdr_gain_mode_t;

typedef struct st_sdr_config
{
    uint32_t ref_clk_hz;
    uint64_t rx_lo_hz;
    uint64_t tx_lo_hz;
    uint32_t sample_rate_hz;
    uint32_t rx_bw_hz;
    uint32_t tx_bw_hz;
    uint8_t rx_channels;
    uint8_t tx_channels;
    sdr_gain_mode_t rx_gain_mode;
} sdr_config_t;

typedef struct st_sdr_handle
{
    void *phy;
    void *rx_adc;
    void *tx_dac;
    void *rx_dmac;
    void *tx_dmac;
    uint8_t initialized;
    uint8_t rx_channels;
    uint8_t tx_channels;
} sdr_handle_t;

typedef struct st_sdr_iq2_sample
{
    int16_t rx1_i;
    int16_t rx1_q;
    int16_t rx2_i;
    int16_t rx2_q;
} sdr_iq2_sample_t;

void sdr_default_config(sdr_config_t *config);
sdr_status_t sdr_init(sdr_handle_t *device, const sdr_config_t *config);
sdr_status_t sdr_deinit(sdr_handle_t *device);
sdr_status_t sdr_set_rx(sdr_handle_t *device, uint64_t lo_hz,
                        uint32_t sample_rate_hz, uint32_t bandwidth_hz);
sdr_status_t sdr_rx_capture(sdr_handle_t *device, sdr_iq2_sample_t *buffer,
                            uint32_t sample_count, uint32_t timeout_ms);

#endif /* TEST_SDR_VENDOR_CONTRACT_H */
