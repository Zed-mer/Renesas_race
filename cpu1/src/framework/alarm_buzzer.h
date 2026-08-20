#ifndef CPU1_ALARM_BUZZER_H
#define CPU1_ALARM_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

#include "activity_service.h"

#define ALARM_BUZZER_DIAG_MAGIC   (0x425A5231UL) /* BZR1 */
#define ALARM_BUZZER_DIAG_VERSION (2U)

/* A one-second alarm cadence: 500 ms active, 500 ms silent. */
#define ALARM_BUZZER_CYCLE_MS     (1000U)
#define ALARM_BUZZER_ACTIVE_MS    (500U)

typedef struct st_alarm_buzzer_diag
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t initialized;
    uint32_t init_error;
    uint32_t last_write_error;
    uint32_t write_failures;
    uint32_t transitions;
    uint32_t request_active;
    uint32_t output_active;
    uint32_t last_tick_ms;
    uint32_t cycle_start_ms;
    uint32_t muted;
} alarm_buzzer_diag_t;

extern volatile alarm_buzzer_diag_t g_alarm_buzzer_diag;

void alarm_buzzer_init(void);
void alarm_buzzer_apply_round(
    const rf_v27_activity_round_decision_t *decision,
    uint32_t now_ms);
void alarm_buzzer_step(uint32_t now_ms);
void alarm_buzzer_set_muted(bool muted, uint32_t now_ms);
bool alarm_buzzer_is_muted(void);
void alarm_buzzer_force_off(void);

#endif
