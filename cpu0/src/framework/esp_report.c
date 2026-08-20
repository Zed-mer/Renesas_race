#include "esp_report.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <rtdevice.h>
#include <rtthread.h>

#include "esp_report_config.h"
#include "esp_report_web.h"
#include "hal_data.h"
#include "ipc_bridge.h"

#define ESP_REPORT_THREAD_PRIORITY (26U)
#define ESP_REPORT_THREAD_TICK     (10U)
#define ESP_REPORT_THREAD_STACK    (5120U)
#define ESP_REPORT_COLLECTOR_PRIORITY (25U)
#define ESP_REPORT_COLLECTOR_TICK     (10U)
#define ESP_REPORT_COLLECTOR_STACK    (1024U)
#define ESP_REPORT_EVENT_QUEUE_DEPTH  (32U)
#define ESP_REPORT_EVENT_NONE      (0U)
#define ESP_REPORT_EVENT_START     (1U)
#define ESP_REPORT_EVENT_CLEAR     (2U)
#define ESP_REPORT_EVENT_UPDATE    (3U)
#define ESP_REPORT_EVENT_NORMAL    (4U)

#define ESP_REPORT_STEP_IDLE       (0U)
#define ESP_REPORT_STEP_BOOT       (1U)
#define ESP_REPORT_STEP_AT         (2U)
#define ESP_REPORT_STEP_WIFI       (3U)
#define ESP_REPORT_STEP_TCP        (4U)
#define ESP_REPORT_STEP_MQTT       (5U)
#define ESP_REPORT_STEP_PUBLISH    (6U)
#define ESP_REPORT_STEP_CLOSE      (7U)

#define ESP_REPORT_ERROR_NONE      (0U)
#define ESP_REPORT_ERROR_UART      (1U)
#define ESP_REPORT_ERROR_AT        (2U)
#define ESP_REPORT_ERROR_TIMEOUT   (3U)
#define ESP_REPORT_ERROR_MQTT      (4U)

#define ESP_REPORT_HTTP_QUEUE_DEPTH  (3U)
#define ESP_REPORT_HTTP_REQUEST_MAX  (256U)
#define ESP_REPORT_IPD_HEADER_MAX     (24U)
#define ESP_REPORT_STATUS_LINE_MAX    (96U)
#define ESP_REPORT_HTTP_HEADER_MAX    (192U)

volatile esp_report_diag_t g_esp_report_diag;

static struct rt_thread g_esp_report_thread;
static rt_uint8_t g_esp_report_stack[ESP_REPORT_THREAD_STACK];
static struct rt_thread g_esp_collector_thread;
static rt_uint8_t g_esp_collector_stack[ESP_REPORT_COLLECTOR_STACK];
static rt_device_t g_esp_uart;
static bool g_esp_report_started;

typedef struct st_esp_report_event_record
{
    uint32_t event;
    uint32_t generation;
    uint32_t working_mask;
} esp_report_event_record_t;

typedef struct st_esp_http_request
{
    bool occupied;
    uint8_t link_id;
    uint16_t length;
    char data[ESP_REPORT_HTTP_REQUEST_MAX + 1U];
} esp_http_request_t;

typedef struct st_esp_ipd_parser
{
    uint8_t prefix_match;
    bool reading_header;
    char header[ESP_REPORT_IPD_HEADER_MAX];
    size_t header_used;
    uint8_t link_id;
    size_t payload_remaining;
    size_t payload_used;
    bool capture_payload;
    char payload[ESP_REPORT_HTTP_REQUEST_MAX + 1U];
} esp_ipd_parser_t;

static esp_http_request_t g_http_queue[ESP_REPORT_HTTP_QUEUE_DEPTH];
static uint8_t g_http_queue_read;
static uint8_t g_http_queue_write;
static uint8_t g_http_queue_count;
static esp_ipd_parser_t g_ipd_parser;
static bool g_esp_service_ready;
static bool g_esp_reset_detected;
static bool g_mqtt_link_active;
static bool g_mqtt_available;
static bool g_sta_retry_requested;
static char g_status_line[ESP_REPORT_STATUS_LINE_MAX];
static size_t g_status_line_used;
static esp_report_event_record_t
    g_event_queue[ESP_REPORT_EVENT_QUEUE_DEPTH];
static uint8_t g_event_queue_read;
static uint8_t g_event_queue_write;
static uint8_t g_event_queue_count;
static bool g_activity_state_ready;
static uint32_t g_activity_generation;
static uint32_t g_activity_working_mask;
static ra8p1_wifi_connection_state_t g_sta_connection_state;
static char g_sta_attempt_ssid[RA8P1_WIFI_SSID_MAX_BYTES + 1U];
static char g_sta_connected_ssid[RA8P1_WIFI_SSID_MAX_BYTES + 1U];

typedef struct st_esp_report_sta_network
{
    const char *ssid;
    const char *password;
} esp_report_sta_network_t;

static const esp_report_sta_network_t g_sta_networks[] =
{
    {ESP_REPORT_WIFI_PRIMARY_SSID, ESP_REPORT_WIFI_PRIMARY_PASSWORD},
    {ESP_REPORT_WIFI_SECONDARY_SSID, ESP_REPORT_WIFI_SECONDARY_PASSWORD}
};

static const char *esp_report_event_name(uint32_t event);
static bool esp_report_sta_connect(void);
static bool esp_report_sta_configured(void);

static size_t esp_report_bounded_length(const char *text, size_t maximum)
{
    size_t length = 0U;
    if (text == NULL)
    {
        return 0U;
    }
    while ((length < maximum) && (text[length] != '\0'))
    {
        ++length;
    }
    return length;
}

static void esp_report_ssid_copy(char *destination, const char *source)
{
    const size_t length = esp_report_bounded_length(
        source, RA8P1_WIFI_SSID_MAX_BYTES);
    memset(destination, 0, RA8P1_WIFI_SSID_MAX_BYTES + 1U);
    if (length != 0U)
    {
        memcpy(destination, source, length);
    }
}

static void esp_report_sta_status_set(
    ra8p1_wifi_connection_state_t connection_state,
    const char *ssid)
{
    char normalized_ssid[RA8P1_WIFI_SSID_MAX_BYTES + 1U];
    esp_report_ssid_copy(normalized_ssid, ssid);
    if ((g_sta_connection_state == connection_state) &&
        (strcmp(g_sta_connected_ssid, normalized_ssid) == 0))
    {
        return;
    }
    g_sta_connection_state = connection_state;
    esp_report_ssid_copy(g_sta_connected_ssid, normalized_ssid);
    ipc_bridge_cpu0_wifi_status_publish(connection_state,
                                         g_sta_connected_ssid);
}

static void esp_report_event_queue_push(uint32_t event,
                                        uint32_t generation,
                                        uint32_t working_mask)
{
    const rt_base_t level = rt_hw_interrupt_disable();

    if (g_event_queue_count >= ESP_REPORT_EVENT_QUEUE_DEPTH)
    {
        g_event_queue_read = (uint8_t)((g_event_queue_read + 1U) %
                                       ESP_REPORT_EVENT_QUEUE_DEPTH);
        g_event_queue_count--;
        g_esp_report_diag.event_queue_drops++;
    }
    g_event_queue[g_event_queue_write].event = event;
    g_event_queue[g_event_queue_write].generation = generation;
    g_event_queue[g_event_queue_write].working_mask = working_mask;
    g_event_queue_write = (uint8_t)((g_event_queue_write + 1U) %
                                    ESP_REPORT_EVENT_QUEUE_DEPTH);
    g_event_queue_count++;
    g_esp_report_diag.event_queue_depth = g_event_queue_count;
    if (g_event_queue_count > g_esp_report_diag.event_queue_high_water)
    {
        g_esp_report_diag.event_queue_high_water = g_event_queue_count;
    }
    rt_hw_interrupt_enable(level);
}

