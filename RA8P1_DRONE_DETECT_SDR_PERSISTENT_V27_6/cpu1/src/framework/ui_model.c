#include "ui_model.h"

#include <string.h>

static ra8p1_system_telemetry_t g_ui_model;
static ui_center_model_t g_center_models[RA8P1_CENTER_COUNT];
static uint32_t g_center_valid_mask;

static uint64_t ui_frame_center_hz(const ra8p1_display_frame_t *frame)
{
    return ((uint64_t)frame->analysis.center_frequency_high << 32U) |
           frame->analysis.center_frequency_low;
}

static uint32_t ui_center_index_from_hz(uint64_t center_hz)
{
    static const uint64_t centers[RA8P1_CENTER_COUNT] = {
        RA8P1_CENTER_2420_HZ,
        RA8P1_CENTER_2464_HZ,
        RA8P1_CENTER_5760_HZ,
        RA8P1_CENTER_5816_HZ
    };
    for (uint32_t index = 0U; index < RA8P1_CENTER_COUNT; ++index)
    {
        if (center_hz == centers[index])
        {
            return index;
        }
    }
    return UINT32_MAX;
}

void ui_model_init(void)
{
    memset(&g_ui_model, 0, sizeof(g_ui_model));
    g_ui_model.magic = RA8P1_SYSTEM_PROTOCOL_MAGIC;
    g_ui_model.version = RA8P1_SYSTEM_PROTOCOL_VERSION;
    g_ui_model.size = (uint16_t) sizeof(g_ui_model);
    g_ui_model.pipeline_state = RA8P1_PIPELINE_WAITING_FOR_IQ;
    memset(g_center_models, 0, sizeof(g_center_models));
    g_center_valid_mask = 0U;
}

void ui_model_update_frame(const ra8p1_display_frame_t *frame)
{
    uint32_t index;
    ui_center_model_t *model;

    if ((frame == NULL) ||
        (frame->magic != RA8P1_DISPLAY_STREAM_MAGIC) ||
        (frame->version != RA8P1_DISPLAY_STREAM_VERSION) ||
        (frame->size != sizeof(*frame)))
    {
        return;
    }

    index = frame->analysis.center_index;
    if (index >= RA8P1_CENTER_COUNT)
    {
        index = ui_center_index_from_hz(ui_frame_center_hz(frame));
    }
    if (index >= RA8P1_CENTER_COUNT)
    {
        return;
    }

    model = &g_center_models[index];
    if (model->valid && (model->session_id == frame->session_id) &&
        ((int32_t)(frame->sequence - model->sequence) <= 0))
    {
        return;
    }
    model->valid = true;
    model->center_index = (uint8_t)index;
    model->tile_index = frame->analysis.tile_index;
    model->tile_count = frame->analysis.tile_count;
    model->session_id = frame->session_id;
    model->sequence = frame->sequence;
    model->center_frequency_hz = ui_frame_center_hz(frame);
    memcpy(model->presence_q15, frame->analysis.presence_q15,
           sizeof(model->presence_q15));
    model->model_flags = frame->analysis.model_flags;
    model->frame = *frame;
    g_center_valid_mask |= (1UL << index);
}

void ui_model_update(const ra8p1_system_telemetry_t *telemetry)
{
    if ((telemetry != NULL) &&
        (telemetry->magic == RA8P1_SYSTEM_PROTOCOL_MAGIC) &&
        (telemetry->version == RA8P1_SYSTEM_PROTOCOL_VERSION) &&
        (telemetry->size == sizeof(*telemetry)))
    {
        g_ui_model = *telemetry;
    }
}

const ra8p1_system_telemetry_t *ui_model_get(void)
{
    return &g_ui_model;
}

const ui_center_model_t *ui_model_get_center(uint32_t center_index)
{
    return (center_index < RA8P1_CENTER_COUNT) ? &g_center_models[center_index] : NULL;
}

uint32_t ui_model_center_valid_mask(void)
{
    return g_center_valid_mask;
}

uint32_t ui_model_presence_q15(uint32_t class_index)
{
    uint32_t best = 0U;
    if (class_index >= RA8P1_CENTER_COUNT)
    {
        return 0U;
    }
    for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
    {
        if (g_center_models[center].valid &&
            (g_center_models[center].presence_q15[class_index] > best))
        {
            best = g_center_models[center].presence_q15[class_index];
        }
    }
    return best;
}

uint32_t ui_model_flags(void)
{
    uint32_t flags = g_ui_model.model_flags;
    for (uint32_t center = 0U; center < RA8P1_CENTER_COUNT; ++center)
    {
        if (g_center_models[center].valid)
        {
            flags |= g_center_models[center].model_flags;
        }
    }
    return flags;
}
