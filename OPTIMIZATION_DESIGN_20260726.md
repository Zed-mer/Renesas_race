# RA8P1 SDR 真实显示恢复与优化设计

## 2026-07-27 当前候选：约 98 ms / ABI v7 / 192-bin / 暂停回看

针对“瀑布流动太快、纵向细节不足、需要暂停拖动”的反馈，当前候选采用 160 个
可视 RF 时间列和 256 列后台历史。每列为 `0.614933 ms RF`，所以屏幕横轴覆盖
`98.389 ms`，暂停快照覆盖 `157.423 ms`；暂停后可横向回看额外 96 列
（`59.034 ms`），恢复即回到最新实时数据。暂停只冻结显示快照，不停止 CPU0 IQ、
STFT、共享 tile 或 CPU1 live history。

瀑布频率维已从 128 提升到 192 个独立组，覆盖同一 56 MHz：FFT1024 原始间隔
`58.59375 kHz`，955 个有效 raw bin 按 187 个 5-bin 组和 5 个 4-bin 组完整聚合，
平均约 `291.67 kHz/格`。频谱曲线仍使用完整窗口产生的真实 128-bin 结果，NPU
输入仍为 `128 x 128 x 3`。因此本轮会明显改善瀑布的纵向细节，但不声称频谱曲线
或模型输入已经升为 192 点。

官方双核构建和静态验收已通过，CPU0/CPU1 ELF SHA-256 分别为
`8E88F2652DD68308E938BB55E633156F346DC831B63283435A7CBACB82CC2A29` 和
`EF3AE72C198D36CBA5B73227120250F6EA298319B88D4852234828BE719CF010`；共享 ABI
摘要为 `99DDF3BB838C23E5A57C4C34858BEBD60A2C767779B95750E2F706DD1CEAEC67`。
`125/125` Python 回归、192-bin 随机等价性、跨核 ABI、Solution 内存、MVE 和
renderer 检查均通过。当前探针未被 Commander 识别，所以还没有烧录，也没有
ABI v7 的板端 FPS、触摸拖动或 underflow 数据；下方 ABI v6 数据仍是可信硬件基线。

## 2026-07-26 当前上板状态：50 ms / ABI v6 / 128-bin

128-bin 试验已通过完整双核构建、官方 `Debug_Multicore` 烧录和板端读取。
CPU0/CPU1 ELF SHA-256 分别为
`71659253BE3D8A170E5D7055C36FAE26E4CCED26003AA16416264EB287203651`、
`AD4D51DFE11CE46EBDE2BDCA5B68A9113D37E36541590A75A40A3A0F46B257AE`。
板端确认 tile v6 为 `128 x 16`、`RowBytes=128`；最终累计 119,812 个
tile，miss/drop/underflow 全 0，presented/content `23.346..23.369 Hz`、
window/inference `7.72..7.78 Hz`、tile `125.0..129.41 Hz`。56 MHz 内约有 120 个独立
pooled 频率估计，8 个相邻格为最近邻重复，不是 128 个独立估计。

## 2026-07-26 历史状态：20 ms FOCUS 实时滚动

本轮已完成 CPU0 continuous FOCUS 同中心预取、CPU1 显示热路径优化和 B 版 20 ms
瀑布验收。最终两个 ELF 已使用探针 `1082495494` 通过官方 `Debug_Multicore`
成功下载并运行：

| 对象 | SHA-256 | 字节数 |
|---|---|---:|
| CPU0 `Debug/rtthread.elf` | `D5F3A31FE7607F2098CC8A98FE2EAD0600C5FE9C12E25DC9102D4974A24348B2` | 3,614,604 |
| CPU1 `Debug/ra8p1_sdr_ai_display_solution_20260718_CPU1.elf` | `65F4B8C99BC229063A3C9F8AA4896A1DE7B35222BF52552E25A3926C8D68A8EE` | 5,000,984 |

共享 ABI 摘要仍为
`16E1C0C5EAB6AE453CC4D7E10ADD2CEA7FE8DE372718BCB611AAACE46D4D6A73`。
`verify-solution.ps1 -SkipBuild` 再次验证 90 个分区宏、CPU0 RAM 使用终点
`0x220D5C64`、96 条 MVE 指令、D/AVE2D 与软件 fallback、16/23 个双核必需符号
和跨核 DWARF ABI。Python 全量回归为 `118/118`；CPU0 host C 状态机 31 个场景
通过，输出 `SDRC client host tests passed`。

