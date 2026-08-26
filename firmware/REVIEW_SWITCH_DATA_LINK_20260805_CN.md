# RA8P1 V27_6 频段切换与数据链路审查报告

日期：2026-08-05  
审查对象：`RA8P1_DRONE_DETECT_SDR_PERSISTENT_V27_6` 外层工程  
审查方式：源码、MAP/ELF、共享协议、host 回归测试和历史证据只读审查；未烧录

## 1. 结论

当前“屏幕稳定但频段切换卡顿”的首要原因不在 AD9361 调谐、DSI 时钟或 NPU，而在 CPU1 的频段切换显示状态机：

1. 点击频段后，UI 强制等待该频段在点击之后产生一版新的完整窗口，不立即使用已经缓存的频谱、瀑布和检测结果。
2. 新窗口到达后，CPU1 仍按“一次 GLCDC line event 只执行一个小步骤”重建并原子提交画面。
3. 正常 CLUT4 overlay 路径至少需要 38 个面板事件。当前面板为 46.869 Hz，每个事件 21.336 ms，因此仅 UI 构建下限约为 810.8 ms；catch-up、deferred resync、box fusion 或重启会继续增加延迟。

数据链路的主要限制是“2.361 MB 完整 IQ 窗口 + 完整分析 + CPU1 已复制结果 + WINDOW_ACK/credit”串行闭环。预取只允许 SDR 提前采集并缓存下一窗，不能在前一窗 ACK 前发送下一窗 IQ。网络/分析的单窗量级约为百毫秒，明显小于 UI 自身约 0.81 s 的构建下限，因此用户感知的切频卡顿主要是 UI 路径，链路串行化是次级吞吐瓶颈。

建议先做 UI 快速响应和现有双源缓存复用，再做链路时间戳实测，最后改双 session/window ownership。不要先改 DSI、NPU、fastlock，也不要直接增加四频点完整 framebuffer。

## 2. 证据边界

当前发布产物哈希为：

| 产物 | 当前 SHA-256 |
|---|---|
| CPU0 ELF | `CFB6352F4A72CEB85A1359B0DFF45C3106C658901856407B2769A02B4EC32909` |
| CPU1 ELF | `FFBCFC4F20B5D8BED6ED1170E6C4A626C877F21F7DB95BD90AA202BF8D8F2729` |

但 README 记录的是 `BB84558A... / F14BBA78...`，2026-07-29 证据记录的是 `80A17C2A... / 921635F7...`。因此历史的 `50 windows / 5.49 s`、`18 windows / 3.375 s`、payload、STFT、NPU 和 CPU1 visible 数据只能作为架构参考，不能作为当前 V27_6 的性能验收结果。

当前 J-Link 探针未连接，目标电压为 0，`COM9` 不存在，所以本次无法读取 `g_rf_ui_channel_switch_diag`、GLCDC underflow 和 CPU0 trace。以下“约 810.8 ms”是由当前源码和显示时序直接推导的静态下限，不是本次板测值。

## 3. 主要发现

### P0-1：UI 主动把每次点击变成“等待下一完整窗”

`rf_ui_set_selected_channel()` 将目标频段所需的 spectrum/window revision 都设置为当前 revision 的下一版，然后进入 `WAIT_WINDOW`。即使四个频段的 `g_spectrum_data[]`、`g_complete_windows[]` 和瀑布历史已经有效，点击后也不能立即提交缓存。

影响：

- SCAN 模式要等扫描再次轮到目标中心频点。
- FOCUS 模式还要先完成旧调度器的 STOP/CANCEL，再 START 新中心频点。
- 等待期间 `busy_before` 仍会占用 VSync work slot，虽然 `WAIT_WINDOW` 步骤没有写 SDRAM，其他显示工作也可能被推迟。

### P0-2：新窗口到达后仍有至少 38 个 VSync 小步骤

当前正常 overlay 路径的最小步骤为：

