# Integration provenance

## 2026-07-29 tune-guard SDR -> RA8P1 end-to-end acceptance

本轮从基线 `fb4bc0b54c41` 集成 SDR tune guard，并使用 FSP 6.4.0 的官方
`Debug_Multicore` 路径成套构建、下载 CPU0/CPU1。跨核 ABI、Solution 内存分区、
CPU0 MVE 和 CPU1 D/AVE2D 校验均通过。实际烧录与运行制品为：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 ELF | `80A17C2A8DC0B90B9305AFFC066B83DE0CE2FBF0E5BC95501F53D46F0BFAC57E` | 3,604,572 |
| CPU0 MAP | `3B5B195B30EDE5B3F60D4D23062F02581122E57B4B0356077E1753306B5C107B` | 2,459,816 |
| CPU1 ELF | `921635F71E1E799723063E542F0B483C7637EC381DF5653E3C34C3E8EDA7C4A3` | 4,996,780 |
| CPU1 MAP | `B09B29B2469E850CDD1CC205D14BEE992D0CB536E5245F348CED2EEAADC308A9` | 1,263,597 |
| SDR capture agent | `D67E6D229FBE7624E24DD0E587AF7336333C0388BF4A89143C6F3BC6E9DA7524` | 48,884 |
| SDR mmap adapter | `B68F277DF9BE295EEFE2906220787F124E2900920A05D5FFB8D54D27ADEE7C0F` | 22,648 |

SDR 端运行 `/tmp/sdr_capture_agent_tuneguard` 与
`/tmp/sdr_adapter_iio_mmap_tuneguard.so`。5.49 s 内完成 50 个真实采集窗口，payload
为 370-393 Mbps；retry、timeout、gap、reorder、IQ invalid、CRC error 和 ring
drop 增量均为 0。CPU1 presented 速率为 `28.497 FPS`，用户完成现场验收。

摘要保存在 `evidence/tune_guard_e2e_20260729/`。本次实时 trace 未作为原始大文件
纳入仓库；当前 SDR 程序只在 `/tmp` 运行，尚未持久化到 `/mnt/jffs2`。NPU 权重仍为
placeholder，因此本次验收只覆盖真实 IQ 采集、传输、STFT/显示链路和运行稳定性，
不构成分类准确率验收。

## 2026-07-27 B2 / genuine 256-bin spectrum hardware run

当前板上版本使用 B2 单屏布局。瀑布仍为 192 个独立频率估计、160 个可视 RF
时间列，每列 `0.614933 ms`，可视范围 `98.389 ms`；CPU1 保留 256 列历史，
暂停后可拖动回看并用 LIVE 返回最新数据。频谱共享 ABI 升级为 version 3：
`spectrum[1][256]`、frame `504 B`、slot `512 B`。CPU0 将 955 个有效 FFT raw bin
完整分为 `187 x 4 + 69 x 3` 个互不重叠的显示组，并平均最终 9 个 STFT frame；
这 256 点不是从模型的 128 点插值，平均显示频率跨度约 `218.75 kHz/bin`。

官方 DDSC 洁净双核构建、`verify-solution.ps1 -SkipBuild`、Debug_Multicore 下载和
板端运行均已通过：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 ELF | `21C0FA04C118BCA6089D5EFB6F41EC634D3F0F9A8457668ADD5D2C973A37D047` | 3,632,240 |
| CPU1 ELF | `BF94E1D6BC9A1C2EF4808D08E6038B42E8DF8BD06F66F8E79E3EE7C726E29017` | 5,021,868 |

共享头摘要为
`798D8E7C00F0080EE1FF83FBA4BAC1B0DF595EF7BE52D00BD88B8903D055F908`；
CPU0/CPU1 的 15 个 DWARF 布局摘要均为
`B623D745A55FF205ADB6240596E76D84B3DFBE50DF865C41977DFDBBE606725E`。
跨核校验明确得到 display stream v3、peak 2、spectrum `1 x 256` 以及
offset `48/304/308`。Solution 分区、90 个运行时分区宏、CPU1 D/AVE2D 与软件
fallback、100 条 CPU0 MVE 指令和全部必需符号通过。CPU0 普通 RAM 到 shared
边界余 `42,132 B`；CPU1 SDRAM 使用 `4,278,784 / 5,242,880 B`，余 `964,096 B`。
Python 回归为 `131/131`。

