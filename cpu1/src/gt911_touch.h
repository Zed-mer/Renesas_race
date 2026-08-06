#ifndef GT911_TOUCH_H
#define GT911_TOUCH_H

#include "hal_data.h"

#define GT911_MAX_TOUCHES (5U)

typedef struct st_gt911_point
{
    uint16_t x;
    uint16_t y;
    uint16_t size;
    uint8_t  track_id;
} gt911_point_t;

typedef struct st_gt911_sample
{
    bool           updated;
    uint8_t        count;
    gt911_point_t  points[GT911_MAX_TOUCHES];
} gt911_sample_t;

typedef struct st_gt911_diag
{
    uint32_t magic;
    uint32_t initialized;
    uint32_t i2c_address;
    uint32_t init_error;
    uint32_t last_error;
    uint32_t last_i2c_event;
    uint32_t i2c_transfers;
    uint32_t i2c_errors;
    uint32_t resets;
    uint32_t product_id;
    uint32_t firmware_version;
    uint32_t config_x_max;
    uint32_t config_y_max;
    uint32_t polls;
    uint32_t ready_frames;
    uint32_t touch_frames;
    uint32_t last_status;
    uint32_t last_touch_count;
    uint32_t last_track_id;
    uint32_t last_x;
    uint32_t last_y;
    uint32_t last_size;
} gt911_diag_t;

extern volatile gt911_diag_t g_gt911_diag;

fsp_err_t gt911_touch_init(void);
fsp_err_t gt911_touch_poll(gt911_sample_t * p_sample);
void touch_i2c_callback(i2c_master_callback_args_t * p_args);

#endif