static bool esp_report_event_queue_pop(esp_report_event_record_t *record)
{
    bool available = false;
    const rt_base_t level = rt_hw_interrupt_disable();

    if ((record != NULL) && (g_event_queue_count != 0U))
    {
        *record = g_event_queue[g_event_queue_read];
        g_event_queue_read = (uint8_t)((g_event_queue_read + 1U) %
                                       ESP_REPORT_EVENT_QUEUE_DEPTH);
        g_event_queue_count--;
        g_esp_report_diag.event_queue_depth = g_event_queue_count;
        available = true;
    }
    rt_hw_interrupt_enable(level);
    return available;
}

static bool esp_report_activity_snapshot(uint32_t *generation,
                                         uint32_t *working_mask)
{
    bool ready;
    const rt_base_t level = rt_hw_interrupt_disable();

    ready = g_activity_state_ready;
    if (ready)
    {
        *generation = g_activity_generation;
        *working_mask = g_activity_working_mask;
    }
    rt_hw_interrupt_enable(level);
    return ready;
}

static void esp_report_status_line_complete(void)
{
    g_status_line[g_status_line_used] = '\0';
    if (strstr(g_status_line, "WIFI GOT IP") != NULL)
    {
        g_esp_report_diag.sta_connected = 1U;
        if (g_sta_attempt_ssid[0] != '\0')
        {
            esp_report_sta_status_set(RA8P1_WIFI_CONNECTED,
                                      g_sta_attempt_ssid);
        }
    }
    else if (strstr(g_status_line, "WIFI DISCONNECT") != NULL)
    {
        g_esp_report_diag.sta_connected = 0U;
        g_mqtt_available = false;
        g_sta_retry_requested = true;
        esp_report_sta_status_set(RA8P1_WIFI_DISCONNECTED, "");
    }

    if (strstr(g_status_line, "4,CLOSED") != NULL)
    {
        g_mqtt_link_active = false;
    }
    if (g_esp_service_ready && (strcmp(g_status_line, "ready") == 0))
    {
        g_esp_service_ready = false;
        g_esp_reset_detected = true;
        g_mqtt_link_active = false;
        g_mqtt_available = false;
        g_esp_report_diag.service_ready = 0U;
        g_esp_report_diag.ap_ready = 0U;
        g_esp_report_diag.sta_connected = 0U;
        g_esp_report_diag.web_server_ready = 0U;
        g_esp_report_diag.esp_resets++;
        g_sta_attempt_ssid[0] = '\0';
        esp_report_sta_status_set(RA8P1_WIFI_DISCONNECTED, "");
    }
    g_status_line_used = 0U;
}

static void esp_report_status_capture_byte(uint8_t byte)
{
    if (byte == (uint8_t)'\r')
    {
        return;
    }
    if (byte == (uint8_t)'\n')
    {
        if (g_status_line_used > 0U)
        {
            esp_report_status_line_complete();
        }
        return;
    }
    if ((byte < 0x20U) || (byte > 0x7EU))
    {
        g_status_line_used = 0U;
        return;
    }
    if (g_status_line_used < (sizeof(g_status_line) - 1U))
    {
        g_status_line[g_status_line_used++] = (char)byte;
    }
    else
    {
        g_status_line_used = 0U;
    }
}

static bool esp_report_http_header_parse(const char *header,
                                         size_t length,
                                         uint8_t *link_id,
                                         size_t *payload_length)
{
    size_t cursor = 0U;
    uint32_t link = 0U;
    size_t payload = 0U;
    size_t digits = 0U;

    while ((cursor < length) && (header[cursor] >= '0') &&
           (header[cursor] <= '9'))
    {
        link = link * 10U + (uint32_t)(header[cursor] - '0');
        ++cursor;
        ++digits;
    }
    if ((digits == 0U) || (link > 4U) || (cursor >= length) ||
        (header[cursor] != ','))
    {
        return false;
    }
    ++cursor;
    digits = 0U;
    while ((cursor < length) && (header[cursor] >= '0') &&
           (header[cursor] <= '9'))
    {
        payload = payload * 10U + (size_t)(header[cursor] - '0');
        ++cursor;
        ++digits;
    }
    if ((digits == 0U) || (cursor != length) || (payload > 65535U))
    {
        return false;
    }
    *link_id = (uint8_t)link;
    *payload_length = payload;
    return true;
}

static void esp_report_http_enqueue(uint8_t link_id,
                                    const char *payload,
                                    size_t length)
{
    if ((length == 0U) || (length > ESP_REPORT_HTTP_REQUEST_MAX) ||
        (g_http_queue_count >= ESP_REPORT_HTTP_QUEUE_DEPTH))
    {
        g_esp_report_diag.web_rx_drops++;
        return;
    }
    esp_http_request_t *request = &g_http_queue[g_http_queue_write];
    request->occupied = true;
    request->link_id = link_id;
    request->length = (uint16_t)length;
    memcpy(request->data, payload, length);
    request->data[length] = '\0';
    g_http_queue_write = (uint8_t)((g_http_queue_write + 1U) %
                                   ESP_REPORT_HTTP_QUEUE_DEPTH);
    g_http_queue_count++;
}

static void esp_report_http_capture_bytes(const uint8_t *data, size_t length)
{
    static const char prefix[] = "+IPD,";
    esp_ipd_parser_t *parser = &g_ipd_parser;

    for (size_t index = 0U; index < length; ++index)
    {
        const uint8_t byte = data[index];
        if (parser->payload_remaining > 0U)
        {
            if (parser->capture_payload &&
                (parser->payload_used < ESP_REPORT_HTTP_REQUEST_MAX))
            {
                parser->payload[parser->payload_used++] = (char)byte;
            }
            parser->payload_remaining--;
            if (parser->payload_remaining == 0U)
            {
                if (parser->capture_payload)
                {
                    parser->payload[parser->payload_used] = '\0';
                    esp_report_http_enqueue(parser->link_id,
                                            parser->payload,
                                            parser->payload_used);
                }
                parser->payload_used = 0U;
                parser->capture_payload = false;
            }
            continue;
        }

        esp_report_status_capture_byte(byte);

        if (parser->reading_header)
        {
            if (byte == ':')
            {
                size_t payload_length = 0U;
                uint8_t link_id = 0U;
                if (esp_report_http_header_parse(parser->header,
                                                 parser->header_used,
                                                 &link_id,
                                                 &payload_length))
                {
                    g_status_line_used = 0U;
                    parser->link_id = link_id;
                    parser->payload_remaining = payload_length;
                    parser->payload_used = 0U;
                    parser->capture_payload =
                        (link_id != ESP_REPORT_MQTT_LINK_ID);
                }
                parser->reading_header = false;
                parser->header_used = 0U;
                parser->prefix_match = 0U;
            }
            else if (parser->header_used < (sizeof(parser->header) - 1U))
            {
                parser->header[parser->header_used++] = (char)byte;
            }
            else
            {
                parser->reading_header = false;
                parser->header_used = 0U;
                parser->prefix_match = 0U;
                g_esp_report_diag.web_rx_drops++;
            }
            continue;
        }

        if (byte == (uint8_t)prefix[parser->prefix_match])
        {
            parser->prefix_match++;
            if (parser->prefix_match == (sizeof(prefix) - 1U))
            {
                parser->reading_header = true;
                parser->header_used = 0U;
                parser->prefix_match = 0U;
            }
        }
        else
        {
            parser->prefix_match = (byte == (uint8_t)prefix[0]) ? 1U : 0U;
        }
    }
}