探针 `1082495494`、VTref `3.3 V`。板端状态为 `stage/error/running=6/0/1`、
GLCDC underflow `0`；15.129 s 内 presented counter 增加 423，实测
`27.96 FPS`。无抓屏干扰的快照中 presented/content 为 `27.698..28.626 Hz`，
window/inference 为 `8.45..8.75 Hz`。累计 11,768 个 192-bin 瀑布列时
generated/consumed 一致，tile drop 和 IPC miss 均为 0。读取两块共 2.4 MB
framebuffer 后累计出现 1 次 drop/miss；随后无调试器区间继续处理 7,733 列且
计数不再增长，因此该单次事件记录为 SWD 抓屏干扰，不作为脱机链路故障。

`evidence/b2_256bin_20260727/` 保存两块 1024 x 600 RGB565/PNG。画面可见 B2、
真实瀑布和频谱、`-98 ms..NOW` 时间轴、频率纵轴及 `SP 256 | WF 192 x 160`，
没有启动彩条。两缓冲有 215,418 个像素不同，证明捕获期间内容正在更新；live
SaveBin 可能跨刷新读取，因此不作为无撕裂证明。

SDR 端 `/mnt/jffs2/autorun.sh` 已由冷启动验证，启动链为
`rcS -> /etc/init.d/S98autostart -> autorun.sh`。持久化 agent 自动配置
`192.168.31.10/24`、目标 `192.168.31.20` 并监听 UDP/5004；本轮运行时 RA 已持续
收到真实 frame/tile。诊断 trace 的 831 个完整窗口中仅一次首传 gap/CRC，随后同
request 自动重传成功；恢复后的独立 5.51 s / 53 窗口区间错误增量为 0。

## 2026-07-27 ABI v7 / 192-bin / pause-review host-built candidate

本候选版本把瀑布共享 tile 从 ABI v6 / 128-bin 升级为 ABI v7 / 192-bin。
FFT1024 在 60 MS/s 下的原始频率间隔为 `58.59375 kHz`；56 MHz 有效带宽内的
955 个有效 raw bin 被完整且不重复地分为 192 个独立显示组，其中 187 组包含
5 个 raw bin，5 组包含 4 个 raw bin。平均显示频率跨度约为
`291.67 kHz/格`。固定 NPU 输入仍为 `128 x 128 x 3`，完整频谱曲线仍为真实的
128-bin STFT 结果；本次 192-bin 升级只作用于渐进瀑布支路，不能描述为模型或
完整频谱 ABI 已升为 192 点。

瀑布可视区保存 160 个真实 RF 时间列，每列由采样合同推导为
`590336 / 16 / 60 MHz = 0.614933 ms`，因此可视范围为 `98.389 ms`。CPU1 为每个
中心保留 256 列、即 `157.423 ms` 的 live 历史。暂停时冻结当前中心的频谱和
256 列瀑布快照，但 CPU0、IPC 和 live history 继续推进；横向拖动可在暂停快照内
向前回看最多 96 列、即 `59.034 ms`，恢复 LIVE 后立即重建最新 160 列。拖动重建
以 33 ms 为最短提交周期，松手或 PRESS_LOST 强制提交最终位置。该交互逻辑已通过
主机回归，触摸手感、拖动 FPS、SDRAM 带宽和 GLCDC underflow 仍需上板验证。

官方 DDSC 双核构建与 `verify-solution.ps1 -SkipBuild` 已通过：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 ELF | `8E88F2652DD68308E938BB55E633156F346DC831B63283435A7CBACB82CC2A29` | 3,616,544 |
| CPU1 ELF | `EF3AE72C198D36CBA5B73227120250F6EA298319B88D4852234828BE719CF010` | 5,013,136 |

