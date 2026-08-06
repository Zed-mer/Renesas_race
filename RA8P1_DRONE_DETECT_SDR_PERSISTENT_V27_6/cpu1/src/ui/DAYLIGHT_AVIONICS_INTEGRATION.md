# 白昼航电 LVGL UI 对接与实际替换说明

## 1. 适用范围

本文对应 7 寸 1024 x 600 屏幕上的“白昼航电”界面，板端实现位于：

- `cpu1/src/ui/rf_ui.c`
- `cpu1/src/ui/rf_ui.h`
- `cpu1/src/ui/rf_ui_fonts.h`
- `cpu1/src/ui/rf_ui_font_zh_14.c`
- `cpu1/src/ui/rf_ui_glyphs.txt`
- `cpu1/src/lv_conf.h`

本次集成以 CPU1 的 LVGL 表现层为主，并对显示流做了一个受控的 v4 版本升级：

- CPU0 SDR 采集、频点调谐、FIFO 和 DMA。
- FFT/STFT/NPU 分析流程和采集节拍。
- 共享内存槽位、偏移和 512B 大小保持不变；检测框字段语义升级为物理 RF 坐标。
- `display_app` 的 STOP -> STOPPED -> START 频点切换状态机。
- `lvgl_app -> rf_ui` 增加 `rf_ui_update_rf_boxes()`，其余数据与控制接口保持兼容。
- GLCDC、MIPI-DSI、触控和背光初始化流程。

`rf_ui.c` 不读取、清空或重置 SDR 硬件 FIFO。界面的“保持”和历史回放只操作
CPU1 SDRAM 中的瀑布图历史，数据接收仍继续进行。

## 2. 数据链路

```text
SDR / DMA / FIFO
        |
        v
CPU0 采集与分析
        |
        v
shared/display_stream.h + IPC tile/frame
        |
        v
CPU1 framework/ipc_bridge
        |
        v
display_app -> ui_model
        |
        v
lvgl_app -> rf_ui -> LVGL -> GLCDC/MIPI 屏幕
```

界面层只消费已经形成的频谱、瀑布行、通道指标和识别结果。切换频点时，UI 只调用
`display_app_request_focus()` 或 `display_app_request_scan()`，真正的停采、应答和重启由
现有后端完成。

## 3. 固定布局

所有坐标均以屏幕左上角为原点，单位为像素。

| 区域 | x | y | w | h | 说明 |
|---|---:|---:|---:|---:|---|
| 顶栏 | 0 | 0 | 1024 | 54 | 标题、数据源、扫描/锁定、保持、对比 |
| 四目标条 | 0 | 54 | 1024 | 58 | 4 x 256，型号和频段解耦 |
| 瀑布面板 | 0 | 112 | 864 | 324 | 主视觉区域 |
| 瀑布 RGB565 | 32 | 148 | 800 | 264 | 192 频率 bin x 160 RF 时间列 |
| 频谱面板 | 0 | 436 | 864 | 92 | 256 点原生频谱 |
| 频谱 RGB565 | 32 | 470 | 800 | 40 | 400 x 40 纹理横向 2 倍显示 |
| 四通道栏 | 0 | 528 | 500 | 48 | 4 x 125，显示中心频率和占用 |
| 历史栏 | 500 | 528 | 364 | 48 | 前后移动、保持/实时、滑块 |
| 目标详情 | 864 | 112 | 160 | 464 | 当前目标、信道指标和数据状态 |
| 状态栏 | 0 | 576 | 1024 | 24 | 扫描率、FPS、渲染和下溢诊断 |

所有主要点击区域不小于 44 x 44。瀑布图保留横向拖动手势，其他区域不叠加拖动手势。

## 4. 后端到 UI 的函数对接

这些函数均由 LVGL owner 上下文调用，不应从其他任务直接操作 LVGL 对象。