static rt_ssize_t esp_report_uart_read(uint8_t *buffer, size_t capacity)
{
    const rt_ssize_t received = rt_device_read(g_esp_uart, 0, buffer,
                                                capacity);
    if (received > 0)
    {
        g_esp_report_diag.uart_rx_bytes += (uint32_t)received;
        esp_report_http_capture_bytes(buffer, (size_t)received);
    }
    return received;
}

static bool esp_report_tick_due(rt_tick_t now, rt_tick_t deadline)
{
    return (rt_int32_t)(now - deadline) >= 0;
}

static void esp_report_power(bool enabled)
{
    (void) R_IOPORT_PinWrite(g_ioport.p_ctrl, ESP_WIFI_EN,
                             enabled ? BSP_IO_LEVEL_HIGH :
                                       BSP_IO_LEVEL_LOW);
}

static void esp_report_uart_drain(void)
{
    uint8_t bytes[64];
    rt_ssize_t received;
    do
    {
        received = esp_report_uart_read(bytes, sizeof(bytes));
        if (received > 0)
        {
            /* esp_report_uart_read accounts for diagnostics and HTTP input. */
        }
    } while (received > 0);
}

static bool esp_report_uart_write_bytes(const uint8_t *data, size_t length)
{
    if (length == 0U)
    {
        return true;
    }
    if (rt_device_write(g_esp_uart, 0, data, length) != (rt_ssize_t)length)
    {
        g_esp_report_diag.last_error = ESP_REPORT_ERROR_UART;
        return false;
    }
    g_esp_report_diag.uart_tx_bytes += (uint32_t)length;
    return true;
}

static bool esp_report_uart_write(const char *text)
{
    return esp_report_uart_write_bytes((const uint8_t *)text, strlen(text));
}

static bool esp_report_bytes_contains(const uint8_t *data,
                                      size_t data_length,
                                      const char *needle)
{
    const size_t needle_length = strlen(needle);
    if ((needle_length == 0U) || (data_length < needle_length))
    {
        return false;
    }
    for (size_t index = 0U; index <= data_length - needle_length; ++index)
    {
        if (memcmp(data + index, needle, needle_length) == 0)
        {
            return true;
        }
    }
    return false;
}

static uint32_t esp_report_cwjap_code_parse(const uint8_t *data,
                                            size_t data_length)
{
    static const char prefix[] = "+CWJAP:";
    const size_t prefix_length = sizeof(prefix) - 1U;
    if (data_length <= prefix_length)
    {
        return ESP_REPORT_CWJAP_CODE_NONE;
    }
    for (size_t index = 0U; index + prefix_length < data_length; ++index)
    {
        if (memcmp(data + index, prefix, prefix_length) != 0)
        {
            continue;
        }
        const uint8_t code = data[index + prefix_length];
        if ((code >= (uint8_t)'1') && (code <= (uint8_t)'4'))
        {
            return (uint32_t)(code - (uint8_t)'0');
        }
    }
    return ESP_REPORT_CWJAP_CODE_NONE;
}

static bool esp_report_wait_ascii(uint32_t timeout_ms,
                                  const char *accept1,
                                  const char *accept2)
{
    uint8_t response[768];
    size_t used = 0U;
    const rt_tick_t deadline = rt_tick_get() +
        rt_tick_from_millisecond(timeout_ms);

    memset(response, 0, sizeof(response));
    while (!esp_report_tick_due(rt_tick_get(), deadline))
    {
        uint8_t chunk[64];
        rt_ssize_t received = esp_report_uart_read(chunk, sizeof(chunk));
        if (received > 0)
        {
            size_t count = (size_t)received;
            if (count >= (sizeof(response) - used))
            {
                const size_t keep = sizeof(response) / 2U;
                if (used > keep)
                {
                    memmove(response, response + used - keep, keep);
                    used = keep;
                }
                else
                {
                    used = 0U;
                }
            }
            if (count > (sizeof(response) - used - 1U))
            {
                count = sizeof(response) - used - 1U;
            }
            memcpy(response + used, chunk, count);
            used += count;
            response[used] = '\0';
            const uint32_t cwjap_code =
                esp_report_cwjap_code_parse(response, used);
            if (cwjap_code != ESP_REPORT_CWJAP_CODE_NONE)
            {
                g_esp_report_diag.sta_last_cwjap_code = cwjap_code;
            }
            if (esp_report_bytes_contains(response, used, "ERROR") ||
                esp_report_bytes_contains(response, used, "FAIL"))
            {
                g_esp_report_diag.last_error = ESP_REPORT_ERROR_AT;
                return false;
            }
            if ((accept1 != NULL) &&
                esp_report_bytes_contains(response, used, accept1))
            {
                return true;
            }
            if ((accept2 != NULL) &&
                esp_report_bytes_contains(response, used, accept2))
            {
                return true;
            }
        }
        else
        {
            rt_thread_mdelay(10U);
        }
    }
    g_esp_report_diag.last_error = ESP_REPORT_ERROR_TIMEOUT;
    return false;
}

static bool esp_report_at_expect(const char *command,
                                 uint32_t timeout_ms,
                                 const char *accept1,
                                 const char *accept2)
{
    esp_report_uart_drain();
    return esp_report_uart_write(command) &&
           esp_report_uart_write("\r\n") &&
           esp_report_wait_ascii(timeout_ms, accept1, accept2);
}

static bool esp_report_at(const char *command, uint32_t timeout_ms)
{
    return esp_report_at_expect(command, timeout_ms, "OK", NULL);
}

static bool esp_report_wait_prompt(uint32_t timeout_ms)
{
    return esp_report_wait_ascii(timeout_ms, ">", NULL);
}

static bool esp_report_send_binary(uint8_t link_id,
                                   const uint8_t *data,
                                   size_t length,
                                   uint32_t timeout_ms)
{
    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u,%lu",
                   (unsigned int)link_id,
                   (unsigned long)length);
    esp_report_uart_drain();
    if (!esp_report_uart_write(command) ||
        !esp_report_uart_write("\r\n") ||
        !esp_report_wait_prompt(ESP_REPORT_AT_TIMEOUT_MS))
    {
        return false;
    }
    if (!esp_report_uart_write_bytes(data, length))
    {
        return false;
    }
    return esp_report_wait_ascii(timeout_ms, "SEND OK", NULL);
}