数据链路改动不改变完整 590,336-sample STFT/NPU 合同。continuous FOCUS 会用全新
request/session 身份预取同一中心下一窗口，复用既有 READY、credit、promotion、
retry、cancel tombstone 和 terminal confirmation 语义；SCAN 仍只预取下一中心且末
中心不跨轮预取。CPU1 频谱接收只缓存，最多每 100 ms 栅格化一次；瀑布像素生成改用
256 项 RGB565 色表和 64-bin 到 128-row LUT；FOCUS 下检测、告警和频谱只融合当前中心。

`evidence/waterfall20ms_focus_prefetch_20260726/runtime_final.json` 绑定上述双核哈希。
最终恢复快照为 `stage/running/last_error=6/1/0`、GLCDC underflow `0`、center
mask `0x1`；presented/content `20.568/18.609 Hz`、window/inference
`7.81/7.81 Hz`、真实 row `117.65 Hz`。累计生成/消费 `43190/43190` 个真实
列，tile drop 和 IPC tile miss 均为 0。此前同一正式镜像最佳稳定 gauge 为
`23.369/23.369 Hz`、`7.75 Hz` 和 `125 Hz`；应报告区间和零丢失计数，不只取峰值。

相对优化前 FOCUS 基线 presented `17.769 Hz`、content `16.782 Hz`、window
`5.13 Hz`、row `96.15 Hz`，最终恢复快照分别约提升 15.8%、10.9%、52.2%
和 22.4%。物理面板仍是双 lane、40 MHz pixel clock、46.869 Hz，未修改稳定 DSI
时序。

15.485 s 网络区间新增 132 个 completed window，约 `8.52 window/s`，并记录 85 次
同中心 prefetch IQSC proof；timeout、missing-capture、gap、reorder、invalid、
CRC error、ring full/oversize drop 增量均为 0。区间有 1 次 retry，RMAC message-lost
IRQ 的少量增量可能受两次 J-Link halt 扰动，因此该侵入式区间不替代脱离调试器后的
显示 gauge。

受控 390/450/500 Mbps A/B 的 payload 中位数/p95 分别为
`231.439/232.162`、`216.738/232.410`、`232.295/232.522 Mbps`。500 相对
390 的中位数仅提高约 0.37%，p95 仅约 0.15%，没有工程意义；默认继续保持
`390 Mbps / batch 24`。

最新 FOCUS trace 的 126 个完整窗口全部来自 center 0，gap/reorder/invalid/ring
drop 全为 0。当前时段 payload 中位数 `208.450 Mbps`、capture `23.335 ms`、
STFT elapsed span `89.522 ms`、NPU `0.653 ms`、CPU0 load `54.7%`。
STFT span 与收包流水重叠，不能和 payload 时间机械相加。

两块最新 framebuffer 从 `0x68B00000` 和 `0x68C2C000` 各读取 1,228,800 B。
PNG 可见真实瀑布、频谱、`-20 ms/-10 ms/NOW` 横轴、频率纵轴和无彩条 B 布局；
双缓冲分别含 405/419 种颜色并有 281,676 个像素不同，证明捕获期间内容持续变化。
抓帧只用于证明非空真实内容、布局和活动变化，不用于证明无撕裂。

20 ms 是 32 列真实 RF 样本历史：每列 `0.614933 ms`，合计
`19.677867 ms`；它不是 20 ms 墙钟滚屏周期。物理呈现受约 `21.336 ms` VSync
限制，原始 60 MS/s S16 I/Q 连续流又需要至少 1.92 Gbps。长期严格连续的墙钟滚动
仍应由 SDR 侧输出带时间戳、序号、中心、标度、discontinuity 和 CRC 的派生
FFT/STFT 显示流，不能用 UI 插值伪造。

残余正确性项：手动切换时 SCAN/FOCUS 徽标仍在请求入队后乐观翻转，尚未等待匹配
command sequence 的 CPU0 `APPLIED` 回显。它不影响默认 FOCUS 数据链路，但异常
拒绝时徽标可能短暂不可信。模型权重仍是 placeholder，板上性能不能解释为真实识别精度。

日期：2026-07-26（Asia/Shanghai）

## 1. 当前结论