共享 ABI 摘要为
`99DDF3BB838C23E5A57C4C34858BEBD60A2C767779B95750E2F706DD1CEAEC67`，
跨核 ABI、Solution 分区、90 个运行时分区宏、CPU1 D/AVE2D 与软件 fallback、
96 条 CPU0 MVE 指令和必需符号均通过。CPU0 为 text/data/bss
`266856/2140/7472280 B`，CPU1 为 `419952/392/4932393 B`；CPU1 SDRAM 静态
使用约 `4,627,200 / 5,242,880 B`，余量约 `615,680 B`，不再增加大型画布缓存。
Python 回归为 `125/125`；另有随机热路径等价测试证明 80 个 FFT 帧的 192-bin
显示功率和与标量参考逐项一致，原 128-bin 模型 reducer 继续 bit-exact。

本候选版本尚未烧录。2026-07-27 环境复查显示配置探针 `1088229345` 不在线，
Commander 未匹配探针、RA8P1 设备或内核；配置串口 `COM11` 也不存在，仅发现
`COM7`、`COM9`。因此上述结果仅证明主机构建和静态合同，不能作为 ABI v7 的板端
FPS、row rate、故障寄存器或拖动性能证据。下方 ABI v6 数据仍是当前最新可信硬件
测量；恢复已验证探针后必须通过官方 `Debug_Multicore` 成套烧录再重新采样。

## 2026-07-26 50 ms ABI v6 / 128-bin hardware experiment

本轮将渐进瀑布传输从 ABI v5 / 64-bin 升级为 ABI v6 / 128-bin，并通过官方
`Debug_Multicore` 成套构建、下载和运行：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 ELF | `71659253BE3D8A170E5D7055C36FAE26E4CCED26003AA16416264EB287203651` | 3,615,728 |
| CPU1 ELF | `AD4D51DFE11CE46EBDE2BDCA5B68A9113D37E36541590A75A40A3A0F46B257AE` | 5,002,868 |

共享 ABI 摘要为
`F68586835726361BFFC9DE60EBE471AC479E6F72FC456AB855127F6185582ADF`；
CPU0/CPU1 的 15 个完整 DWARF 布局摘要均为
`3BB40799CC34229EE6A73B599855003059507CA1E11D848A5A3665EA9103D955`。
`verify-solution.ps1`、跨核 ABI 校验、runtime sampler 自测和 Python 全量回归
`123/123` 均通过。CPU0 `g_display_tile` 已移出 DTCM，链接为普通 RAM 中的
`0x800 B` 缓冲；`CPU0RamUsedEnd=0x220D656C`，到 shared RAM 仍余 47,764 B。

tile 共享区地址和总量保持不变，布局为 `16 x 256 B`，每槽 payload 248 B，
携带一个完整 128-byte 频率 row。56 MHz 有效掩码提供约 120 个独立 pooled
频率估计；其中 8 个估计各占两个相邻显示格，以铺满 128 格。该路径不做幅度
插值，因此应表述为“128 个传输/显示 bin、约 120 个独立真实频率估计”，不能
声称 128 个独立估计。

板端读取确认 `Tile.Valid=true`、`Width=128`、`RowBytes=128`、
`stage/running/last_error=6/1/0`。稳定样本的 presented/content 为
`23.346..23.369 Hz`，window/inference 为 `7.72..7.78 Hz`，tile 为
`125.0..129.41 Hz`；最终累计消费 119,812 个 tile，waterfall drop、IPC tile miss、
GLCDC underflow 均为 0。与 64-bin 基线 `23.392/7.87/124.51 Hz` 相比，吞吐未见
实质回退。

