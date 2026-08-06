# CPU1 UI workspace

This directory is the single project-owned workspace for the 1024 x 600 LVGL
interface. Hardware initialization and transport protocols intentionally stay
outside this directory.

## Files

- `rf_ui.c/.h`: screen layout, coordinates, styles, widgets, and touch events.
- `rf_ui_fonts.h`: UI font mapping.
- `rf_ui_font_zh_14.c` and `rf_ui_glyphs.txt`: Noto Sans SC 14 px Chinese
  subset and its reproducible glyph inventory.
- `rf_demo_data.c/.h`: fixed channel/class metadata and empty startup values;
  live RF samples still come from CPU0.
- `../lvgl_app.c/.h`: LVGL lifecycle, display flush, tile-to-widget binding,
  four-frequency fusion binding, and render telemetry.
- `../framework/display_app.c/.h`: CPU1 controller, IPC polling, presentation
  ACKs, activity-service polling, and commands initiated by the UI.
- `../framework/ui_model.c/.h`: CPU1-owned display/view model.
- `../lv_conf.h`: project-owned LVGL build configuration.
- `DAYLIGHT_AVIONICS_INTEGRATION.md`: exact layout, backend mapping, resource
  budget, replacement procedure, and board validation checklist.
- `OFL-1.1.txt`: license for the converted Noto Sans SC glyph subset.
- `../../../ui-workbench/`: browser-only design workspace outside all firmware
  build roots.

The compiler-facing `../lv_conf.h` remains the editable project configuration
because this V20 integration keeps the existing source ownership boundaries.

## Boundary

```text
framework/ipc_bridge
        |
        v
display_app +----> ui_model
            |
            +----> lvgl_app ----> rf_ui ----> LVGL
                         |                         |
                         +----> display_bringup / GLCDC / panel
```

Keep these modules outside `ui/`:

- `display_bringup.*`, `jd9165_panel.*`, `gt911_touch.*`: hardware drivers.
- `framework/ipc_bridge.*`: cross-core transport.
- `shared/` and `framework/*_protocol.h`: cross-core ABI.

For visual changes, work only in `../../../ui-workbench/` first. Follow its
migration guide before implementing an approved design in `rf_ui.c`. Do not
change transport cadence, frame-presentation acknowledgement, or RF-time
semantics as part of styling.

The approved board design is the 1024 x 600 “白昼航电” layout. Read
`DAYLIGHT_AVIONICS_INTEGRATION.md` before replacing it in another firmware
snapshot. UI pause/history controls never clear or stop the SDR FIFO.

Run the focused checks from the solution root:

```powershell
python tools/test_rf_ui_waterfall_layout.py
python tools/test_cpu1_campaign_control.py
python tools/test_analysis_partial_tile_schedule.py
```