显示面板、MIPI DSI、GLCDC 和 LVGL 不是本次“没有真实频谱/瀑布”的根因。
故障时 Pluto SDR 的 `/tmp` 采集代理未运行，CPU0 收不到真实 IQ，CPU1 只能保留
最后内容或空内容。重新部署并启动临时代理、再通过官方 `Debug_Multicore`
下载同一对 ELF 后，真实数据闭环已经恢复：

```text
Pluto AD9361/IIO -> SDR /tmp agent -> IQSC UDP/RMAC -> CPU0 CRC/ring
-> 590336-sample STFT -> NPU -> shared frame/tile -> CPU1 LVGL -> panel
```

当前恢复仍受绝对约束：不得修改 Pluto 固件、FPGA、U-Boot、rootfs、启动脚本
或 IP。代理只能放在 `/tmp`，Pluto 每次重启后必须重新部署。

## 2. 已验证身份与板上证据

当前板上已验证 ELF：

| 对象 | SHA-256 |
|---|---|
| CPU0 `rtthread.elf` | `D5F3A31FE7607F2098CC8A98FE2EAD0600C5FE9C12E25DC9102D4974A24348B2` |
| CPU1 ELF | `65F4B8C99BC229063A3C9F8AA4896A1DE7B35222BF52552E25A3926C8D68A8EE` |
| SDR agent | `0D86A1D50CE96F3FE3D4A23E3814E69159900CC08BB0815A8706B465178067D9` |
| SDR mmap adapter | `F2B9CFE191BE5ACDCA939592B9D55C37D1F7F276AA99B1E5A1C5C58E3F9D4B6D` |

探针为 `1082495494`，CPU0 诊断串口为 `COM9`，Pluto 管理串口为 `COM7`。
这些值只用于本机本次运行，不替换全局主机配置。

本轮最终 framebuffer 视觉证据位于：

- `evidence/waterfall20ms_focus_prefetch_20260726/framebuffer_68B00000_rgb565.png`
- `evidence/waterfall20ms_focus_prefetch_20260726/framebuffer_68C2C000_rgb565.png`

图中 CH1 2420 MHz 的真实频谱曲线、噪声底和连续瀑布纹理均可见，横轴为
`-20 ms/-10 ms/NOW`，纵轴为 `2392/2420/2448 MHz`。抓帧时 CPU 正在运行，
因此它适合证明非空真实内容和布局，不用于证明无撕裂；同目录 runtime、network、
trace 和 flash summary 将画面绑定到本轮最终 ELF 与探针身份。

本轮最终 `runtime_final.json` 的板上 rate gauge 为：

| 指标 | 实测值 |
|---|---:|
| presented / real-content | `20.568 / 18.609 Hz` |
| window / inference | `7.81 / 7.81 Hz` |
| real tile row | `117.65 Hz` |
| generated / consumed rows | `43190 / 43190` |
| tile drop / IPC tile miss | `0 / 0` |
| stage / running / last_error / underflow | `6 / 1 / 0 / 0` |

网络采样证明 CH1 continuous FOCUS 继续推进：15.485 s 新增 132 个窗口，并记录
85 次同中心预取 IQSC proof；timeout、CRC、gap、reorder、invalid、ring full drop
和 oversize drop 增量均为 0。受控 A/B 后默认仍为 `390 Mbps / batch 24`。

`presented_fps` 是内容驱动的 final flush 计数，不是面板扫描率。静态或低频更新
时显示 1 Hz 不代表面板只有 1 Hz。保持已验证的双 DSI lane、40 MHz pixel clock、
46.869 Hz panel baseline，不把 DSI 调时作为本轮优化手段。

## 3. 快速恢复流程

Pluto 重启后先重新上传两个已固定哈希的文件：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\sdr-serial-upload.ps1 `
  .\artifacts\sdr\sdr_capture_agent_0d86a1d5 `
  /tmp/sdr_capture_agent_0d86a1d5 -Port COM7

powershell -ExecutionPolicy Bypass -File .\tools\sdr-serial-upload.ps1 `
  .\artifacts\sdr\sdr_adapter_iio_mmap_f2b9cfe1.so `
  /tmp/sdr_adapter_iio_mmap_f2b9cfe1.so -Port COM7
```

在 Pluto root shell 中只写 `/tmp` 并启动：

```sh
chmod +x /tmp/sdr_capture_agent_0d86a1d5
nohup /tmp/sdr_capture_agent_0d86a1d5 192.168.31.20 \
  --adapter /tmp/sdr_adapter_iio_mmap_f2b9cfe1.so --trace \
  >/tmp/sdr_capture_agent.log 2>&1 &