同版 CPU0 trace 的 128 个完整窗口给出 request→首 payload p50/p95
`126.59/128.89 ms`、首→末 payload `75.19/84.19 ms`、payload
`229.81/278.85 Mbps`、末 payload→CRC `15.49/27.79 ms`。ring HWM 仍为
`961/4096`，gap/reorder/invalid/ring drop 全 0。相对 64-bin，request 边界只变化
`+0.11/+0.13 ms`；本轮未观察到 128-bin 对首包延迟的可测退化。

`evidence/waterfall50ms_128bin_abi6_20260726/` 保存双 framebuffer BIN/PNG。
两帧有 290,255 个像素不同，瀑布区域 95.29% 像素发生变化；同一抓屏方法下，
瀑布纵向像素变化次数均值从 64-bin 的 169.1 增至 128-bin 的 202.1，作为更细
纵向结构的图像启发式证据。该指标受实时输入内容影响，不替代协议维度和独立
频率估计数量的证明。

## 2026-07-26 20 ms FOCUS prefetch historical hardware run

最终镜像由同一 Solution 正式双核构建生成，并使用探针 `1082495494` 通过官方
`Debug_Multicore` 成功下载：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 ELF | `D5F3A31FE7607F2098CC8A98FE2EAD0600C5FE9C12E25DC9102D4974A24348B2` | 3,614,604 |
| CPU1 ELF | `65F4B8C99BC229063A3C9F8AA4896A1DE7B35222BF52552E25A3926C8D68A8EE` | 5,000,984 |

共享 ABI 摘要为
`16E1C0C5EAB6AE453CC4D7E10ADD2CEA7FE8DE372718BCB611AAACE46D4D6A73`。
最终静态验证通过 90 个分区宏、`CPU0RamUsedEnd=0x220D5C64`、96 条 MVE 指令、
D/AVE2D/软件 fallback、16 个 CPU0 和 23 个 CPU1 必需符号以及跨核 DWARF ABI。
Python 全量回归 `118/118`；CPU0 host C 状态机 31 个场景通过并输出
`SDRC client host tests passed`。

最终显示镜像保留 32 列真实 RF 历史，每列 `0.614933 ms`，总计
`19.677867 ms`；5 ms 仅为 waterfall dirty-head 的 submission arming，物理呈现
仍受 46.869 Hz VSync 限制。tile loss 使用显式 unknown，跨 center 无法归属的 loss
会保守失效相关历史。普通无标志 RETRY 和 flagged discontinuity 都由双 lane 逻辑
窗口进度识别；row 回退、window 回绕或无法消歧的 gap 会在追加新 row 前把 20 ms
历史置为 unknown。

CPU0 continuous FOCUS 现在预取同中心的全新 request/session，SCAN 保持下一中心预取。
active/prefetch 身份由 boot epoch、request、session、center 和 frequency 隔离；
READY、credit、promotion、retry、cancel tombstone 和 terminal confirmation 语义不变。
默认仍为 `390 Mbps / batch 24`；受控 390/450/500 Mbps A/B 的 payload 中位数
分别为 `231.439/216.738/232.295 Mbps`，500 相对 390 仅提高约 0.37%，不保留升速。

CPU1 将完整频谱栅格化合并到最多每 100 ms 一次，瀑布像素生成使用 256 项 RGB565
色表和 64-bin 到 128-row LUT。FOCUS 只融合当前中心的频谱、告警和分类，真实 row
仍逐列写入，不插值、不生成伪动画。双 lane、40 MHz、46.869 Hz DSI 基线未修改。

`evidence/waterfall20ms_focus_prefetch_20260726/runtime_final.json` 绑定同一对最终
ELF 哈希，并通过 `R7KA8P1KF_CPU0` 读取到
`stage/running/last_error=6/1/0`、underflow `0`、center mask `0x1`，
presented/content `20.568/18.609 Hz`、window/inference `7.81/7.81 Hz`、tile
`117.65 Hz`；累计 43,190 个 row 全部生成和消费，tile drop、IPC tile miss 均为
0。该文件保存 CPU1 自计时的一秒 gauge，不把 J-Link 主机等待时间误称为显示测量区间。