static bool esp_report_send_segments(uint8_t link_id,
                                     const uint8_t *first,
                                     size_t first_length,
                                     const uint8_t *second,
                                     size_t second_length,
                                     uint32_t timeout_ms)
{
    char command[32];
    const size_t total_length = first_length + second_length;

    (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u,%lu",
                   (unsigned int)link_id,
                   (unsigned long)total_length);
    esp_report_uart_drain();
    if (!esp_report_uart_write(command) ||
        !esp_report_uart_write("\r\n") ||
        !esp_report_wait_prompt(ESP_REPORT_AT_TIMEOUT_MS) ||
        !esp_report_uart_write_bytes(first, first_length) ||
        !esp_report_uart_write_bytes(second, second_length))
    {
        return false;
    }
    return esp_report_wait_ascii(timeout_ms, "SEND OK", NULL);
}

static bool esp_report_wait_mqtt_connack(uint32_t timeout_ms)
{
    uint8_t response[256];
    size_t used = 0U;
    const rt_tick_t deadline = rt_tick_get() +
        rt_tick_from_millisecond(timeout_ms);

    while (!esp_report_tick_due(rt_tick_get(), deadline))
    {
        uint8_t chunk[64];
        rt_ssize_t received = esp_report_uart_read(chunk, sizeof(chunk));
        if (received <= 0)
        {
            rt_thread_mdelay(10U);
            continue;
        }

        const size_t count = (size_t)received;
        if (used + count > sizeof(response))
        {
            const size_t drop = used + count - sizeof(response);
            memmove(response, response + drop, used - drop);
            used -= drop;
        }
        memcpy(response + used, chunk, count);
        used += count;

        if (esp_report_bytes_contains(response, used, "ERROR") ||
            esp_report_bytes_contains(response, used, "FAIL") ||
            esp_report_bytes_contains(response, used, "4,CLOSED"))
        {
            g_esp_report_diag.last_error = ESP_REPORT_ERROR_MQTT;
            return false;
        }

        for (size_t index = 0U; index + 5U < used; ++index)
        {
            if (memcmp(response + index, "+IPD,", 5U) != 0)
            {
                continue;
            }
            size_t cursor = index + 5U;
            while ((cursor < used) && (response[cursor] != ':') &&
                   ((cursor - (index + 5U)) < ESP_REPORT_IPD_HEADER_MAX))
            {
                ++cursor;
            }
            if ((cursor >= used) || (response[cursor] != ':'))
            {
                continue;
            }
            uint8_t link_id = 0U;
            size_t packet_length = 0U;
            if (!esp_report_http_header_parse((const char *)(response + index +
                                                               5U),
                                               cursor - (index + 5U),
                                               &link_id,
                                               &packet_length) ||
                (link_id != ESP_REPORT_MQTT_LINK_ID))
            {
                continue;
            }
            ++cursor;
            if ((packet_length < 4U) || (cursor + 4U > used))
            {
                continue;
            }
            if ((response[cursor] == 0x20U) &&
                (response[cursor + 1U] == 0x02U) &&
                (response[cursor + 2U] == 0x00U) &&
                (response[cursor + 3U] == 0x00U))
            {
                return true;
            }
            g_esp_report_diag.last_error = ESP_REPORT_ERROR_MQTT;
            return false;
        }
    }
    g_esp_report_diag.last_error = ESP_REPORT_ERROR_TIMEOUT;
    return false;
}

static bool esp_report_send_mqtt_connect(uint8_t link_id,
                                         const uint8_t *data,
                                         size_t length)
{
    char command[32];
    (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u,%lu",
                   (unsigned int)link_id,
                   (unsigned long)length);
    esp_report_uart_drain();
    if (!esp_report_uart_write(command) ||
        !esp_report_uart_write("\r\n") ||
        !esp_report_wait_prompt(ESP_REPORT_AT_TIMEOUT_MS) ||
        !esp_report_uart_write_bytes(data, length))
    {
        return false;
    }

    /* Read the send confirmation and the broker's CONNACK in one pass. The
     * ESP can coalesce both responses into a single UART read. */
    return esp_report_wait_mqtt_connack(ESP_REPORT_MQTT_TIMEOUT_MS);
}

static bool esp_report_at_ready(void)
{
    for (uint32_t attempt = 0U; attempt < 3U; ++attempt)
    {
        if (esp_report_at("AT", ESP_REPORT_AT_TIMEOUT_MS))
        {
            return true;
        }
        rt_thread_mdelay(250U);
    }
    return false;
}

static bool esp_report_http_service_start(bool start_listener)
{
    char command[192];

    if (!esp_report_at("AT+CIPMODE=0", ESP_REPORT_AT_TIMEOUT_MS) ||
        !esp_report_at("AT+CIPMUX=1", ESP_REPORT_AT_TIMEOUT_MS))
    {
        return false;
    }

    /* These commands are optional on older Nano AT builds. */
    (void)esp_report_at("AT+CIPDINFO=0", ESP_REPORT_AT_TIMEOUT_MS);
    (void)esp_report_at("AT+CIPSERVER=0", ESP_REPORT_AT_TIMEOUT_MS);
    (void)snprintf(command, sizeof(command), "AT+CIPSERVERMAXCONN=%u",
                   (unsigned int)ESP_REPORT_HTTP_MAX_CLIENTS);
    (void)esp_report_at(command, ESP_REPORT_AT_TIMEOUT_MS);

    g_esp_service_ready = true;
    g_esp_report_diag.service_ready = 1U;
    g_esp_report_diag.web_server_ready = 0U;

    if (!start_listener)
    {
        return true;
    }

    (void)snprintf(command, sizeof(command), "AT+CIPSERVER=1,%u",
                   (unsigned int)ESP_REPORT_HTTP_PORT);
    if (!esp_report_at(command, ESP_REPORT_AT_TIMEOUT_MS))
    {
        g_esp_service_ready = false;
        g_esp_report_diag.service_ready = 0U;
        return false;
    }

    g_esp_report_diag.web_server_ready = 1U;
    return true;
}

static bool esp_report_service_start(void)
{
    bool sta_connected = false;
    uint32_t sta_failure = ESP_REPORT_ERROR_NONE;
    uint32_t cwjap_code = ESP_REPORT_CWJAP_CODE_NONE;

    g_esp_service_ready = false;
    g_esp_reset_detected = false;
    g_mqtt_link_active = false;
    g_mqtt_available = false;
    g_sta_retry_requested = false;
    g_esp_report_diag.service_ready = 0U;
    g_esp_report_diag.ap_ready = 0U;
    g_esp_report_diag.sta_connected = 0U;
    g_esp_report_diag.web_server_ready = 0U;
    g_sta_attempt_ssid[0] = '\0';
    esp_report_sta_status_set(RA8P1_WIFI_DISCONNECTED, "");
    memset(g_http_queue, 0, sizeof(g_http_queue));
    memset(&g_ipd_parser, 0, sizeof(g_ipd_parser));
    g_http_queue_read = 0U;
    g_http_queue_write = 0U;
    g_http_queue_count = 0U;
    g_status_line_used = 0U;

    g_esp_report_diag.last_step = ESP_REPORT_STEP_BOOT;
    esp_report_power(true);
    g_esp_report_diag.esp_resets++;
    rt_thread_mdelay(ESP_REPORT_BOOT_DELAY_MS);

    g_esp_report_diag.last_step = ESP_REPORT_STEP_AT;
    if (!esp_report_at_ready() ||
        !esp_report_at("ATE0", ESP_REPORT_AT_TIMEOUT_MS))
    {
        return false;
    }

    /* Keep the radio in pure STA mode. Running a SoftAP on a different
     * channel can prevent older ESP AT firmware from joining REDMIha. */
    if (!esp_report_at("AT+CWMODE=1", ESP_REPORT_AT_TIMEOUT_MS))
    {
        return false;
    }
    /* Manual joins prevent credentials retained by ESP flash from overriding
     * the configured REDMIha-only policy. */
    (void)esp_report_at("AT+CWAUTOCONN=0", ESP_REPORT_AT_TIMEOUT_MS);
    (void)esp_report_at("AT+CWQAP", ESP_REPORT_AT_TIMEOUT_MS);

    if (esp_report_sta_configured())
    {
        sta_connected = esp_report_sta_connect();
        sta_failure = g_esp_report_diag.last_error;
        cwjap_code = g_esp_report_diag.sta_last_cwjap_code;
    }

    if (!esp_report_http_service_start(sta_connected))
    {
        return false;
    }

    if (esp_report_sta_configured() && !sta_connected)
    {
        g_esp_report_diag.last_step = ESP_REPORT_STEP_WIFI;
        g_esp_report_diag.last_error = sta_failure;
        g_esp_report_diag.sta_last_cwjap_code = cwjap_code;
    }
    else
    {
        g_esp_report_diag.last_step = ESP_REPORT_STEP_IDLE;
        g_esp_report_diag.last_error = ESP_REPORT_ERROR_NONE;
    }
    return true;
}