sha256sum /tmp/sdr_capture_agent_0d86a1d5 \
  /tmp/sdr_adapter_iio_mmap_f2b9cfe1.so
```

随后验证进程、日志和 RA8P1 动态计数。只有需要替换板上固件或从旧 ERROR
状态恢复时才执行官方 Solution 多核下载，绝不单独下载 CPU1。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-runtime-sampler.ps1 `
  -ProbeSerial 1082495494
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-cpu0-net-stats.ps1 `
  -ProbeSerial 1082495494
```

## 4. 已知恢复缺口

当前 `sdr_control_start()` 正确地隔离 ERROR 状态，但 CPU1 campaign/live retry
在 timeout 后直接再次 START。此时 START 会被判为 BUSY，不能在代理稍后恢复时
自动回到真实采集。

最小可靠修复是统一使用以下状态序列：

```text
timeout/error -> STOP/CANCEL -> 等待 STOPPED/ack -> 有界退避 -> START
```

不得通过允许 `START(ERROR)` 来绕过隔离；还必须保证用户显式 STOP 后不会被
自动重启。该修复需要 host 单测、双核构建、Solution 验证和新的 ELF 哈希，
通过前不替换当前工作正常的板上镜像。

## 5. 当前性能分解

当前 CH1 continuous FOCUS 链路分段实测如下。网络接收、CRC 和 STFT 是流水重叠
关系，不能把每行机械相加成总延迟；表中的“主要影响”用于判断优化优先级。

| 分段 | 当前实测 | 主要影响 |
|---|---:|---|
| IIO/DMAC capture | median/p95 `23.335/23.568 ms` | 高于 9.84 ms RF 窗跨度，仍是墙钟供给成本 |
| request -> first packet | median/p95 `126.045/130.100 ms` | 含 capture、代理调度及调试器扰动 |
| 2,361,344-byte IQ payload send | median/p95 `89.579/90.604 ms` | `208.450/208.776 Mbps`，当前时段主要吞吐瓶颈 |
| last packet -> CRC complete | median/p95 `1.596/3.392 ms` | CRC feed 大部分与接收/STFT 重叠 |
| IQ ring | high-water 244/4096 | 余量充足，full/oversize drop 为 0 |
| CPU0 STFT elapsed span | median/p95 `89.522/91.371 ms` | 渐进处理并与收包重叠，不等于纯计算时间 |
| Ethos-U55 NPU | median/p95 `0.653/0.657 ms` | 不是当前瓶颈 |
| request -> CPU1 visible 上界 | median/p95 `249.939/255.873 ms` | 含供给、计算、跨核和调度边界 |
| CPU0 load | median/p95 `54.7%/58.0%` | 尚有余量；提高发送目标已证明没有有效收益 |
| panel / presented / content | `46.869/20.568/18.609 Hz` | 面板扫描不是供给瓶颈，内容呈现受真实数据节拍限制 |

早期的预取授权竞态和 10 秒空洞已经修复；continuous FOCUS 又增加了同中心下一
request/session 预取。最终 15.485 s 网络区间新增 132 个窗口、85 次同中心
prefetch IQSC proof，timeout/invalid/gap/CRC error 增量均为 0；因此不能再用旧的
`0.40 frame/s` 和 `6.42 columns/s` 作为当前结论。当前观感主要由约 23.3 ms
capture、约 89.6 ms payload、完整 STFT 窗口节拍，以及每个真实 RF row 在 896 px
宽瀑布中占 28 px 共同决定。5 ms submission arming 已消除不必要的软件等待，但
不能越过 46.869 Hz VSync，也不能用插值制造不存在的 row。

当前 590,336 complex samples、1024 FFT、512 hop 会产生 1,152 个 STFT frame，
再按 frequency pool 8、time pool 9 形成 128 x 128 x 3 模型输入。

启动时 synthetic STFT 的已测分解为：

| 阶段 | 时间 | 占 full 比例 |
|---|---:|---:|
| full window | 34.907 ms | 100% |
| FFT | 18.832 ms | 53.9% |
| power reduction | 6.293 ms | 18.0% |
| pool/log/quantize | 5.083 ms | 14.6% |
| ingest/schedule | 2.433 ms | 7.0% |
| Hann window/apply | 2.268 ms | 6.5% |