| 阶段 | 最少事件数 |
|---|---:|
| `WAIT_WINDOW -> build_start` | 1 |
| spectrum base，40 行 / 32 | 2 |
| spectrum trace，255 段 / 112 | 3 |
| spectrum peak | 1 |
| waterfall base，252 行 / 11 | 23 |
| metadata 分阶段刷新 | 4 |
| spectrum render，66 行 / 20 | 4 |
| 合计 | **38** |

显示时序为 `40 MHz / (1344 x 635) = 46.869 Hz`，所以 `38 x 21.336 ms = 810.768 ms`。这还没有计入等待目标窗口、catch-up、重建重启、deferred resync 和优先级更高的 box fusion。fallback RGB565 路径还会增加瀑布构建和 render 步数。

该设计确实降低了单次 SDRAM 写突发，解释了屏幕稳定和 underflow 改善；但它把稳定性预算直接转换成了可见的切频延迟。

### P1-1：数据链路以完整窗口串行闭环

每个频点固定处理：

- `590,336` complex samples；
- S16 I/Q，共 `2,361,344 B`；
- FFT1024、hop512，共 `1,152` 个 STFT frame。

CPU0 只有在以下条件同时成立时才构造 `WINDOW_ACK`：IQSC 完成、ring 全空、payload/CRC 正确、analysis complete、CPU1 已复制完整结果。下一窗的 credit 绑定在该 ACK 上。SDR 可以用 `credit=0` 预采集下一窗，但不能提前发送下一窗 IQ。

同时，CPU0 的 START 接收明确要求 `ring.queued == 0`，分析管线只有一套 active session 状态。因此当前并不是双窗口流水，只是“采集预取 + 串行传输/分析确认”。

历史异版本数据可用于判断数量级：payload `208.450 Mbps`、capture `23.335 ms`、STFT elapsed span `89.522 ms`、NPU `0.653 ms`，request 到 CPU1 visible 中位数约 `249.939 ms`。这些数据说明 NPU 和调谐不是首要优化点。

### P1-2：接收数据存在多次 SDRAM 访问和复制

当前热路径依次执行：

1. RMAC/UDP payload `memcpy` 到 4096 槽 IQ ring；
2. pipeline 从 ring 读取同一 payload 做 CRC；
3. 再读取一次做 S12/S16 到 Q15 转换；
4. 分块复制到 FFT 环形工作区。

这些访问会占用 CPU0/SDRAM 带宽，但由于 STFT 已能和收包重叠，它是中期优化项，不是约 0.81 s 可见切换延迟的首因。

### P1-3：当前产物与性能证据未绑定

`artifacts/ra8p1/V27_6.sha256` 与当前 ELF/MAP 自洽，但 README、PROVENANCE 和两个历史 evidence 目录记录了至少三组不同 ELF 哈希。当前源码的性能、稳定性和板端证据没有形成同一哈希闭环。

后果：无法判断当前版本真实的 `ACK -> next IQSC START` 空档、切频 line-event 延迟、catch-up 重启率和 underflow。后续任何优化对比都必须先补当前哈希基线。

### P2-1：CPU1 SDRAM 余量过小，不能增加四套完整渲染缓存

当前 MAP：

| 资源 | 使用 / 容量 | 结论 |
|---|---:|---|
| CPU0 SDRAM | `8,920,064 / 11,534,336 B`，77.3% | 尚有约 2.49 MiB |
| CPU0 IQ ring | `6,422,528 B` | 占 CPU0 SDRAM 的主要部分 |
| CPU0 普通 RAM | used end `0x220D662C` | 距 shared 起点仅 `47,572 B` |
| CPU1 SDRAM | `5,025,280 / 5,242,880 B`，95.8% | 仅余 `217,600 B` |
| CPU1 `rf_ui.o` SDRAM | `2,567,296 B` | 最大 UI 数据所有者 |

