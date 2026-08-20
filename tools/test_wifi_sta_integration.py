#!/usr/bin/env python3
"""Static integration checks for prioritized ESP STA status reporting."""

from __future__ import annotations

import ctypes
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG = (ROOT / "cpu0/src/framework/esp_report_config.h").read_text(encoding="utf-8")
ESP = (ROOT / "cpu0/src/framework/esp_report.c").read_text(encoding="utf-8")
ESP_HEADER = (ROOT / "cpu0/src/framework/esp_report.h").read_text(encoding="utf-8")
ESP_WEB = (ROOT / "cpu0/src/framework/esp_report_web.h").read_text(encoding="utf-8")
CPU0_IPC = (ROOT / "cpu0/src/framework/ipc_bridge.c").read_text(encoding="utf-8")
CPU1_IPC = (ROOT / "cpu1/src/framework/ipc_bridge.c").read_text(encoding="utf-8")
DISPLAY = (ROOT / "cpu1/src/framework/display_app.c").read_text(encoding="utf-8")
LVGL = (ROOT / "cpu1/src/lvgl_app.c").read_text(encoding="utf-8")
RF_UI = (ROOT / "cpu1/src/ui/rf_ui.c").read_text(encoding="utf-8")
MAILBOX = (ROOT / "shared/wifi_status_mailbox.h").read_text(encoding="utf-8")
LAYOUT = (ROOT / "shared/resource_layout.h").read_text(encoding="utf-8")