最终 FOCUS trace 中，STFT elapsed span 中位数为 89.522 ms，但它与约 89.579 ms
的 payload 接收重叠，不能解释为额外 89.522 ms 纯计算；NPU 本身约 0.653 ms。
390/450/500 Mbps 受控 A/B 已完成，500 相对 390 的 payload 中位数仅提升 0.37%，
因此保持 390 Mbps。CPU1 已降低低优先级频谱重绘并使用瀑布 LUT；下一阶段优先测量
SDR 侧派生显示流，不先动模型执行器或稳定的面板时钟。

## 6. 分阶段优化方案

### Phase 0：固定可复现基线

1. 固定 CPU0/CPU1 ELF、agent、adapter 哈希和每次 flash log。
2. 每个 2420/2464/5760/5816 MHz 窗口绑定同一 request/session 的 agent log、
   CPU0 trace、network stats、runtime 和 CPU1 campaign proof。
3. 先做四窗口 smoke，再做四频 10 轮；要求 gap/reorder/CRC/drop/underflow 为 0。
4. 正式验收禁止用 lifetime counter 替代缺失的 `after/cpu1_campaign.json`。

### Phase 1：低风险 CPU0 优化（预取与渐进 row 已完成）

先完成两个直接影响实时显示的改动：

1. **预取 session 原子授权（已落源码并完成板上验收）**：在发送 `WINDOW_ACK credit=1` 之前预置 client
   接收授权状态，并在底层 `sendto()` 前发布 `g_sdr_expected_*`。发送失败时只有
   在没有观察到 reentrant IQSC START proof 的情况下才回滚。host test 必须模拟
   send callback 内同步进入 START，证明首发窗口不再被拒绝。continuous FOCUS
   现在也预取同中心的全新 request/session；最终 15.485 s 区间记录 85 次同中心
   prefetch IQSC proof，timeout/invalid 增量均为 0。
2. **渐进式真实 row 发布（已落源码）**：display tile ABI v6 使用 `16 x 256 B`
   seqlock ring，每槽携带一个 128-bin pooled row，其中约 120 个频率估计独立。
   模型/NPU 保持完整 590,336-sample
   窗不变；CPU0 每生成一个 pooled row 就发布一次，CPU1 立即写入历史，并以
   `5 ms` 软件周期 arming 新的 LVGL 呈现。该值只限制新数据等待提交的时间，物理
   上屏仍受 `46.869 Hz` VSync（约 `21.336 ms`/帧）限制。首窗或 gap 后发布全部
   16 行，连续 50% overlap 窗只发布新增 8 行。该路径不插值、不伪造数据；定向
   测试和 ABI 检查已经覆盖槽驻留、边界和 O(1) 正常消费；ABI v6 板上实流
   采样已证明累计 119,812 个 row 全部生成/消费且 tile miss/drop 为 0。显式 unknown 的
   故障注入视觉验收仍应与正常零丢失吞吐验收分开记录。

随后按下列顺序一次只改一项，每项都做 synthetic 5-run median 与真实四频 A/B：

3. **CRC 与 S16->Q15 单遍实验**：当前 rfpipe consumer 先喂 CRC，再由 STFT
   读取同一 ring payload 做转换。评估在同一 payload traversal 中并行完成
   RA8P1 CRC feed 与 MVE 饱和左移，目标是减少 SDRAM 重读。必须监测 ring
   high-watermark 和控制报文延迟；若增加接收阻塞则撤销。
4. **CRC backend A/B**：当前板上 backend=2，即 RA8P1 CRC peripheral。
   与 slicing-by-8 software backend 比较 total/max cycles、payload Mbps 和
   p95 E2E，不能仅凭“硬件 CRC”名称判断更快。
5. **pool/log/quantize**：为 log2/归一化/量化建立受控 LUT 或定点候选，
   保留 near-boundary 精确 fallback。要求最终 int8 模型输入逐元素一致；若
   放宽为容差，必须先得到模型重训/准确率协议。
6. **masked power reduction 全 MVE**：目前 full-mask group 使用 MVE，边缘
   masked group 回落 scalar。用 predicate/masked MVE 消除边缘标量路径，保留
   peak 的 first-equal tie 规则。