现有两个 render source 中，每增加一个完整 waterfall+spectrum source 约需 `841,280 B`。扩展到四源还需约 `1,682,560 B`，远大于 CPU1 剩余空间。正确方向是复用现有双源做 LRU/预构建，而不是四频点各放一套完整像素缓存。

### P2-2：交付包有重复项目和依赖版本噪声

外层工程内还有一份同名完整嵌套副本，约 `139.66 MiB / 5,187 files`。关键文件抽查哈希一致，不影响固件运行，但造成：

- 工程入口、审查和脚本选择歧义；
- 包体和哈希验证成本翻倍；
- 外层 `MANIFEST.sha256` 不覆盖嵌套副本。

CPU0 源码包同时携带 lwIP 1.4.1、2.0.3、2.1.2，当前 MAP 实际链接 2.0.3。其余版本应从最终交付包剔除或明确标成 vendor archive，避免误改非生效源码。

### P2-3：一项 tile 调度回归测试与当前稳定显示策略不一致

相关 host 测试共执行 127 项，126 项通过，1 项失败。失败项仍要求 `display_app_drain_tiles()` 一次遍历全部 16 槽，并要求 VSync flush wait 内继续 drain；当前生产代码改为每 1 ms 最多处理 4 槽，flush wait 只 `__WFE()`，这是为限制 SDRAM 突发而引入的新策略。

这更像测试合同滞后，而不是已证明的运行故障，但必须处理：要么恢复经过 underflow 验证的 bounded drain during wait，要么更新测试，证明 16 槽在最坏 21.336 ms VSync 等待和实测 tile rate 下仍不会覆盖。不能长期保留红色回归。

## 4. 建议实施方案

### 阶段 A：先把用户感知切换降下来

1. 分离 `view_channel` 与 `capture_focus_channel`。点击后先展示目标频段最近一版有效缓存，并显示 data age；后台再等待 fresh revision 和执行 FOCUS 调度。
2. `WAIT_WINDOW` 没有实际写工作时不得占用 VSync work slot，避免等待新窗期间冻结其他 spectrum/waterfall 更新。
3. 在 `channel_switch_build_start()` 覆盖 inactive source 前，检查两套 `waterfall_source_state`。若目标 channel/revision 已在任一 source 中，直接 rebind + 原子 metadata commit，形成真正的双源 LRU 快速切换。
4. 将“一事件一步”改为“一事件内按 DWT 时间和字节预算执行多个 micro-step”。初始建议每事件预算不超过 4 ms，并保留现有 32 KiB 单块上限；以实测 underflow 为准逐步提高总预算。
5. 不增加四套完整 framebuffer，不修改 40 MHz/46.869 Hz 稳定显示时序。

阶段 A 的目标：缓存命中切换在 1-2 个 VSync 内有可见响应；fresh window 到达后的完整原子提交 p95 不超过 8 个 VSync（170.7 ms），1000 次切换 GLCDC underflow 增量为 0。

### 阶段 B：先量化链路空档，再改协议

利用现有诊断并补齐以下同一 session 时间戳：

- SDR `CAPTURE_READY`；
- IQSC START、首包、末包、END；
- ring drain complete、CRC complete；
- analysis commit、NPU publish；
- CPU1 owned-copy visible；
- `WINDOW_ACK TX`、`CREDIT_ACCEPTED RX`；
- next IQSC START。

当前已有 `read_rf_ui_diag.ps1`、`read_display_diag.ps1`、`ra8p1-cpu0-trace.ps1` 和 board campaign 统计基础。先用当前两枚 ELF 哈希建立 100--1000 窗基线，再决定协议改造收益。

重点指标：

- `fresh window visible -> UI atomic commit`；
- `IQSC END -> WINDOW_ACK TX`；
- `WINDOW_ACK TX -> next IQSC START`；
- `build_restarts`、catch-up backlog、deferred abort/resync；
- ring HWM/drop、CRC/gap/reorder；
- GLCDC underflow。