配套 `network_prefetch_15s.json` 在 15.485 s 内新增 132 个 completed window，
timeout、missing-capture、gap、reorder、invalid、CRC error、ring drop 增量均为 0，
并记录 85 次同中心 prefetch IQSC proof。区间有 1 次 retry；J-Link 两点读取会扰动
实时流，因此网络区间不替代脱离调试器后的显示 gauge。

`trace_focus_current_summary.json` 的 126 个完整窗口全部来自 center 0；payload、
capture、STFT elapsed span 和 NPU 中位数分别为 `208.450 Mbps`、`23.335 ms`、
`89.522 ms` 和 `0.653 ms`。CPU0 load 中位数为 `54.7%`，high-water 最大
244/4096，gap/reorder/invalid/ring drop 均为 0。STFT span 与收包重叠，不能和
payload 时间机械相加。

CPU0 还从 `0x68B00000`、`0x68C2C000` 读取两块 1,228,800 B RGB565 framebuffer。
同证据目录下两张 PNG 可见最终 B 布局、真实瀑布/频谱、20 ms 横轴、频率纵轴且没有
启动彩条。双缓冲分别含 405/419 种颜色并有 281,676 个像素差异，证明捕获期间内容
正在变化；抓取不作为无撕裂证明。J-Link `Go` 返回“CPU is not halted”，因为本次
live SaveBin 没有先停核；随后 `runtime_final.json` 再次证明 CPU0/CPU1 正常推进。

60 MS/s S16 IQ 连续流仍需 `1.92 Gbps`（未计协议开销）。20 ms 是 RF 样本历史，
不是 20 ms 墙钟滚屏周期；长期严格连续墙钟滚动必须使用 SDR 侧派生 FFT/STFT 显示流，
不能用 UI 插值伪造。模型仍是 placeholder，所有性能证据都不构成识别准确率验收。

本 Solution 是从已验证基线复制出的独立集成工程。原始基线目录没有被重命名或删除；本目录及两个带 `20260719` 后缀的子工程是当前开发对象。

## 工程关系

| 区域 | 来源 | 当前副本 |
| --- | --- | --- |
| 显示/触摸 | `D:/Renesas_race/ra8p1_jd9165_dual_lane_touch_gt911_20260718` | `ra8p1_sdr_stft_npu_display_solution_20260719_CPU1` |
| 以太网/RMAC | `D:/Renesas_race/git_renesas/ra8p1_competition_eth_test` | `ra8p1_sdr_stft_npu_display_solution_20260719_CPU0` |
| STFT/NPU 参考 | `D:/Renesas_race/RA8P1_RF_IQ_STFT_NPU_Demo_20260719_641234b/RA8P1_RF_IQ_STFT_NPU_Demo` | CPU0 `src/framework/analysis_pipeline.*`、`npu_model/*` |

## CPU1 集成