7. **credit=0 prefetch 闭环（已落源码并完成本轮零错误验收）**：继续保留
   `prefetch_iqsc_credit_proofs`，证明 delayed READY 不会把已冻结的 credit=0
   升级。最终区间 `prefetch_credit_without_ready` 增量为 0。
8. **FFT bit-reversal/gather 实验**：只作为独立 A/B。任何取消 bit reversal、
   改写频移读取或 gather 路径都必须保持 1024 bins、peak bin 和 pooled feature
   一致；无明确收益就不保留。

阶段目标不是承诺值：组合后以 synthetic full <=30 ms、单独新增的 STFT hot
exclusive p95 <=40 ms 作为 go/no-go 参考；现有 `STFT elapsed span` 含等包和抢占，
不能直接用于该门槛。同时要求首包到 NPU result p95 不高于当前 `89.081 ms`，
且网络和显示正确性零回归。

### Phase 2：算法合同降维

若目标是稳定 800 Mbps，单个 2,361,344-byte 窗口的理想 payload wire time
约 23.61 ms，而最终 trace 的 STFT elapsed span 中位数为 89.522 ms，并与当前
89.579 ms payload 接收重叠。该字段没有隔离纯 hot compute，因此不能继续沿用
旧版 `45-49 ms` 作为最终实测；下一轮需新增 exclusive cycle 指标。即便如此，
每窗 1,152 次 FFT 也很难在约 23.61 ms 持续间隔内完成，算法合同候选仍应把
FFT frame 数量降到约 128 次后再评估。

可比较两种合同：

| 候选 | RF 时间覆盖 | 计算量变化 | 代价 |
|---|---|---|---|
| 保持 590,336 samples，将 hop 从 512 改为约 4,608 | 仍覆盖约 9.84 ms | FFT frame 约 1/9 | 时间分辨率和特征分布改变 |
| 保持 hop=512，将 tile 缩至 66,048 samples | 缩至约 1.10 ms | FFT frame 约 1/9 | RF 观察窗显著缩短 |

两种方案都会改变训练分布，必须重新生成数据、训练、量化和验证模型。占位模型
只能验证流水线执行，不能作为无人机识别准确率依据。合同变更前后要分别固定
版本，不能把旧模型权重与新 STFT 合同混用。

## 7. 每项优化的保留门槛

候选改动只有同时满足以下条件才保留：

1. 同一 ELF 至少 5 次 synthetic，报告 raw/min/median/max，不只报最好值。
2. 四中心至少各一个真实窗口，正式验收再做四频 10 轮。
3. feature/model input 等价测试通过，NPU checksum 符合对应模型合同。
4. CRC、gap、reorder、invalid、ring drop、tile drop、underflow 均为 0。
5. CFSR/HFSR 为 0，CPU0 stack low-water 仍有明确余量。
6. median 改善至少 5% 或绝对改善至少 1 ms，且 p95 E2E、payload Mbps、
   CPU1 可见性没有显著回归。
7. 当前面板双 lane/40 MHz baseline 不变；显示 FPS 与内容 FPS 分开解释。

## 8. 实施顺序

```text
恢复状态机 + 证据工具
-> 当前 ELF 的干净四频 baseline（已完成）
-> 预取 session 原子授权，关闭 10 秒重传空洞（已完成）
-> 1-row real tile + 5 ms submission arming（已完成）
-> 390/450/500 Mbps 受控传输 A/B（已完成，保留 390 Mbps）
-> 频谱 10 Hz 上限 + 瀑布色表/行映射 LUT（已完成）
-> 瀑布 dirty 区域的 exclusive refresh cycle 测量
-> CRC/convert 单遍
-> CRC backend A/B
-> pool/log/quantize
-> masked power MVE
-> credit=0 prefetch
-> FFT gather/bit-reversal
-> 仅在仍需 800 Mbps 时启动 128-frame 合同和重训
```

每一阶段都先 build/verify，再使用官方 `Debug_Multicore` 下载，随后恢复 Pluto
`/tmp` agent 并采集独立证据目录。禁止单独下载 CPU1，也不编辑 `ra/`、
`ra_cfg/`、`ra_gen/`。

## 9. B 版画布与最新实测更新

本节覆盖前文中已经完成或被最新板上证据修正的内容。当前快速修复已将 CPU1
结果 ownership/ACK 从 LVGL 光栅化和物理 VSync 呈现中解耦，完整窗口率由旧版
约 1 Hz 提升到约 9 Hz；partial tile 也已在完整 NPU 窗结束前持续到达 CPU1。