| `rf_ui` 函数 | 输入来源 | 板端行为 |
|---|---|---|
| `rf_ui_create()` | `lvgl_app` 初始化 | 在 `lv_screen_active()` 创建完整对象树 |
| `rf_ui_set_external_spectrum_mode()` | 实际 IPC 数据状态 | 切换 IQ/演示数据标识，不改变采集 |
| `rf_ui_update_spectrum()` | `ra8p1_display_frame_t.spectrum` | 复制一个通道的原生 256 bin 数据 |
| `rf_ui_present_spectrum()` | LVGL 呈现节拍 | 只对当前通道脏数据重绘并 invalidate |
| `rf_ui_update_waterfall_rows()` | display tile | 每个真实 RF 行追加一个时间列 |
| `rf_ui_append_waterfall_gap_columns()` | tile 序列缺口 | 插入“未知数据”列，不伪装成零功率 |
| `rf_ui_present_waterfall()` | LVGL 呈现节拍 | 提交当前通道最新 ring head |
| `rf_ui_update_channel_metrics()` | `lvgl_app` 指标计算 | 更新峰值、底噪、占用和数据龄期 |
| `rf_ui_update_detection()` | 四频 round fusion 状态 | 更新工作/疑似/未工作、融合强度和最后阳性通道 |
| `rf_ui_update_rf_boxes()` | V20 物理频偏/带宽/时间边界 | 在频谱和瀑布图上绘制与真实 RF 窗口对齐的框 |
| `rf_ui_mark_channel_result()` | 每个频点完成结果 | 更新对应通道的活动反馈 |
| `rf_ui_set_scan_rate_x10()` | 推理/窗口速率 | 更新扫描率文本 |
| `rf_ui_set_model_placeholder()` | 模型有效性 | 标记当前是否为占位输出 |
| `rf_ui_set_focus_mode()` | 后端模式应答 | 同步“全频扫描/重点锁定”状态 |
| `rf_ui_set_render_metrics()` | GLCDC/LVGL 诊断 | 更新面板频率、FPS、最大渲染时间和 UF |

`rf_ui.h` 是当前表现层契约。后续只优化视觉时，不要改变结构体字段、函数参数或调用线程。

## 5. UI 事件到后端命令

| 用户操作 | UI 行为 | 后端调用 |
|---|---|---|
| 点击通道卡 | 切换当前瀑布/频谱 | 锁定模式下调用 `display_app_request_focus(channel)` |
| 点击在线目标卡 | 根据该目标的 `channel_index` 联动通道 | 跨通道且处于锁定模式时调用 `display_app_request_focus(channel)` |
| 点击“全频扫描” | 请求恢复四频点扫描 | `display_app_request_scan()` |
| 点击“重点锁定” | 请求锁定当前通道 | `display_app_request_focus(selected_channel)` |
| 点击“实时/恢复” | 冻结或恢复本地视图 | 无采集命令，CPU1 继续接收数据 |
| 拖动瀑布图 | 自动保持并浏览历史 | 无采集命令 |
| 点击历史左右键/滑块 | 浏览 256 列保留历史 | 无采集命令 |
| 点击“对比” | 打开四目标全屏对比层 | 无采集命令 |

频点切换的时序仍由 `display_app` 管理：先发 STOP，等待匹配的 STOPPED，再发新的 START。
UI 不自行擦除 FIFO，也不绕过该门控。

## 6. 多无人机和任意频段

四个目标槽代表四个识别类别，不再和 CH1 到 CH4 固定绑定。

`lvgl_app::ui_flow_update_detections()` 只读取 `rf_v25_activity_service` 的完整有效四频
round fusion 结果。`WORKING / UNCERTAIN / NO_RF_OBSERVED` 分别映射为工作、疑似和未工作；
`channel_index` 使用融合状态记录的最后阳性频点。因此：

- 任意型号都可以出现在任意频点。
- 多个型号可以同时指向同一通道。
- 通道卡上的目标数量按动态 `channel_index` 统计。
- 点击目标时，频谱和瀑布图切到该目标当前所在通道。

显示流 v4 的 `ra8p1_detection_box_t` 使用 8 字节原位版本化，不增加共享 SRAM：

- `frequency_start_q8 / frequency_span_q8`：在可靠 56 MHz 分析带宽内的半开区间。
- `time_start_q8 / time_span_q8`：在当前 `window_sample_count` 内的半开区间。
- `analysis.window_sequence`：关联同一窗口的 16 个瀑布时间列。
- `metadata`：原检测类别和 `RF_GEOMETRY_VALID`、裁切、20 MHz 等有效位。

CPU1 只有在匹配窗口的第 16 列已经进入历史环后才提交框，因此实时滚动、保持和拖动回放
都使用同一 RF 时间基准，不会把模型特征坐标伪装成射频位置。

## 7. 中文字体

