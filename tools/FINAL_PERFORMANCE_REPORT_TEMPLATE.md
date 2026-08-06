# RA8P1 SDR 流式推理最终性能报告

> 本模板用于人工补充测试环境、优化历史和结论。机器判定表由
> `ra8p1_acceptance_report.py` 生成并附在本报告后。

## 1. 发布物身份

- 日期：`<UTC/本地时间>`
- 板卡/探针序列号：`<probe serial>`
- CPU0 ELF SHA-256：`<hash>`
- CPU1 ELF SHA-256：`<hash>`
- SDR 固件/库 SHA-256：`<hash>`
- 构建命令与配置：`<command/configuration>`
- 烧录命令与成功日志：`<path>`

## 2. 固定数据与算法契约

- 中心频点：2420 / 2464 / 5760 / 5816 MHz
- 采样：60 MSPS，S16 LE interleaved I,Q
- Session：6,000,000 complex samples / 24,000,000 bytes
- STFT tile：590,336 samples，stride 295,168，严格 19 tile
- FFT/hop/frames：1024 / 512 / 1152
- RF 窗跨度：9.838933 ms（590,336 / 60 MSPS）
- 每窗 IQ payload：2,361,344 bytes
- 模型状态：`<placeholder/production and accuracy evidence boundary>`

## 3. 正确性与故障证据

- 四中心完整轮数：`<count>`
- 每 session 19 tile：`<PASS/FAIL + evidence>`
- gap/reorder/invalid：`<0/0/0>`
- ring full/oversize drop：`<0/0>`
- backpressure：`<0>`
- NPU checksum：`<value>`
- CFSR/HFSR：`<0/0>`
- CPU1 headless/UI 缓存：`<heartbeat/tile/session evidence>`

## 4. 延时分布

| 目标速率 | payload wire time | 首包→窗完成 p50/p95/max | 窗完成→NPU发布 p50/p95/max | 窗完成→CPU1发布 p50/p95/max |
|---:|---:|---:|---:|---:|
| `<Mbps>` | `<ms>` | `<ms>` | `<ms>` | `<ms>` |

说明时间戳来源、时钟频率、跨核同步方法、采样数量、预热次数和 percentile
算法。缓存发送不要求等于 RF 实时速率，RF 9.838933 ms 窗跨度和网络传输
耗时必须分开表述。

## 5. 吞吐扫速与稳定默认值

| 目标 Mbps | 四中心轮数 | 零丢包 | 19 tile | p95 延时 | 结论 |
|---:|---:|:---:|:---:|---:|:---:|
| `<rate>` | `<count>` | `<yes/no>` | `<yes/no>` | `<ms>` | `<pass/fail>` |

- 最高完整四中心零丢包速率：`<peak Mbps>`
- 90% 规则：`floor(<peak> * 0.90) = <stable Mbps>`
- 稳定速率独立复测：`<PASS/FAIL>`
- 长稳时间/轮数：`<duration/cycles>`

## 6. 分阶段性能与优化历史

| 版本/ELF | 改动 | STFT | NPU | E2E | 吞吐 | 正确性/稳定性结论 |
|---|---|---:|---:|---:|---:|---|
| `<hash>` | `<change>` | `<cycles/ms>` | `<cycles/ms>` | `<cycles/ms>` | `<Mbps>` | `<result>` |

记录每项候选优化的基线、重复次数、中位数/范围、收益、回归和保留/撤销决定。
“不存在进一步优化空间”只能由已测瓶颈和无收益/损害正确性或稳定性的实验支持。

## 7. 证据边界与剩余风险

- `<hardware stages included/excluded>`
- `<screen disconnected/headless limitation>`
- `<placeholder model accuracy limitation>`
- `<remaining external dependency or none>`

## 8. 可复现命令

```text
<build>
<flash>
<sender sweep>
<per-session network/runtime/latency capture>
<offline acceptance merger>
```