### 9.1 五种刷新率必须分开报告

| 指标 | 当前值 | 含义 |
|---|---:|---|
| panel scan | `46.869 Hz` | JD9165/GLCDC 的固定扫描率 |
| LVGL presented | `20.568 FPS` | 成功完成 final flush 和 VSync 的画面 |
| real-content | `18.609 FPS` | 含真实频谱或瀑布变化的呈现 |
| FOCUS window/inference | `7.81/7.81 Hz` | 当前 CH1 的完整 STFT/NPU 窗口 |
| FOCUS real row | `117.65 Hz` | CPU1 runtime 的真实 row 生成/消费速率 |
| selected-center spectrum | 最大 10 Hz，实际随 window dirty | 频谱栅格化最多每 100 ms 一次 |

当前 18.609 FPS 不是面板扫描率。最终区间 GLCDC underflow 为 0、tile miss/drop
为 0；面板其余扫描周期只重复上一帧，不能计为真实内容更新。当前还应单独量化
整幅 `896 x 326` image invalidation 的 refresh max，才能判断显示侧 SDRAM 重绘
是否已成为高数据速率下的下一个瓶颈。

瀑布 dirty head 现在最快每 `5 ms` 提交一次，且在 `lv_timer_handler()` 之前 arming，
以便赶上下一个可用 VSync。`5 ms` 是软件提交延迟上限，不是 200 FPS 承诺；面板
物理更新上限仍为 `46.869 Hz`。由于一个面板周期约 `21.336 ms`，甚至略长于整段
`19.677867 ms RF` 历史，单个 20 ms RF 视窗内无法保证出现两次独立物理扫描。

### 9.2 B 版实际布局和数据合同

B 版固定 1024 x 600 几何为 `38 + 36 + 386 + 108 + 32`：紧凑头部、窄告警条、
主瀑布、次级频谱和性能底栏。四个中心各有独立的 `128 x 32` 逻辑历史；选中中心
另有 `128 x 1792` RGB565 双映射渲染环，将每个真实 RF row 固定展开为 28 px，
形成 `896 x 326` 有效瀑布。新列写入 ring 和 mirror，禁止整幅 `memmove`。

每个 pooled row 覆盖 `590336 / 16 = 36896` samples，在 60 MS/s 下为
`0.614933 ms RF`；32 列总范围为 `19.677867 ms RF`。瀑布纵轴是频率且高频在上，
横轴固定标为 `-20 ms / -10 ms / NOW`。这里的时间是样本推导的 RF 时间，不是
tile 到达的墙钟间隔。SCAN 的四中心驻留和已测约 208-233 Mbps 有效 payload 会
让墙钟滚动慢于 20 ms；FOCUS 只消除四中心分时，不能突破 1 GbE 对 60 MS/s S16 IQ
所需 1.92 Gb/s 的物理限制。

tile sequence 缺口不再被压缩：CPU1 在下一条真实 row 之前插入等量的中性
`unknown/gap` 列，最多物理写满 32 列可见历史，但诊断计数保留完整丢失数量。
gap 颜色与强度 0 的真实低功率颜色不同，不参与插值或检测。若同一 transport
session 出现多个 center、导致缺失序号无法诚实归属，则把该 session 已涉及中心的
历史置为 unknown；输入 `DISCONTINUITY` 则在每段连续 flagged row 的首行把受影响
中心的完整历史置为 unknown，再追加真实 row。同 session/window 的合法重试在正常
row 后会重新 arming，因此不会把跨断点的旧数据伪装成连续 RF 时间。

启动 framebuffer 已改为 SDRAM 自检后清黑双缓冲，不再生成彩条。NPU 权重仍是
placeholder；告警只能显示 `RAW/PLACEHOLDER`，不得把 raw score 表述为识别准确率。

### 9.3 最终 FOCUS 126 个完整窗口性能分解

以下数据对应 CPU0 ELF
`D5F3A31FE7607F2098CC8A98FE2EAD0600C5FE9C12E25DC9102D4974A24348B2`；
trace ring 同时含 2 个尚未完成的采集记录，已从完整窗口分位数中排除：

