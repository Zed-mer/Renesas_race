#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include "display_stream.h"
#include "system_protocol.h"

typedef struct st_ui_center_model
{
    bool valid;
    uint8_t center_index;
    uint8_t tile_index;
    uint8_t tile_count;
    uint32_t session_id;
    uint32_t sequence;
    uint64_t center_frequency_hz;
    uint16_t presence_q15[RA8P1_CENTER_COUNT];
    uint32_t model_flags;
    ra8p1_display_frame_t frame;
} ui_center_model_t;

void ui_model_init(void);
void ui_model_update(const ra8p1_system_telemetry_t *telemetry);
void ui_model_update_frame(const ra8p1_display_frame_t *frame);
const ra8p1_system_telemetry_t *ui_model_get(void);
const ui_center_model_t *ui_model_get_center(uint32_t center_index);
uint32_t ui_model_center_valid_mask(void);
uint32_t ui_model_presence_q15(uint32_t class_index);
uint32_t ui_model_flags(void);

#endif