`rf_ui_font_zh_14.c` 是从 `C:\Windows\Fonts\simhei.ttf` 生成的 14 px、4 bpp
SimHei 汉字子集。较高的灰度位深让黑色界面上的中文笔画更清晰；ASCII、数字和 LVGL
图标回退到 `lv_font_montserrat_14`。不要启用完整 CJK
字体，当前 CPU1 Flash 预算不允许额外增加约百 KiB 的全量字库。

修改界面中文后：

1. 把新增汉字加入 `rf_ui_glyphs.txt`。
2. 在 Solution 根目录运行：

```powershell
$rfGlyphs = (Get-Content -Raw 'cpu1/src/ui/rf_ui_glyphs.txt').Trim()
npx --yes lv_font_conv `
  --font 'C:\Windows\Fonts\simhei.ttf' `
  --symbols $rfGlyphs `
  --size 14 --bpp 4 --format lvgl --no-compress --no-kerning `
  --lv-font-name rf_ui_font_zh_14 `
  --lv-include lvgl.h `
  --lv-fallback lv_font_montserrat_14 `
  -o 'cpu1/src/ui/rf_ui_font_zh_14.c'
```

3. 运行 `python tools/test_rf_ui_waterfall_layout.py`，它会检查所有 UI 字符串是否被子集覆盖。
4. 重新执行 CPU1 构建并检查 Flash。

SimHei 来自构建机的 Windows 字体目录；发布生成字库前需按目标产品的字体授权审查。

## 8. 资源预算

2026-07-31 的 CPU1 Debug 无烧录构建结果：

| 项目 | 使用 | 分区/上限 | 余量 |
|---|---:|---:|---:|
| Flash (`text + data`) | 431,344 B | 524,288 B | 92,944 B |
| CPU1 SDRAM 链接范围 | 4,222,464 B | 5,242,880 B | 1,020,416 B |
| `rf_ui.o` `.sdram_noinit` | 1,764,416 B | 包含在 CPU1 SDRAM | - |
| 中文字体只读数据 | 6,860 B | 包含在 Flash | - |

主要 UI SDRAM 对象：

- 四通道瀑布历史 ring：`0xC0000`。
- 保持快照：`0x18000`。
- 800 x 264 双映射渲染 ring：`0xCEA40`。
- 400 x 40、416 stride 频谱纹理：`0x8200`。

每次增加图片、字体或更大纹理后，都必须重新检查 MAP，不能只看源文件大小。

## 9. 实际替换步骤

1. 保留当前可工作的 CPU0 采集、显示驱动和频点切换基线。
2. 只迁移 `cpu1/src/ui/` 内本文件第 1 节列出的 UI 文件。
3. 保留工程实际使用的 `cpu1/src/lv_conf.h`，并启用包内字体所需的 Montserrat 12/14/16/20。
4. 不修改 `ra/`、`ra_cfg/`、`ra_gen/` 中的生成文件。
5. 确认 e2 studio 生成的 `Debug/src/ui/subdir.mk` 包含 `rf_ui_font_zh_14.c`。
6. 核对显示流版本为 v4，并验证 frame/slot 仍为 504/512B、CPU0/CPU1 DWARF 布局一致。
7. 先运行静态测试，再做 CPU1/双核无烧录构建。
8. 检查 ELF/MAP 时间戳、构建日志中的 `0 errors`、Flash 和 SDRAM 余量。
9. 获得明确授权后才能烧录；烧录前重新确认目标板和 J-Link 序列号。

## 10. 验证清单

Solution 根目录执行：

```powershell
python tools/test_rf_ui_waterfall_layout.py
python tools/test_cpu1_campaign_control.py
python tools/test_analysis_partial_tile_schedule.py
python ui-workbench/tools/validate_workbench.py
```

板上验证应覆盖：

- 1024 x 600 无裁切、文字不越界、瀑布为主视觉。
- 四种目标在四个频点任意组合，且同通道可以显示多个目标。
- 全频扫描和重点锁定命令有明确状态反馈。
- 保持、恢复、拖动和滑块不会停止数据接收。
- 连续切换频点后不显示上一频点的伪新数据。
- GLCDC underflow 不增加，触控无明显卡顿。
- 复位多次后界面稳定进入实时状态。

本次交付只完成无烧录构建验证，未连接、选择或刷新探头。