- `src/jd9165_panel.c/.h` 保持显示基线。
- `src/gt911_touch.c` 使用已验收的 status-first 轮询：无触摸时只读取状态字节，READY 时才读取触点记录。
- `src/display_bringup.c/.h`、`src/hal_warmstart.c` 保持双 lane、40 MHz JD9165/GLCDC 启动路径；SDRAM 自检后先清黑两个 framebuffer，启动阶段不再显示彩条。
- `src/ui/rf_ui.c/.h`、`src/lvgl_app.c/.h` 和 `src/framework/display_app.c` 实现 B 版 1024 x 600 单屏布局：`38 + 36 + 386 + 108 + 32`，主瀑布有效区 `896 x 326`，次级频谱有效区 `896 x 52`，并保留频段选择、告警和性能底栏。瀑布保存 80 个 pooled RF 时间列，每列 `0.614933 ms`，总 RF 历史 `49.1947 ms`；纵轴为频率且高频在上，横轴从 `-50 ms` 流向 `NOW`。所有真实到达列立即写入双映射环；缺失 tile 在下一条真实 row 前写成与强度 0 明确不同的中性 `unknown/gap` 列，discontinuity 会先把受影响历史置为 unknown，不插值、不生成伪动画。dirty head 最快每 `5 ms` 在 LVGL refresh 前 arming，但物理呈现仍受 `46.869 Hz` VSync（约 `21.336 ms`/帧）限制，不能把 5 ms 解释成面板帧周期。
- 频谱接收只更新 CPU1-owned cache 和 dirty 状态，实际栅格化最多每 100 ms 一次；瀑布热路径使用 256 项 RGB565 色表和 128-bin 到 128-row 一一反向映射。FOCUS 下只融合选中中心，避免历史中心污染频谱、告警和分类。
- CPU1 由项目自有 `src/lv_conf.h` 启用 LVGL D/AVE2D renderer，同时保留软件 fallback 和自定义 GLCDC direct 双缓冲路径。验收要求最终 ELF 同时包含 `lv_draw_dave2d_init`、TES `d2_*` 和 `lv_draw_sw_init`；面板扫描率、LVGL presented FPS 与真实内容更新率必须分别报告。
- UI 提供 `SCAN`/`FOCUS` 切换。SCAN 连续轮询四个中心；FOCUS 连续采集当前选中中心，切换频段时调度随选择迁移。模式切换先 STOP/CANCEL 旧调度，再在匹配的 terminal confirmation 后启动新调度。
- CPU1 不读取原始 IQ；频谱、瀑布、mask 和 RGB565 buffer 均在 CPU1 自有 SDRAM。

## CPU0 集成

- `board/ports/drv_rtl8211.c`、`libraries/HAL_Drivers/drv_eth.h` 与以太网基线保持一致；`drv_eth.c` 保留原 RMAC buffer 释放和有界让步行为。
- `src/eth_iq_fast.c/.h` 在原快速接收路径上增加 S16 IQ header 校验、STREAM_START/END 配置和 ring 投递。
- `src/framework/iq_ring.*` 是 CPU0 SDRAM 4096 槽 SPSC ring；它只属于 CPU0。
- `src/framework/analysis_pipeline.*` 是当前生产 STFT 路径：S16 12-bit 到 Q15、FFT1024/hop512、MVE 功率、`590336` sample（约 `9.839 ms RF`）完整窗口池化和 display frame 发布。完整窗口/NPU 合同不变；每生成一个 pooled row 就另外发布一个 128-bin 瀑布 row，其中约 120 个为独立真实频率估计。首窗或 discontinuity 后发布全部 16 行，50% overlap 的后续连续窗口只发布新增 8 行。
- `src/framework/npu_runner.*` 和 `src/framework/npu_model/*` 合并了 FSP `rm_ethosu`、Ethos-U core driver、Vela command stream、model data 和 DWT timing。CPU0 会复查并恢复被调试器关闭的 DWT，frame 用独立 valid bits 标记 STFT/NPU/E2E timing。模型 payload 是随机权重 placeholder，不能作为识别精度证据。
- `src/framework/rf_pipeline.*` 负责 ring 水位调度、synthetic fallback、stream 配置、telemetry 和一次/窗口 NPU 调度。当前 session 只接受 S16 channel A；START 配置在 pipeline 线程 pending 应用，每个 ring slot 带当前接收 session ID；批量消费在 pending 出现时让出，并在 pop 后按 slot session 再次应用配置，避免新 session 首包按旧配置丢失。END 校验完整 IQSC 的 session/total samples，并在 ring 为空且最后入包 quiet >=20 ms 后收束；仅在 frontier 未覆盖 expected，或发生 ring drop/sample/sequence discontinuity 时，才附加要求 END age >=100 ms。100 ms 不是总等待上限。START 会切换 display session 并清除旧 frame/tile。普通 data header 不带 session ID，数据连续性检查只能标记 discontinuity 或拒绝无效/越界包，不能提供绝对的旧 session 隔离。
- `src/framework/sdr_control_client.*` 在 continuous-single 模式下预取同中心的下一 request/session；四中心 SCAN 仍只预取 current+1 且末中心不跨轮预取。active/prefetch 使用完整身份匹配，credit=0、晚到 READY、lost prefetch、retry 和 terminal cancel 的原有安全闭环保持不变。
- 当前 synthetic fallback 每轮注入 2048 个复采样并延时约 2 ms；这是无 SDR 的调度自检节拍，不是 60 MSPS 实时吞吐证明。

