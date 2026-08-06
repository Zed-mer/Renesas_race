#ifndef JD9165_PANEL_H
#define JD9165_PANEL_H

#include "hal_data.h"

fsp_err_t jd9165_panel_configure(void);
fsp_err_t jd9165_panel_read_power_mode(void);
fsp_err_t jd9165_panel_read_dsi_error_count(uint8_t * p_error_count);
fsp_err_t jd9165_panel_read_lane_config(uint8_t * p_lane_config, uint8_t * p_lane_control);
fsp_err_t jd9165_panel_disable_bist(void);
fsp_err_t jd9165_panel_enable_bist(void);

#endif
