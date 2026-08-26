#include "alarm_buzzer.h"

#include "hal_data.h"

#define ALARM_BUZZER_PIN_CFG \
    ((uint32_t) IOPORT_CFG_DRIVE_MID | \
     (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
     (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)

volatile alarm_buzzer_diag_t g_alarm_buzzer_diag;

static bool g_alarm_buzzer_initialized;
static bool g_alarm_buzzer_requested;
static bool g_alarm_buzzer_output_active;
static bool g_alarm_buzzer_muted;
static uint32_t g_alarm_buzzer_cycle_start_ms;

static fsp_err_t alarm_buzzer_write(bool active)
{
    const bsp_io_level_t level = active ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH;
    const fsp_err_t error = R_IOPORT_PinWrite(g_ioport.p_ctrl,
                                              ALARM_BUZZER,
                                              level);
    if (FSP_SUCCESS != error)
    {
        g_alarm_buzzer_diag.last_write_error = (uint32_t) error;
        g_alarm_buzzer_diag.write_failures++;
        return error;
    }
    if (g_alarm_buzzer_output_active != active)
    {
        g_alarm_buzzer_diag.transitions++;
    }
    g_alarm_buzzer_output_active = active;
    g_alarm_buzzer_diag.output_active = active ? 1U : 0U;
    return FSP_SUCCESS;
}

static bool alarm_buzzer_round_requests_alarm(
    const rf_v27_activity_round_decision_t *decision)
{
    if (decision == NULL ||
        (decision->flags & RF_V27_ROUND_DECISION_OUTPUT_VALID) == 0U)
    {
        return g_alarm_buzzer_requested;
    }
    for (uint32_t object = 0U; object < RF_V13_OBJECT_COUNT; ++object)
    {
        if (decision->object_activity_state[object] ==
            RF_V25_ACTIVITY_WORKING)
        {
            return true;
        }
    }
    return false;
}

void alarm_buzzer_init(void)
{
    fsp_err_t error;
    g_alarm_buzzer_initialized = false;
    g_alarm_buzzer_requested = false;
    g_alarm_buzzer_output_active = false;
    g_alarm_buzzer_muted = false;
    g_alarm_buzzer_cycle_start_ms = 0U;
    g_alarm_buzzer_diag.magic = ALARM_BUZZER_DIAG_MAGIC;
    g_alarm_buzzer_diag.version = ALARM_BUZZER_DIAG_VERSION;
    g_alarm_buzzer_diag.size = (uint16_t) sizeof(g_alarm_buzzer_diag);
    g_alarm_buzzer_diag.initialized = 0U;
    g_alarm_buzzer_diag.init_error = 0U;
    g_alarm_buzzer_diag.last_write_error = 0U;
    g_alarm_buzzer_diag.write_failures = 0U;
    g_alarm_buzzer_diag.transitions = 0U;
    g_alarm_buzzer_diag.request_active = 0U;
    g_alarm_buzzer_diag.output_active = 0U;
    g_alarm_buzzer_diag.last_tick_ms = 0U;
    g_alarm_buzzer_diag.cycle_start_ms = 0U;
    g_alarm_buzzer_diag.muted = 0U;

    /* The generated pin configuration also starts HIGH.  Re-assert it here
     * after the shared IOPORT instance is open so a warm reset stays silent. */
    error = R_IOPORT_PinCfg(g_ioport.p_ctrl, ALARM_BUZZER,
                            ALARM_BUZZER_PIN_CFG);
    if (FSP_SUCCESS != error)
    {
        g_alarm_buzzer_diag.init_error = (uint32_t) error;
        return;
    }
    error = alarm_buzzer_write(false);
    if (FSP_SUCCESS != error)
    {
        g_alarm_buzzer_diag.init_error = (uint32_t) error;
        return;
    }
    g_alarm_buzzer_initialized = true;
    g_alarm_buzzer_diag.initialized = 1U;
}

void alarm_buzzer_apply_round(
    const rf_v27_activity_round_decision_t *decision,
    uint32_t now_ms)
{
    bool requested;
    if (!g_alarm_buzzer_initialized || decision == NULL)
    {
        return;
    }
    if ((decision->flags & RF_V27_ROUND_DECISION_CPU0_EPOCH_RESET) != 0U)
    {
        g_alarm_buzzer_requested = false;
        g_alarm_buzzer_diag.request_active = 0U;
        (void) alarm_buzzer_write(false);
        return;
    }
    requested = alarm_buzzer_round_requests_alarm(decision);
    if (requested != g_alarm_buzzer_requested)
    {
        g_alarm_buzzer_requested = requested;
        g_alarm_buzzer_diag.request_active = requested ? 1U : 0U;
        g_alarm_buzzer_cycle_start_ms = now_ms;
        g_alarm_buzzer_diag.cycle_start_ms = now_ms;
        (void) alarm_buzzer_write(requested && !g_alarm_buzzer_muted);
    }
    if (!requested)
    {
        (void) alarm_buzzer_write(false);
    }
}

void alarm_buzzer_step(uint32_t now_ms)
{
    uint32_t elapsed;
    bool active;
    if (!g_alarm_buzzer_initialized)
    {
        return;
    }
    g_alarm_buzzer_diag.last_tick_ms = now_ms;
    if (!g_alarm_buzzer_requested || g_alarm_buzzer_muted)
    {
        (void) alarm_buzzer_write(false);
        return;
    }
    elapsed = now_ms - g_alarm_buzzer_cycle_start_ms;
    active = (elapsed % ALARM_BUZZER_CYCLE_MS) < ALARM_BUZZER_ACTIVE_MS;
    (void) alarm_buzzer_write(active);
}

void alarm_buzzer_set_muted(bool muted, uint32_t now_ms)
{
    if (g_alarm_buzzer_muted == muted)
    {
        return;
    }
    g_alarm_buzzer_muted = muted;
    g_alarm_buzzer_diag.muted = muted ? 1U : 0U;
    g_alarm_buzzer_cycle_start_ms = now_ms;
    g_alarm_buzzer_diag.cycle_start_ms = now_ms;
    if (g_alarm_buzzer_initialized)
    {
        (void) alarm_buzzer_write(!muted && g_alarm_buzzer_requested);
    }
}

bool alarm_buzzer_is_muted(void)
{
    return g_alarm_buzzer_muted;
}

void alarm_buzzer_force_off(void)
{
    g_alarm_buzzer_requested = false;
    g_alarm_buzzer_diag.request_active = 0U;
    if (g_alarm_buzzer_initialized)
    {
        (void) alarm_buzzer_write(false);
    }
}