## 共享协议

父 Solution 的 `shared/` 是 CPU0/CPU1 的共同源：

- `iq_protocol.h`：32 B IQ header、START/END 共用的 68 B packed IQSC、S16/S8/stream flags；当前数据面只启用 S16/A。
- `display_stream.h`：version 2、504 B derived display frame、4 x 512 B seqlock slots、32 x 16 mask、最多 4 个 bbox 和三个 timing-valid bits。
- `display_tile.h`：ABI v6，shared offset `0x0C00` 起的 `16 x 256 B` cache-aligned seqlock ring；每槽 payload 248 B，携带一个 128-bin pooled row，`levels[]` 从低频到高频。CPU1 正常序列按 sequence O(1) 直读目标槽，只有 overwrite/session handover/partial commit 后才扫描环恢复最老 retained row。16 槽在 500 Mb/s qualified payload 上界下可保留约 `37.78 ms`，覆盖一个 `21.336 ms` 面板周期；在 1 Gb/s 物理理论上界下为 `18.89 ms`，因此依赖约 1 ms 轮询和 VSync 等待中的持续排空。板测累计 119,812 个 tile 时 miss/drop 为 0。sequence 缺口推进 RF 时间轴并插入显式 unknown，多 center session 中无法归属的缺口保守地把已涉及中心置为 unknown。
- `system_protocol.h`：128 B telemetry、64 B UI command 和 pipeline state。
- `ipc_mailbox.h`、`resource_layout.h`：固定 shared SRAM 地址、128 B runtime status ABI v3 和 Solution 分区；runtime 扩展包含 CPU1 自计时 FPS、窗口/推理/tile/underflow 率，以及 IPC/session/command identity 和 runtime flags。

共享协议明确禁止放入原始 IQ、NPU arena、完整瀑布矩阵和 framebuffer。CPU0 发布前后使用 DMB/缓存维护；CPU1 对完整 frame 和渐进 row 分别按 session + sequence 消费，并校验 center、row index 和 row count。VSync 等待期间约每 1 ms 继续排空 row ring，避免显示等待占用共享槽。当前 CPU0 只发布 channel A；frame 的 channel B 字段是扩展接口。

60 MS/s S16 原始 IQ 的连续数据率为 `60e6 x I/Q x 16 bit = 1.92 Gbps`，未计协议开销，不能由当前 1 GbE 链路连续承载。长期实时显示方案是在 SDR 侧建立独立的派生 FFT/STFT 流，携带真实频谱 row、center、RF sample range、时间戳、序号、标度、discontinuity 和 CRC；原始 IQ/模型合同与显示流分离。这样可降低显示数据率并保持可审计的真实时间轴，CPU1 不得以插值或动画伪造缺失 row。

## 主机工具

