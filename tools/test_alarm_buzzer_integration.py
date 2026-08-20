from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "cpu1/src/framework/alarm_buzzer.c").read_text(encoding="utf-8")
HEADER = (ROOT / "cpu1/src/framework/alarm_buzzer.h").read_text(encoding="utf-8")
PIN_HEADER = (ROOT / "cpu1/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h").read_text(encoding="utf-8")
DISPLAY = (ROOT / "cpu1/src/framework/display_app.c").read_text(encoding="utf-8")
WARMSTART = (ROOT / "cpu1/src/hal_warmstart.c").read_text(encoding="utf-8")
RF_UI = (ROOT / "cpu1/src/ui/rf_ui.c").read_text(encoding="utf-8")
RF_UI_HEADER = (ROOT / "cpu1/src/ui/rf_ui.h").read_text(encoding="utf-8")


def require(text, fragment):
    if fragment not in text:
        raise AssertionError(f"missing {fragment!r}")


require(HEADER, "ALARM_BUZZER_CYCLE_MS     (1000U)")
require(HEADER, "ALARM_BUZZER_ACTIVE_MS    (500U)")
require(HEADER, "ALARM_BUZZER_DIAG_VERSION (2U)")
require(HEADER, "uint32_t muted;")
require(PIN_HEADER, "#define ALARM_BUZZER (BSP_IO_PORT_05_PIN_15)")
require(SERVICE, "ALARM_BUZZER")
require(SERVICE, "BSP_IO_LEVEL_LOW")
require(SERVICE, "BSP_IO_LEVEL_HIGH")
require(SERVICE, "RF_V27_ACTIVITY_WORKING")
require(SERVICE, "RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET")
require(SERVICE, "(void) alarm_buzzer_write(false);\n        return;")
require(DISPLAY, "alarm_buzzer_apply_round")
require(DISPLAY, "alarm_buzzer_step")
require(DISPLAY, "alarm_buzzer_force_off")
require(DISPLAY, "display_app_set_alarm_muted")
require(DISPLAY, "display_app_alarm_muted")
require(WARMSTART, "ALARM_BUZZER")
require(WARMSTART, "BSP_IO_LEVEL_HIGH")
mute = SERVICE.split("void alarm_buzzer_set_muted", 1)[1].split(
    "bool alarm_buzzer_is_muted", 1
)[0]
require(mute, "alarm_buzzer_write(!muted && g_alarm_buzzer_requested)")
if "g_alarm_buzzer_requested =" in mute:
    raise AssertionError("muting must not clear the active alarm request")
require(SERVICE, "if (!g_alarm_buzzer_requested || g_alarm_buzzer_muted)")
require(RF_UI, "RF_UI_INPUT_CONTROL_ALARM_MUTE")
require(RF_UI, "LV_SYMBOL_MUTE")
require(RF_UI, "LV_SYMBOL_VOLUME_MAX")
require(RF_UI, "display_app_set_alarm_muted(muted)")
require(RF_UI_HEADER, "RF_UI_INPUT_DIAG_VERSION          2u")
print("alarm buzzer integration: PASS")
