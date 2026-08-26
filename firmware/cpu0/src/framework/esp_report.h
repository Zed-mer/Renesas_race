#ifndef CPU0_ESP_REPORT_H
#define CPU0_ESP_REPORT_H

#include <stdint.h>

#define ESP_REPORT_DIAG_MAGIC   (0x52505345UL) /* ESPR */
#define ESP_REPORT_DIAG_VERSION (5U)

#define ESP_REPORT_CWJAP_CODE_NONE               (0U)
#define ESP_REPORT_CWJAP_CODE_CONNECTION_TIMEOUT (1U)
#define ESP_REPORT_CWJAP_CODE_WRONG_PASSWORD     (2U)
#define ESP_REPORT_CWJAP_CODE_AP_NOT_FOUND       (3U)
#define ESP_REPORT_CWJAP_CODE_CONNECTION_FAILED  (4U)

typedef struct st_esp_report_diag
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t configured;
    uint32_t thread_started;
    uint32_t collector_started;
    uint32_t uart_ready;
    uint32_t last_generation;
    uint32_t last_working_mask;
    uint32_t publish_attempts;
    uint32_t publish_successes;
    uint32_t publish_failures;
    uint32_t normal_reports;
    uint32_t event_reports;
    uint32_t last_event;
    uint32_t last_publish_tick;
    uint32_t last_step;
    uint32_t last_error;
    uint32_t tcp_connects;
    uint32_t mqtt_connects;
    uint32_t mqtt_publish_count;
    uint32_t uart_rx_bytes;
    uint32_t uart_tx_bytes;
    uint32_t service_ready;
    uint32_t ap_ready;
    uint32_t sta_connected;
    uint32_t web_server_ready;
    uint32_t web_requests;
    uint32_t web_failures;
    uint32_t web_rx_drops;
    uint32_t esp_resets;
    uint32_t activity_edges;
    uint32_t event_queue_depth;
    uint32_t event_queue_high_water;
    uint32_t event_queue_drops;
    uint32_t sta_connect_attempts;
    uint32_t sta_join_failures;
    uint32_t sta_last_cwjap_code;
    uint32_t sta_last_failure;
} esp_report_diag_t;

extern volatile esp_report_diag_t g_esp_report_diag;

void esp_report_start(void);

#endif
