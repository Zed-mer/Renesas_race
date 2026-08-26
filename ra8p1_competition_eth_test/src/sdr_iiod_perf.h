#ifndef SDR_IIOD_PERF_H
#define SDR_IIOD_PERF_H

#include <stdint.h>

#define SDR_IIOD_PERF_MAGIC 0x46524453UL

typedef struct st_sdr_iiod_perf_result
{
    uint32_t magic;
    uint32_t schema_version;
    uint32_t state;
    int32_t  last_error;
    int32_t  ip_result;
    int32_t  netmask_result;
    int32_t  gateway_result;
    int32_t  phy_result;
    uint32_t phy_speed;
    uint32_t local_pause;
    uint32_t partner_pause;
    int32_t  ping_result;
    uint32_t ping_time_ms;
    int32_t  connect_result;
    int32_t  timeout_result;
    int32_t  open_result;
    uint32_t version_ok;
    char     version[32];
    uint32_t request_bytes;
    uint32_t read_requests;
    uint32_t read_chunks;
    uint32_t stream_mask;
    uint32_t elapsed_ms;
    uint32_t payload_mbps_x1000;
    uint32_t checksum;
    uint32_t errors;
    uint64_t bytes_received;
} sdr_iiod_perf_result_t;

extern volatile sdr_iiod_perf_result_t g_sdr_iiod_perf_result;
extern volatile char g_sdr_iiod_perf_report[256];

void sdr_iiod_perf_start(void);

#endif
