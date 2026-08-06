/* Project-owned LVGL configuration overrides.
 * The FSP-generated include path searches src/ before the generated LVGL
 * configuration, so this file remains stable across content generation.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH             16
#define LV_USE_OS                  LV_OS_NONE
#define LV_MEM_SIZE                (256U * 1024U)
#define LV_USE_DRAW_DAVE2D         1
#define LV_USE_RENESAS_GLCDC       0

/* The deployed UI and both display layers are RGB565.  Keep the software
 * fallback for RGB565 and glyph alpha masks, but do not link format mixers
 * which cannot be reached by this firmware. */
#define LV_DRAW_SW_SUPPORT_RGB565                  1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED          0
#define LV_DRAW_SW_SUPPORT_RGB565A8                0
#define LV_DRAW_SW_SUPPORT_RGB888                  0
#define LV_DRAW_SW_SUPPORT_XRGB8888                0
#define LV_DRAW_SW_SUPPORT_ARGB8888                0
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED  0
#define LV_DRAW_SW_SUPPORT_L8                      0
#define LV_DRAW_SW_SUPPORT_AL88                    0
#define LV_DRAW_SW_SUPPORT_A8                      1
#define LV_DRAW_SW_SUPPORT_I1                      0

#define LV_FONT_MONTSERRAT_12      1
#define LV_FONT_MONTSERRAT_14      1
#define LV_FONT_MONTSERRAT_16      1
#define LV_FONT_MONTSERRAT_20      1
#define LV_FONT_MONTSERRAT_24      0
#define LV_FONT_DEFAULT            &lv_font_montserrat_20

/* Only the widgets referenced by the live RF UI and the compiled legacy
 * fallback are enabled.  Every visible object is styled explicitly. */
#define LV_USE_ANIMIMG             0
#define LV_USE_ARC                 0
#define LV_USE_BAR                 1
#define LV_USE_BUTTON              1
#define LV_USE_BUTTONMATRIX        0
#define LV_USE_CALENDAR            0
#define LV_USE_CANVAS              1
#define LV_USE_CHART               0
#define LV_USE_CHECKBOX            0
#define LV_USE_DROPDOWN            0
#define LV_USE_IMAGE               1
#define LV_USE_IMAGEBUTTON         0
#define LV_USE_KEYBOARD            0
#define LV_USE_LABEL               1
#define LV_USE_LED                 0
#define LV_USE_LINE                0
#define LV_USE_LIST                0
#define LV_USE_MENU                0
#define LV_USE_MSGBOX              0
#define LV_USE_ROLLER              0
#define LV_USE_SCALE               0
#define LV_USE_SLIDER              1
#define LV_USE_SPAN                0
#define LV_USE_SPINBOX             0
#define LV_USE_SPINNER             0
#define LV_USE_SWITCH              0
#define LV_USE_TABLE               0
#define LV_USE_TABVIEW             0
#define LV_USE_TEXTAREA            0
#define LV_USE_TILEVIEW            0
#define LV_USE_WIN                 0

#define LV_USE_THEME_DEFAULT       0
#define LV_USE_THEME_SIMPLE        0
#define LV_USE_THEME_MONO          0
#define LV_USE_FLEX                0
#define LV_USE_GRID                0

#define LV_USE_LOG                 0
#define LV_BUILD_EXAMPLES          0
#define LV_BUILD_DEMOS             0

#endif