static bool esp_report_sta_connect(void)
{
    char command[192];
    const bool restore_http_service = g_esp_service_ready;
    bool connected = false;

    if (!g_esp_report_diag.uart_ready)
    {
        g_esp_report_diag.sta_connected = 0U;
        esp_report_sta_status_set(RA8P1_WIFI_DISCONNECTED, "");
        return false;
    }

    if (restore_http_service)
    {
        g_esp_service_ready = false;
        g_esp_report_diag.service_ready = 0U;
        g_esp_report_diag.ap_ready = 0U;
        g_esp_report_diag.web_server_ready = 0U;
        (void)esp_report_at("AT+CIPSERVER=0", ESP_REPORT_AT_TIMEOUT_MS);
        rt_thread_mdelay(100U);
    }

    for (size_t network_index = 0U;
         network_index < (sizeof(g_sta_networks) / sizeof(g_sta_networks[0]));
         ++network_index)
    {
        const esp_report_sta_network_t * const network =
            &g_sta_networks[network_index];
        if (network->ssid[0] == '\0')
        {
            continue;
        }
        esp_report_ssid_copy(g_sta_attempt_ssid, network->ssid);
        esp_report_sta_status_set(RA8P1_WIFI_CONNECTING,
                                  g_sta_attempt_ssid);
        (void)snprintf(command, sizeof(command),
                       "AT+CWJAP=\"%s\",\"%s\"",
                       network->ssid, network->password);
        g_esp_report_diag.sta_connect_attempts++;
        g_esp_report_diag.sta_last_cwjap_code =
            ESP_REPORT_CWJAP_CODE_NONE;
        g_esp_report_diag.last_step = ESP_REPORT_STEP_WIFI;
        if (esp_report_at(command, ESP_REPORT_WIFI_TIMEOUT_MS))
        {
            g_esp_report_diag.sta_connected = 1U;
            g_esp_report_diag.last_step = ESP_REPORT_STEP_IDLE;
            g_esp_report_diag.last_error = ESP_REPORT_ERROR_NONE;
            esp_report_sta_status_set(RA8P1_WIFI_CONNECTED,
                                      g_sta_attempt_ssid);
            connected = true;
            break;
        }
        g_esp_report_diag.sta_join_failures++;
        g_esp_report_diag.sta_last_failure =
            g_esp_report_diag.last_error;
        g_esp_report_diag.sta_connected = 0U;
        g_mqtt_available = false;
    }

    if (!connected)
    {
        g_sta_attempt_ssid[0] = '\0';
        esp_report_sta_status_set(RA8P1_WIFI_DISCONNECTED, "");
    }

    if (restore_http_service)
    {
        const uint32_t saved_error = g_esp_report_diag.last_error;
        const uint32_t saved_code =
            g_esp_report_diag.sta_last_cwjap_code;
        if (!esp_report_http_service_start(connected))
        {
            return false;
        }
        if (!connected)
        {
            g_esp_report_diag.last_step = ESP_REPORT_STEP_WIFI;
            g_esp_report_diag.last_error = saved_error;
            g_esp_report_diag.sta_last_cwjap_code = saved_code;
        }
    }
    return connected;
}

static bool esp_report_http_path_is(const esp_http_request_t *request,
                                    const char *path)
{
    static const char get_prefix[] = "GET ";
    const size_t path_length = strlen(path);
    if ((request->length < (sizeof(get_prefix) - 1U + path_length + 1U)) ||
        (memcmp(request->data, get_prefix, sizeof(get_prefix) - 1U) != 0) ||
        (memcmp(request->data + sizeof(get_prefix) - 1U,
                path, path_length) != 0))
    {
        return false;
    }
    const char terminator = request->data[sizeof(get_prefix) - 1U +
                                          path_length];
    return (terminator == ' ') || (terminator == '?');
}

static bool esp_report_http_send_response(uint8_t link_id,
                                          const char *status,
                                          const char *content_type,
                                          const char *body)
{
    char command[32];
    char header[ESP_REPORT_HTTP_HEADER_MAX];
    const size_t body_length = strlen(body);
    const uint32_t saved_step = g_esp_report_diag.last_step;
    const uint32_t saved_error = g_esp_report_diag.last_error;
    const int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, content_type, (unsigned long)body_length);
    bool sent = false;

    if ((header_length > 0) &&
        ((size_t)header_length < sizeof(header)))
    {
        sent = esp_report_send_segments(
            link_id,
            (const uint8_t *)header,
            (size_t)header_length,
            (const uint8_t *)body,
            body_length,
            ESP_REPORT_HTTP_TIMEOUT_MS);
    }

    (void)snprintf(command, sizeof(command), "AT+CIPCLOSE=%u",
                   (unsigned int)link_id);
    (void)esp_report_at(command, ESP_REPORT_AT_TIMEOUT_MS);
    g_esp_report_diag.last_step = saved_step;
    g_esp_report_diag.last_error = saved_error;
    return sent;
}

