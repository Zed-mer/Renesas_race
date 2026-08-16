from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "cpu1/src/framework/alarm_buzzer.c").read_text(encoding="utf-8")
HEADER = (ROOT / "cpu1/src/framework/alarm_buzzer.h").read_text(encoding="utf-8")
DISPLAY = (ROOT / "cpu1/src/framework/display_app.c").read_text(encoding="utf-8")
WARMSTART = (ROOT / "cpu1/src/hal_warmstart.c").read_text(encoding="utf-8")


def require(text, fragment):
    if fragment not in text:
        raise AssertionError(f"missing {fragment!r}")


require(HEADER, "ALARM_BUZZER_CYCLE_MS     (1000U)")
require(HEADER, "ALARM_BUZZER_ACTIVE_MS    (500U)")
require(SERVICE, "ALARM_BUZZER")
require(SERVICE, "BSP_IO_LEVEL_LOW")
require(SERVICE, "BSP_IO_LEVEL_HIGH")
require(SERVICE, "RF_V27_ACTIVITY_WORKING")
require(SERVICE, "RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET")
require(SERVICE, "(void) alarm_buzzer_write(false);\n        return;")
require(DISPLAY, "alarm_buzzer_apply_round")
require(DISPLAY, "alarm_buzzer_step")
require(DISPLAY, "alarm_buzzer_force_off")
require(WARMSTART, "ALARM_BUZZER")
require(WARMSTART, "BSP_IO_LEVEL_HIGH")
print("alarm buzzer integration: PASS")
