#ifndef ETH_PERF_H
#define ETH_PERF_H

#define ETH_PERF_SOCKET_PORT    5001U
#define ETH_PERF_RAW_RX_PORT    5002U
#define ETH_PERF_PACKET_MAGIC   0x46505445UL
#define ETH_PERF_REPORT_REPEATS 4U

void eth_perf_start(void);
void eth_perf_raw_service_start(void);

#endif