static void esp_report_http_service_one(void)
{
    char status_json[512];
    const char *status = "200 OK";
    const char *content_type = "application/json; charset=utf-8";
    const char *body = status_json;
    esp_http_request_t *request;
    uint8_t link_id;

    if (g_http_queue_count == 0U)
    {
        return;
    }

    request = &g_http_queue[g_http_queue_read];
    link_id = request->link_id;
    if (esp_report_http_path_is(request, "/api/status"))
    {
        uint32_t event = g_esp_report_diag.last_event;
        if (event == ESP_REPORT_EVENT_NONE)
        {
            event = (g_esp_report_diag.last_working_mask != 0U) ?
                    ESP_REPORT_EVENT_START : ESP_REPORT_EVENT_CLEAR;
        }
        (void)snprintf(
            status_json, sizeof(status_json),
            "{\"mask\":%lu,\"event\":\"%s\",\"generation\":%lu,"
            "\"uptime_ms\":%lu,\"publish_successes\":%lu,"
            "\"service\":%s,\"ap\":%s,\"sta\":%s,\"mqtt\":%s}",
            (unsigned long)g_esp_report_diag.last_working_mask,
            esp_report_event_name(event),
            (unsigned long)g_esp_report_diag.last_generation,
            (unsigned long)((uint64_t)rt_tick_get() * 1000ULL /
                            RT_TICK_PER_SECOND),
            (unsigned long)g_esp_report_diag.publish_successes,
            g_esp_service_ready ? "true" : "false",
            g_esp_report_diag.ap_ready ? "true" : "false",
            g_esp_report_diag.sta_connected ? "true" : "false",
            g_mqtt_available ? "true" : "false");
    }
    else if (esp_report_http_path_is(request, "/api/health"))
    {
        (void)snprintf(status_json, sizeof(status_json),
                       "{\"ok\":%s,\"web_requests\":%lu,"
                       "\"web_failures\":%lu,\"web_rx_drops\":%lu,"
                       "\"event_queue_depth\":%lu,"
                       "\"event_queue_drops\":%lu}",
                       g_esp_service_ready ? "true" : "false",
                       (unsigned long)g_esp_report_diag.web_requests,
                       (unsigned long)g_esp_report_diag.web_failures,
                       (unsigned long)g_esp_report_diag.web_rx_drops,
                       (unsigned long)g_esp_report_diag.event_queue_depth,
                       (unsigned long)g_esp_report_diag.event_queue_drops);
    }
    else if (esp_report_http_path_is(request, "/"))
    {
        content_type = "text/html; charset=utf-8";
        body = g_esp_report_web_page;
    }
    else
    {
        status = "404 Not Found";
        content_type = "text/plain; charset=utf-8";
        body = "Not found\n";
    }

    request->occupied = false;
    g_http_queue_read = (uint8_t)((g_http_queue_read + 1U) %
                                  ESP_REPORT_HTTP_QUEUE_DEPTH);
    g_http_queue_count--;
    g_esp_report_diag.web_requests++;

    if (!esp_report_http_send_response(link_id, status, content_type, body))
    {
        g_esp_report_diag.web_failures++;
    }
}

static bool mqtt311_append_byte(uint8_t *buffer,
                                size_t capacity,
                                size_t *used,
                                uint8_t value)
{
    if (*used >= capacity)
    {
        return false;
    }
    buffer[(*used)++] = value;
    return true;
}

static bool mqtt311_append_u16(uint8_t *buffer,
                               size_t capacity,
                               size_t *used,
                               uint16_t value)
{
    return mqtt311_append_byte(buffer, capacity, used,
                               (uint8_t)(value >> 8U)) &&
           mqtt311_append_byte(buffer, capacity, used,
                               (uint8_t)(value & 0xFFU));
}

static bool mqtt311_append_utf8(uint8_t *buffer,
                                size_t capacity,
                                size_t *used,
                                const char *text)
{
    const size_t length = strlen(text);
    if (length > UINT16_MAX)
    {
        return false;
    }
    if (!mqtt311_append_u16(buffer, capacity, used, (uint16_t)length))
    {
        return false;
    }
    if (length > (capacity - *used))
    {
        return false;
    }
    if (length > 0U)
    {
        memcpy(buffer + *used, text, length);
    }
    *used += length;
    return true;
}

static bool mqtt311_encode_remaining_length(size_t value,
                                            uint8_t encoded[4],
                                            size_t *encoded_length)
{
    size_t used = 0U;
    do
    {
        if (used >= 4U)
        {
            return false;
        }
        uint8_t byte = (uint8_t)(value % 128U);
        value /= 128U;
        if (value != 0U)
        {
            byte |= 0x80U;
        }
        encoded[used++] = byte;
    } while (value != 0U);
    *encoded_length = used;
    return true;
}

static bool mqtt311_finish_packet(uint8_t header,
                                  const uint8_t *body,
                                  size_t body_length,
                                  uint8_t *packet,
                                  size_t capacity,
                                  size_t *packet_length)
{
    uint8_t encoded[4];
    size_t encoded_length = 0U;
    if (!mqtt311_encode_remaining_length(body_length, encoded,
                                         &encoded_length) ||
        (1U + encoded_length + body_length > capacity))
    {
        return false;
    }
    packet[0] = header;
    memcpy(packet + 1U, encoded, encoded_length);
    if (body_length > 0U)
    {
        memcpy(packet + 1U + encoded_length, body, body_length);
    }
    *packet_length = 1U + encoded_length + body_length;
    return true;
}

static bool mqtt311_build_connect(uint8_t *packet,
                                  size_t capacity,
                                  size_t *packet_length)
{
    uint8_t body[256];
    size_t body_length = 0U;
    uint8_t flags = 0x02U; /* Clean session. */
    const bool has_username = ESP_REPORT_MQTT_USERNAME[0] != '\0';
    const bool has_password = ESP_REPORT_MQTT_PASSWORD[0] != '\0';

    if (has_password && !has_username)
    {
        return false;
    }
    if (has_username)
    {
        flags |= 0x80U;
    }
    if (has_password)
    {
        flags |= 0x40U;
    }
    if (!mqtt311_append_utf8(body, sizeof(body), &body_length, "MQTT") ||
        !mqtt311_append_byte(body, sizeof(body), &body_length, 4U) ||
        !mqtt311_append_byte(body, sizeof(body), &body_length, flags) ||
        !mqtt311_append_u16(body, sizeof(body), &body_length, 60U) ||
        !mqtt311_append_utf8(body, sizeof(body), &body_length,
                             ESP_REPORT_MQTT_CLIENT_ID))
    {
        return false;
    }
    if (has_username &&
        !mqtt311_append_utf8(body, sizeof(body), &body_length,
                             ESP_REPORT_MQTT_USERNAME))
    {
        return false;
    }
    if (has_password &&
        !mqtt311_append_utf8(body, sizeof(body), &body_length,
                             ESP_REPORT_MQTT_PASSWORD))
    {
        return false;
    }
    return mqtt311_finish_packet(0x10U, body, body_length, packet, capacity,
                                 packet_length);
}

static bool mqtt311_build_publish(const char *topic,
                                  const char *payload,
                                  uint8_t *packet,
                                  size_t capacity,
                                  size_t *packet_length)
{
    uint8_t body[512];
    size_t body_length = 0U;
    if (!mqtt311_append_utf8(body, sizeof(body), &body_length, topic))
    {
        return false;
    }
    const size_t payload_length = strlen(payload);
    if (payload_length > (sizeof(body) - body_length))
    {
        return false;
    }
    memcpy(body + body_length, payload, payload_length);
    body_length += payload_length;
    return mqtt311_finish_packet(0x30U, body, body_length, packet, capacity,
                                 packet_length);
}

static bool mqtt311_build_disconnect(uint8_t packet[2], size_t *packet_length)
{
    packet[0] = 0xE0U;
    packet[1] = 0x00U;
    *packet_length = 2U;
    return true;
}

static const char *esp_report_event_name(uint32_t event)
{
    switch (event)
    {
        case ESP_REPORT_EVENT_START:  return "START";
        case ESP_REPORT_EVENT_CLEAR:  return "CLEAR";
        case ESP_REPORT_EVENT_UPDATE: return "UPDATE";
        case ESP_REPORT_EVENT_NORMAL: return "NORMAL";
        default:                      return "UNKNOWN";
    }
}