| 环节 | p50 | p95 | 说明 |
|---|---:|---:|---|
| SDR capture | `23.335 ms` | `23.568 ms` | 同中心预取，部分可与上一窗重叠 |
| request 到首包 | `126.045 ms` | `130.100 ms` | 含 capture、代理调度和调试器扰动 |
| IQ first-to-last packet | `89.579 ms` | `90.604 ms` | payload p50/p95 `208.450/208.776 Mbps` |
| last packet 到 CRC 完成 | `1.596 ms` | `3.392 ms` | 接收尾部；CRC feed 与前段重叠 |
| STFT elapsed span | `89.522 ms` | `91.371 ms` | 含等包、RX 抢占和调度，不是 exclusive hot cycles |
| NPU | `0.653 ms` | `0.657 ms` | 不是瓶颈 |
| request 到 CPU1 visible 上界 | `249.939 ms` | `255.873 ms` | 保守控制路径上界 |
| CPU0 load | `54.7%` | `58.0%` | 仍有余量，网络目标升速没有有效收益 |

126 个完整窗口全部为 center 0；网络无 gap/reorder/invalid/CRC error，IQ ring
high-water 最大 244/4096，且无 full 或 oversize drop。390/450/500 Mbps 受控 A/B
已经完成，500 相对 390 没有达到 5% 保留门槛，因此默认保持 390 Mbps。STFT span、
CRC 和 wire time 互相重叠，禁止机械相加。

## 10. B 版后的优化路线

1. **统一证据命名**：跨核状态明确拆成 `CPU1_CONSUMED`、`PANEL_PRESENTED`
   和 `PANEL_SUPERSEDED`。ACK/credit 只依赖 CPU1-owned copy，不依赖 VSync。
2. **补齐精确时间戳**：记录 READY、ACK send/receive 和 next IQ START，定位当前
   约 20 ms 的窗口间空档；目标 ACK 到下一首包 p95 小于 10 ms。
3. **两种运行模式（已落源码）**：`FOCUS` 固定单中心并在选择频段时跟随，优先
   满足实时瀑布；`SCAN` 连续四频覆盖并如实显示每中心 data age。切换先等待旧
   调度 STOP/CANCEL terminal confirmation，再启动新调度。
4. **1-row delta tile（已落源码）**：完整窗口/NPU 合同不变，每个真实 pooled row
   单独发布；后续验收目标是首包到首列 p95 不超过 20 ms、无 tile miss/drop。
5. **频谱渐进预览**：partial tile 携带或生成明确标注的真实 128-bin preview，解决
   当前 SCAN 单中心完整频谱约 2.26 Hz（均分推导）的问题；不得用插值伪造完整窗频谱。
6. **CPU1 尾部解耦**：共享帧轮询、CPU1-owned queue 与 LVGL flush wait 分离，
   目标 NPU result 到 CPU1 consumed p95 小于 3 ms。
7. **CPU0 低风险 A/B**：依次比较 CRC peripheral/Arm CRC32/slicing-by-8，测试
   CRC + S16->Q15 单遍、有限 RX drain/更大 datagram，再优化 FFT bit reversal、
   MVE reduction 和内存放置。每次只保留可测量收益且必须通过 feature 等价测试。
8. **长期派生显示流**：60 MS/s、I/Q 各 16 bit 的连续原始流为
   `60e6 x 2 x 16 = 1.92 Gbps`，尚未计入 Ethernet/IP/UDP 开销，1 GbE 无法承载。
   为获得连续墙钟 20 ms 滚动，应在 SDR 侧生成独立 FFT/STFT 显示流，只传真实
   频谱 row、center、RF sample range、时间戳、序号、标度、discontinuity 和 CRC；
   原始 IQ/模型特征路径保持独立合同。CPU1 只按序列展示派生实测 row，不允许用
   插值或动画填补缺失采样。

当前 1-row tile 每个连续 overlap 窗贡献 8 个新列。单中心达到 100 列/s 要求完整
窗口周期不超过 80 ms；四中心同时达到每中心 100 列/s 要求总窗口周期不超过
20 ms，当前算法和 1 GbE 链路合同均不可达。严格连续墙钟滚动应通过 SDR 侧降采样、
派生 FFT/STFT 显示流或更高速数据面实现，不能用显示插值冒充。

新增验收要求：至少 5 次独立运行、每次 100 窗；gap/reorder/invalid/CRC error、
ring drop、tile miss/drop、underflow、CFSR/HFSR 全为 0；首包到首个真实列 p95
不超过 20 ms；`接收列数 == 写入历史列数`，只有 presentation 可被 VSync 合并。