### 阶段 C：改成双窗口所有权流水

1. 将“数据接收/CRC 完成”和“分析结果/CPU1 可见”拆成两个不同的所有权事件。
2. SDR 保留至少两个不可变 capture slot；CPU0 为两个 in-flight session 保存独立元数据，下一窗 IQ 传输不再等待前一窗 CPU1 visible。
3. 可将现有 4096 槽 IQ ring 分为两个逻辑 bank，或保留 session-tagged ring 并增加明确的 bank owner；不要只放宽 `ring.queued == 0` 而继续复用单一 analysis/config 状态。
4. 先在完整 payload + CRC 正确且 bank 所有权安全时授予 next-transfer credit；result ACK 继续负责分析结果确认和错误收束。
5. 保证 retry 时 SDR 仍持有对应原始窗口，防止提前释放 buffer 后无法重传。

验收建议：prefetch READY 时 `WINDOW_ACK/transfer credit -> next IQSC START` p95 小于 5 ms；同环境 focus window rate 相对当前 V27_6 提升至少 25%；1000 窗 gap/reorder/CRC/ring drop 均为 0。

### 阶段 D：降低内存流量和长期带宽

1. 合并 CRC 与 S12->Q15 的遍历，减少对 ring payload 的重复读取；用等价性测试保护 CRC、Q15 和 STFT 输出。
2. 根据当前版本实测 HWM 重新评估 4096 槽 ring。历史最大值为 244 或 961/4096，但哈希不同，不能直接据此缩容。
3. 若需要接近连续的 60 MS/s S16 IQ，原始吞吐为 `60e6 x 4 = 1.92 Gbps`，1 GbE 物理上无法持续承载。长期应让 SDR 侧输出派生 FFT/STFT/display row，原始 IQ/NPU 数据链路独立分级。
4. 不要仅通过改变 FFT hop/window 数降低 1,152 次 FFT 后继续使用原模型；这会改变训练输入合同，必须重训并重新验收。

## 5. 验收门槛

| 类别 | 建议门槛 |
|---|---|
| 点击反馈 | p95 <= 2 VSync（42.7 ms） |
| fresh window 后原子切换 | p95 <= 8 VSync（170.7 ms） |
| 1000 次切频稳定性 | GLCDC underflow 增量 0，无 framebuffer guard 错误 |
| 链路交接 | prefetch ready 时 credit/ACK -> next START p95 < 5 ms |
| 数据正确性 | 1000 窗 gap/reorder/CRC/ring drop 全 0 |
| 资源 | CPU1 SDRAM 至少保留 5% 或明确 guard；禁止新增四套完整 source |
| 证据 | 报告、ELF、MAP、SDR agent/adapter 哈希完全一致 |

## 6. 本次验证结果

- Solution memory 静态检查通过：CPU0/CPU1 FLASH、RAM、shared SRAM、SDRAM 分区无重叠。
- UI/显示布局与时序测试：78/78 通过。
- SDR 控制协议、CPU1 campaign、STOP 状态测试：29/29 通过。
- 分析 partial-tile/CRC 测试：19/20 通过，1 项为上述 drain 合同不一致。
- 未完成板端诊断：J-Link/串口当前不在线。
- 未完成当前版本性能证明：历史 evidence 的 ELF 哈希与当前 V27_6 不一致。

## 7. 推荐执行顺序

1. 先实现阶段 A 的缓存显示、WAIT 不占槽、双源 LRU 和多 micro-step 预算。
2. 使用当前哈希上板读取 UI diag + display diag，做 1000 次 SCAN/FOCUS 切换 soak。
3. 用 CPU0 trace 测出每个窗口的 ACK/credit 空档，再决定阶段 C 的协议改造范围。
4. 最后做 CRC/Q15 单遍和 ring 重分配；不要在缺少当前基线时先调整 DSI、NPU 或 fastlock。
