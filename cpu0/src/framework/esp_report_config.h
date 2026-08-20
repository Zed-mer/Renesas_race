#ifndef CPU0_ESP_REPORT_CONFIG_H
#define CPU0_ESP_REPORT_CONFIG_H

#define ESP_REPORT_ENABLE              (1)
#define ESP_REPORT_UART_DEVICE         "uart1"

/* The read-only monitor is served at the DHCP address assigned to the ESP
 * after it joins the external STA network below. */
#define ESP_REPORT_HTTP_PORT           (80U)
#define ESP_REPORT_HTTP_MAX_CLIENTS    (3U)
#define ESP_REPORT_MQTT_LINK_ID        (4U)

/* Use the verified 2.4 GHz WPA2 hotspot only. Keep the secondary slot empty
 * so a reconnect cannot spend time trying an unavailable network. */
#define ESP_REPORT_WIFI_PRIMARY_SSID       "REDMIha"
#define ESP_REPORT_WIFI_PRIMARY_PASSWORD   "lzhdasb1"
#define ESP_REPORT_WIFI_SECONDARY_SSID     ""
#define ESP_REPORT_WIFI_SECONDARY_PASSWORD ""
#define ESP_REPORT_MQTT_HOST           "192.168.137.1"
#define ESP_REPORT_MQTT_PORT           (1883U)
#define ESP_REPORT_MQTT_CLIENT_ID       "ra8p1-drone-detector"
#define ESP_REPORT_MQTT_USERNAME        ""
#define ESP_REPORT_MQTT_PASSWORD        ""
#define ESP_REPORT_MQTT_TOPIC           "ra8p1/drone/status"
#define ESP_REPORT_DEVICE_ID            "ra8p1-drone-detector"

#define ESP_REPORT_NORMAL_INTERVAL_MS   (60000U)
#define ESP_REPORT_EVENT_RETRY_MS       (30000U)
#define ESP_REPORT_SERVICE_RETRY_MS     (30000U)
#define ESP_REPORT_STA_RETRY_MS         (30000U)
/* CPU1 publishes one compact activity word. Keep the CPU0 UART transport
 * below the SDR/IQ pipeline and wake it infrequently between AT transactions. */
#define ESP_REPORT_POLL_INTERVAL_MS     (50U)
#define ESP_REPORT_BOOT_DELAY_MS        (1500U)
#define ESP_REPORT_AT_TIMEOUT_MS        (3000U)
#define ESP_REPORT_WIFI_TIMEOUT_MS      (20000U)
#define ESP_REPORT_TCP_TIMEOUT_MS       (10000U)
#define ESP_REPORT_MQTT_TIMEOUT_MS      (5000U)
#define ESP_REPORT_HTTP_TIMEOUT_MS      (3000U)

#endif