- `tools/replay_iq_capture.py`：读取历史 capture metadata/bin，校验 SHA-256，支持 `slow`、`factor3`、synthetic、channel A、partial tail 和 `--dry-run`。它使用 UDP/5003 inline IQSC，不兼容旧 UDP/5004 控制面；START/END 都携带配置。slow 默认保留 capture 的逻辑采样率/10 ms 边界，slow 和 factor3 的安全墙钟默认均为 80 Mbps。
- `tools/sdr_iq_udp_stream.c`：保留用于标准 MTU C 压测和原始数据面测试；它不负责历史 capture 的 metadata/START/END 语义。
- `tools/ra8p1-runtime-sampler.ps1`：从 CPU0 J-Link 目标读取 control、四个 display slots、16 个 ABI v6 256 B row slots 和 128 B CPU1 runtime status ABI v3；第一段读取仍为 `mem32 0x220E2200 1664`，因为 tile 共享区总量保持 4096 B。工具按 seqlock/session 选最新 frame/row，输出 timing flags、STFT/NPU/E2E、CPU1 板上自计时速率、IPC/session/command identity、runtime flags 和两个 ELF 的 SHA-256。`-SelfTest` 覆盖离线 parser、slot 选择、ABI 边界/对齐、保留字段和 counter delta。`-WindowSeconds` 只用于侵入式 CPU0 rate 测试，不能作为 CPU1 显示窗口；CPU0 `DisplayFrameDelta` 是 shared display frame 发布计数，不是 CPU1 presented frame。

## 生成物和验收

`ra/`、`ra_cfg/`、`ra_gen/`、`Debug/` 和 `Release/` 由 FSP/构建系统生成，不作为业务源文件维护。`verify-solution.ps1` 对显示/以太网基线执行哈希检查，并验证 Solution 分区、CPU0 map heap 上界、CPU1 D/AVE2D + software fallback、MVE 指令、Ethos-U linked symbols 和 CPU0/CPU1 必需符号。

2026-07-19 静态构建产物：

- CPU0 `Debug/rtthread.elf`：text 238300 B，data 2372 B，bss 1668008 B，SHA-256 `20F146AB3277CD05A6082F3640629FB33A7374453534132294365B554F861633`。
- CPU1 `Debug/ra8p1_sdr_ai_display_solution_20260718_CPU1.elf`：text 363964 B，data 90 B，bss 3553408 B，SHA-256 `07E55E7ED039CF9CF867FB12AB6EB6C479E5E0885C6A01E6EC20AA836CD7D9C9`。

这些 SHA-256 和 synthetic 数值是 2026-07-19 历史基线，不是当前最终镜像。当时仅执行无 SDR 的 synthetic baseline，`g_link_status=0` 且 IQ stats 全 0；脱离 J-Link 的快照为 presented/content `1.704/1.704 Hz`、window/inference `1.700/1.700 Hz`、tile `6.850 Hz`、local underflow `0 Hz`（累计 `0`），stage/error/running `6/0/1`，低结果率来自刻意的 synthetic debug pacing。2026-07-26 最终实流构建、下载和板上指标以上方 `20 ms FOCUS prefetch final hardware run` 为准，已取代当时仅有静态和 synthetic 证据的状态；模型仍是 placeholder，所以这些运行证据不构成 NPU 识别精度验收。CPU1 framebuffer SDRAM 自检包含最多约 200 ms 的启动重试，后续仍禁止独立下载 CPU1 ELF。

该历史镜像官方烧录后的 synthetic 快照给出 STFT `34.516 ms`、NPU `0.757 ms`、E2E `586.823 ms`，window/inference/tile 约 `1.700/1.700/6.850 Hz`，timing flags `0x7`，queue/drop `0/0`。E2E 包含 `2048 samples + 2 ms` 分块供数的累计等待，不代表 STFT/NPU 的纯计算时间。修复前约 `20 Hz` 高压 synthetic 和 `22.244 Hz` underflow 仅保留为历史对照。

CPU0 display frame slot 的计时字段必须在 seqlock 完整性通过后读取；相对 slot base 的 `window_sample_count`、`stft_frame_count`、`stft_cycles`、`npu_cycles`、`end_to_end_cycles`、`npu_inference_count`、`timing_flags` 偏移分别为 `0x144`、`0x148`、`0x14C`、`0x150`、`0x154`、`0x158`、`0x1E8`。J-Link 可关闭 DWT trace state，固件现在会恢复计数器；跨恢复边界的窗口以 valid bits 标为无效，而不是把 0 cycles 解释为真实性能。验收脚本同时检查 CPU0 map 的 `__RAM_segment_used_end__` 低于 shared SRAM。