def integer_define(source: str, name: str) -> int:
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+\(?\s*(0x[0-9A-Fa-f]+|[0-9]+)",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"missing integer define {name}")
    return int(match.group(1), 0)


class WifiStatusMailbox(ctypes.LittleEndianStructure):
    _fields_ = [
        ("begin_sequence", ctypes.c_uint32),
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("size", ctypes.c_uint16),
        ("cpu0_boot_epoch", ctypes.c_uint32),
        ("generation", ctypes.c_uint32),
        ("connection_state", ctypes.c_uint32),
        ("ssid", ctypes.c_char * 33),
        ("reserved", ctypes.c_uint8 * 3),
        ("end_sequence", ctypes.c_uint32),
    ]


class WifiStaIntegrationTests(unittest.TestCase):
    def test_softap_is_removed_and_http_is_sta_only(self) -> None:
        self.assertNotIn("ESP_REPORT_AP_", CONFIG)
        self.assertNotIn("AT+CWMODE=3", ESP)
        self.assertNotIn("AT+CWSAP", ESP)
        self.assertIn('esp_report_at("AT+CWMODE=1"', ESP)
        self.assertIn("esp_report_http_service_start(sta_connected)", ESP)
        self.assertIn("esp_report_http_service_start(connected)", ESP)
        self.assertIn("if (!start_listener)", ESP)
        self.assertIn("g_esp_report_diag.web_server_ready", ESP)
        configured = ESP.split("static bool esp_report_configured(void)", 1)[1].split(
            "static void esp_report_collector_thread_entry", 1
        )[0]
        self.assertNotIn("ESP_REPORT_AP", configured)
        self.assertIn("esp_report_sta_configured()", configured)
        self.assertNotIn("192.168.4.1", ESP_WEB)
        self.assertNotIn("'AP '", ESP_WEB)
        self.assertIn("Connected Wi-Fi network", ESP_WEB)
        # Keep the legacy diagnostic slot stable for existing memory readers.
        self.assertIn("uint32_t ap_ready;", ESP_HEADER)

    def test_only_verified_redmi_hotspot_is_attempted(self) -> None:
        self.assertIn('#define ESP_REPORT_WIFI_PRIMARY_SSID       "REDMIha"', CONFIG)
        self.assertIn('#define ESP_REPORT_WIFI_PRIMARY_PASSWORD   "lzhdasb1"', CONFIG)
        self.assertIn('#define ESP_REPORT_WIFI_SECONDARY_SSID     ""', CONFIG)
        self.assertIn('#define ESP_REPORT_WIFI_SECONDARY_PASSWORD ""', CONFIG)
        self.assertNotIn('"zed"', CONFIG)
        networks = ESP.split("static const esp_report_sta_network_t g_sta_networks[]", 1)[
            1
        ].split("};", 1)[0]
        self.assertLess(networks.index("ESP_REPORT_WIFI_PRIMARY_SSID"),
                        networks.index("ESP_REPORT_WIFI_SECONDARY_SSID"))
        connect = ESP.split("static bool esp_report_sta_connect(void)", 1)[1].split(
            "static bool esp_report_http_path_is", 1
        )[0]
        self.assertIn("for (size_t network_index = 0U;", connect)
        self.assertIn('AT+CWJAP=\\\"%s\\\",\\\"%s\\\"', connect)
        self.assertIn("RA8P1_WIFI_CONNECTING", connect)
        self.assertIn("RA8P1_WIFI_CONNECTED", connect)
        self.assertIn('esp_report_at("AT+CWAUTOCONN=0"', ESP)
        self.assertIn('esp_report_at("AT+CWQAP"', ESP)

    def test_join_failures_are_diagnosable_and_retried_in_30_seconds(self) -> None:
        self.assertEqual(30000, integer_define(CONFIG, "ESP_REPORT_STA_RETRY_MS"))
        self.assertEqual(20000, integer_define(CONFIG, "ESP_REPORT_WIFI_TIMEOUT_MS"))
        self.assertIn('static const char prefix[] = "+CWJAP:";', ESP)
        self.assertIn("g_esp_report_diag.sta_join_failures++", ESP)
        self.assertIn("g_esp_report_diag.sta_last_cwjap_code", ESP)
        self.assertIn("g_esp_report_diag.sta_last_failure", ESP)
        for code in range(1, 5):
            self.assertRegex(
                ESP_HEADER,
                rf"#define ESP_REPORT_CWJAP_CODE_[A-Z_]+\s+\({code}U\)",
            )

    def test_mailbox_is_one_cache_line_and_does_not_expose_passwords(self) -> None:
        self.assertEqual(64, ctypes.sizeof(WifiStatusMailbox))
        self.assertEqual(60, WifiStatusMailbox.end_sequence.offset)
        wifi_offset = integer_define(LAYOUT, "RA8P1_WIFI_STATUS_OFFSET")
        wifi_bytes = integer_define(LAYOUT, "RA8P1_WIFI_STATUS_BYTES")
        activity_end = (
            integer_define(LAYOUT, "RA8P1_ACTIVITY_CONTROL_OFFSET")
            + integer_define(LAYOUT, "RA8P1_ACTIVITY_CONTROL_BYTES")
        )
        display_offset = integer_define(LAYOUT, "RA8P1_DISPLAY_STREAM_OFFSET")
        self.assertEqual(0x140, wifi_offset)
        self.assertEqual(64, wifi_bytes)
        self.assertLessEqual(activity_end, wifi_offset)
        self.assertLessEqual(wifi_offset + wifi_bytes, display_offset)
        self.assertIn("sizeof(ra8p1_wifi_status_mailbox_t) == RA8P1_WIFI_STATUS_BYTES", MAILBOX)
        mailbox_fields = MAILBOX.split(
            "typedef struct st_ra8p1_wifi_status_mailbox", 1
        )[1].split("} ra8p1_wifi_status_mailbox_t;", 1)[0]
        self.assertNotIn("password", mailbox_fields.lower())
        self.assertNotIn("qwertyuiop", mailbox_fields)
        self.assertNotIn("lzhdasb1", mailbox_fields)

    def test_even_seqlock_epoch_and_cache_contract_are_consumed_by_cpu1(self) -> None:
        publish = CPU0_IPC.split("void ipc_bridge_cpu0_wifi_status_publish", 1)[1].split(
            "static void ipc_cpu0_activity_init", 1
        )[0]
        poll = CPU1_IPC.split("bool ipc_bridge_cpu1_wifi_status_poll", 1)[1].split(
            "bool ipc_bridge_cpu1_activity_poll", 1
        )[0]
        self.assertIn("status->begin_sequence = sequence | 1U", publish)
        self.assertIn("status->end_sequence = sequence", publish)
        self.assertIn("status->begin_sequence = sequence", publish)
        self.assertIn("ipc_cpu0_wifi_status_clean()", publish)
        self.assertIn("SCB_InvalidateDCache_by_Addr", poll)
        self.assertIn("snapshot.cpu0_boot_epoch != g_observed_cpu0_boot_epoch", poll)
        self.assertIn("begin != end", poll)
        self.assertIn("begin != final_begin", poll)

    def test_status_reaches_the_header_with_ssid_and_no_password(self) -> None:
        self.assertIn("ipc_bridge_cpu1_wifi_status_poll(&wifi_status)", DISPLAY)
        self.assertIn("lvgl_app_wifi_status_update(&wifi_status)", DISPLAY)
        self.assertIn("rf_ui_set_wifi_status(", LVGL)
        self.assertIn('"WiFi %.*s OK"', RF_UI)
        self.assertIn('"WiFi %.*s ..."', RF_UI)
        self.assertIn('"WiFi OFFLINE"', RF_UI)
        self.assertNotIn("qwertyuiop", RF_UI)
        self.assertNotIn("lzhdasb1", RF_UI)


if __name__ == "__main__":
    unittest.main()