static bool esp_report_publish(uint32_t event,
                               uint32_t generation,
                               uint32_t working_mask)
{
    char payload[256];
    char command[256];
    uint8_t connect_packet[320];
    uint8_t publish_packet[640];
    uint8_t disconnect_packet[2];
    size_t connect_length = 0U;
    size_t publish_length = 0U;
    size_t disconnect_length = 0U;
    bool success = false;
    bool tcp_connected = false;
    uint32_t failed_step = ESP_REPORT_STEP_IDLE;
    uint32_t failed_error = ESP_REPORT_ERROR_NONE;

    g_esp_report_diag.publish_attempts++;
    g_esp_report_diag.last_event = event;
    g_esp_report_diag.last_step = ESP_REPORT_STEP_TCP;
    g_esp_report_diag.last_error = ESP_REPORT_ERROR_NONE;
    if (!g_esp_service_ready || !g_esp_report_diag.sta_connected)
    {
        goto done;
    }
    (void)snprintf(command, sizeof(command),
                   "AT+CIPSTART=%u,\"TCP\",\"%s\",%u",
                   (unsigned int)ESP_REPORT_MQTT_LINK_ID,
                   ESP_REPORT_MQTT_HOST,
                   (unsigned int)ESP_REPORT_MQTT_PORT);
    g_esp_report_diag.last_step = ESP_REPORT_STEP_TCP;
    if (!esp_report_at_expect(command, ESP_REPORT_TCP_TIMEOUT_MS,
                               "CONNECT", "OK"))
    {
        goto done;
    }
    tcp_connected = true;
    g_mqtt_link_active = true;
    g_esp_report_diag.tcp_connects++;
    g_esp_report_diag.last_step = ESP_REPORT_STEP_MQTT;
    if (!mqtt311_build_connect(connect_packet, sizeof(connect_packet),
                                &connect_length) ||
        !esp_report_send_mqtt_connect(ESP_REPORT_MQTT_LINK_ID,
                                      connect_packet, connect_length))
    {
        goto done;
    }
    g_esp_report_diag.mqtt_connects++;
    (void)snprintf(payload, sizeof(payload),
                   "{\"device\":\"%s\",\"event\":\"%s\","
                   "\"mask\":%lu,\"generation\":%lu,\"uptime_ms\":%lu}",
                   ESP_REPORT_DEVICE_ID,
                   esp_report_event_name(event),
                   (unsigned long)working_mask,
                   (unsigned long)generation,
                   (unsigned long)((uint64_t)rt_tick_get() * 1000ULL /
                                   RT_TICK_PER_SECOND));
    g_esp_report_diag.last_step = ESP_REPORT_STEP_PUBLISH;
    if (!mqtt311_build_publish(ESP_REPORT_MQTT_TOPIC, payload,
                                publish_packet, sizeof(publish_packet),
                                &publish_length) ||
        !esp_report_send_binary(ESP_REPORT_MQTT_LINK_ID,
                                publish_packet, publish_length,
                                ESP_REPORT_MQTT_TIMEOUT_MS))
    {
        goto done;
    }
    g_esp_report_diag.mqtt_publish_count++;
    success = true;

done:
    failed_step = g_esp_report_diag.last_step;
    failed_error = g_esp_report_diag.last_error;
    g_esp_report_diag.last_step = ESP_REPORT_STEP_CLOSE;
    if (tcp_connected &&
        mqtt311_build_disconnect(disconnect_packet, &disconnect_length))
    {
        (void)esp_report_send_binary(ESP_REPORT_MQTT_LINK_ID,
                                     disconnect_packet, disconnect_length,
                                     ESP_REPORT_MQTT_TIMEOUT_MS);
    }
    (void)snprintf(command, sizeof(command), "AT+CIPCLOSE=%u",
                   (unsigned int)ESP_REPORT_MQTT_LINK_ID);
    (void)esp_report_at(command, ESP_REPORT_AT_TIMEOUT_MS);
    g_mqtt_link_active = false;
    g_esp_report_diag.last_publish_tick = (uint32_t)rt_tick_get();
    if (success)
    {
        g_mqtt_available = true;
        g_esp_report_diag.last_step = ESP_REPORT_STEP_IDLE;
        g_esp_report_diag.last_error = ESP_REPORT_ERROR_NONE;
        g_esp_report_diag.publish_successes++;
        if (event == ESP_REPORT_EVENT_NORMAL)
        {
            g_esp_report_diag.normal_reports++;
        }
        else
        {
            g_esp_report_diag.event_reports++;
        }
    }
    else
    {
        g_mqtt_available = false;
        g_esp_report_diag.last_step = failed_step;
        g_esp_report_diag.last_error =
            (failed_error != ESP_REPORT_ERROR_NONE) ? failed_error :
                                                     ESP_REPORT_ERROR_MQTT;
        g_esp_report_diag.publish_failures++;
    }
    return success;
}

static bool esp_report_sta_configured(void)
{
    return (ESP_REPORT_WIFI_PRIMARY_SSID[0] != '\0') ||
           (ESP_REPORT_WIFI_SECONDARY_SSID[0] != '\0');
}

static bool esp_report_mqtt_configured(void)
{
    return esp_report_sta_configured() &&
           (ESP_REPORT_MQTT_HOST[0] != '\0') &&
           (ESP_REPORT_MQTT_TOPIC[0] != '\0');
}

static bool esp_report_configured(void)
{
    return ESP_REPORT_ENABLE && esp_report_sta_configured();
}

static void esp_report_collector_thread_entry(void *parameter)
{
    uint32_t generation = 0U;
    uint32_t working_mask = 0U;
    uint32_t last_generation = 0U;
    uint32_t last_mask = 0U;
    bool have_state = false;

    (void)parameter;
    while (true)
    {
        if (ipc_bridge_cpu0_activity_report_read(&generation, &working_mask) &&
            ((!have_state) || (generation != last_generation) ||
             (working_mask != last_mask)))
        {
            uint32_t event = ESP_REPORT_EVENT_NONE;
            if (!have_state)
            {
                if (working_mask != 0U)
                {
                    event = ESP_REPORT_EVENT_START;
                }
                have_state = true;
            }
            else if (working_mask != last_mask)
            {
                event = (last_mask == 0U) ? ESP_REPORT_EVENT_START :
                        ((working_mask == 0U) ? ESP_REPORT_EVENT_CLEAR :
                                               ESP_REPORT_EVENT_UPDATE);
            }

            {
                const rt_base_t level = rt_hw_interrupt_disable();
                g_activity_generation = generation;
                g_activity_working_mask = working_mask;
                g_activity_state_ready = true;
                g_esp_report_diag.last_generation = generation;
                g_esp_report_diag.last_working_mask = working_mask;
                if (event != ESP_REPORT_EVENT_NONE)
                {
                    g_esp_report_diag.last_event = event;
                    g_esp_report_diag.activity_edges++;
                }
                rt_hw_interrupt_enable(level);
            }
            if ((event != ESP_REPORT_EVENT_NONE) &&
                esp_report_mqtt_configured())
            {
                esp_report_event_queue_push(event, generation, working_mask);
            }
            last_generation = generation;
            last_mask = working_mask;
        }
        rt_thread_mdelay(ESP_REPORT_POLL_INTERVAL_MS);
    }
}

static void esp_report_thread_entry(void *parameter)
{
    uint32_t current_generation = 0U;
    uint32_t current_mask = 0U;
    uint32_t pending_event = ESP_REPORT_EVENT_NONE;
    uint32_t pending_generation = 0U;
    uint32_t pending_mask = 0U;
    rt_tick_t next_normal;
    rt_tick_t next_retry = 0U;
    rt_tick_t next_service_retry = 0U;
    rt_tick_t next_sta_retry = 0U;
    bool have_state = false;

    (void)parameter;
    esp_report_power(false);
    next_normal = rt_tick_get() +
        rt_tick_from_millisecond(ESP_REPORT_NORMAL_INTERVAL_MS);

    if (!g_esp_report_diag.configured)
    {
        rt_kprintf("[esp] service disabled: configure Wi-Fi STA\n");
        while (true)
        {
            rt_thread_mdelay(5000U);
        }
    }

    g_esp_uart = rt_device_find(ESP_REPORT_UART_DEVICE);
    if ((g_esp_uart == RT_NULL) ||
        (rt_device_open(g_esp_uart,
                        RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX) != RT_EOK))
    {
        rt_kprintf("[esp] cannot open %s\n", ESP_REPORT_UART_DEVICE);
        while (true)
        {
            rt_thread_mdelay(5000U);
        }
    }
    g_esp_report_diag.uart_ready = 1U;
    next_service_retry = rt_tick_get();
    next_sta_retry = rt_tick_get();

    while (true)
    {
        const rt_tick_t now = rt_tick_get();
        esp_report_uart_drain();

        if (g_esp_reset_detected)
        {
            g_esp_reset_detected = false;
            next_service_retry = now;
        }
        if (!g_esp_service_ready &&
            esp_report_tick_due(now, next_service_retry))
        {
            if (esp_report_service_start())
            {
                next_sta_retry = rt_tick_get();
            }
            else
            {
                esp_report_power(false);
                next_service_retry = rt_tick_get() +
                    rt_tick_from_millisecond(ESP_REPORT_SERVICE_RETRY_MS);
            }
        }

        if (esp_report_activity_snapshot(&current_generation, &current_mask) &&
            !have_state)
        {
            have_state = true;
            if (current_mask == 0U)
            {
                next_normal = now +
                    rt_tick_from_millisecond(ESP_REPORT_NORMAL_INTERVAL_MS);
            }
        }
        else if (have_state)
        {
            (void)esp_report_activity_snapshot(&current_generation,
                                               &current_mask);
        }

        if (pending_event == ESP_REPORT_EVENT_NONE)
        {
            esp_report_event_record_t record;
            if (esp_report_event_queue_pop(&record))
            {
                pending_event = record.event;
                pending_generation = record.generation;
                pending_mask = record.working_mask;
                next_retry = now;
            }
        }

        /* Serve an already received browser request before beginning any
         * potentially slow STA or MQTT operation. */
        if (g_esp_report_diag.web_server_ready)
        {
            esp_report_http_service_one();
        }

        if (g_esp_service_ready &&
            esp_report_sta_configured() &&
            !g_esp_report_diag.sta_connected &&
            (g_sta_retry_requested ||
             esp_report_tick_due(rt_tick_get(), next_sta_retry)))
        {
            g_sta_retry_requested = false;
            (void)esp_report_sta_connect();
            /* A failed CWJAP may itself emit WIFI DISCONNECT. Do not turn
             * that synchronous failure into an unbounded retry loop. */
            g_sta_retry_requested = false;
            next_sta_retry = rt_tick_get() +
                rt_tick_from_millisecond(ESP_REPORT_STA_RETRY_MS);
        }

        if ((pending_event != ESP_REPORT_EVENT_NONE) &&
            g_esp_report_diag.sta_connected &&
            esp_report_tick_due(rt_tick_get(), next_retry))
        {
            if (esp_report_publish(pending_event,
                                   pending_generation,
                                   pending_mask))
            {
                if (pending_event == ESP_REPORT_EVENT_CLEAR)
                {
                    next_normal = rt_tick_get() +
                        rt_tick_from_millisecond(
                            ESP_REPORT_NORMAL_INTERVAL_MS);
                }
                pending_event = ESP_REPORT_EVENT_NONE;
            }
            else
            {
                next_retry = rt_tick_get() +
                    rt_tick_from_millisecond(ESP_REPORT_EVENT_RETRY_MS);
            }
        }
        else if (have_state && (current_mask == 0U) &&
                  (pending_event == ESP_REPORT_EVENT_NONE) &&
                  esp_report_mqtt_configured() &&
                  g_esp_report_diag.sta_connected &&
                  esp_report_tick_due(rt_tick_get(), next_normal))
        {
            (void)esp_report_publish(ESP_REPORT_EVENT_NORMAL,
                                     current_generation, 0U);
            next_normal = rt_tick_get() +
                rt_tick_from_millisecond(ESP_REPORT_NORMAL_INTERVAL_MS);
        }
        rt_thread_mdelay(ESP_REPORT_POLL_INTERVAL_MS);
    }
}

void esp_report_start(void)
{
    if (g_esp_report_started)
    {
        return;
    }
    memset((void *)&g_esp_report_diag, 0, sizeof(g_esp_report_diag));
    memset(g_event_queue, 0, sizeof(g_event_queue));
    g_event_queue_read = 0U;
    g_event_queue_write = 0U;
    g_event_queue_count = 0U;
    g_activity_state_ready = false;
    g_activity_generation = 0U;
    g_activity_working_mask = 0U;
    g_sta_connection_state = RA8P1_WIFI_DISCONNECTED;
    g_sta_retry_requested = false;
    g_sta_attempt_ssid[0] = '\0';
    g_sta_connected_ssid[0] = '\0';
    ipc_bridge_cpu0_wifi_status_publish(RA8P1_WIFI_DISCONNECTED, "");
    g_esp_report_diag.magic = ESP_REPORT_DIAG_MAGIC;
    g_esp_report_diag.version = ESP_REPORT_DIAG_VERSION;
    g_esp_report_diag.size = (uint16_t)sizeof(g_esp_report_diag);
    g_esp_report_diag.configured = esp_report_configured() ? 1U : 0U;
    esp_report_power(false);

    if ((rt_thread_init(&g_esp_collector_thread,
                        "espstate",
                        esp_report_collector_thread_entry,
                        RT_NULL,
                        g_esp_collector_stack,
                        sizeof(g_esp_collector_stack),
                        ESP_REPORT_COLLECTOR_PRIORITY,
                        ESP_REPORT_COLLECTOR_TICK) == RT_EOK) &&
        (rt_thread_init(&g_esp_report_thread,
                       "esprpt",
                       esp_report_thread_entry,
                       RT_NULL,
                       g_esp_report_stack,
                       sizeof(g_esp_report_stack),
                       ESP_REPORT_THREAD_PRIORITY,
                       ESP_REPORT_THREAD_TICK) == RT_EOK))
    {
        g_esp_report_started = true;
        g_esp_report_diag.thread_started = 1U;
        g_esp_report_diag.collector_started = 1U;
        (void)rt_thread_startup(&g_esp_collector_thread);
        (void)rt_thread_startup(&g_esp_report_thread);
    }
    else
    {
        rt_kprintf("[esp] report thread start failed\n");
    }
}
