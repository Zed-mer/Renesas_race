# RA8P1 无人机检测 + SDR 持久化完整交付包

本包是 2026-07-30 当前工作树快照，包含可重新构建的 RA8P1 双核工程、已集成的 RF V12/V13 检测算法、当前 ELF/MAP、SDR capture agent 源码与经过实机重启验证的持久化部署系统。

## 目录

- `cpu0/`：CPU0 RT-Thread、以太网 IQ、STFT、Ethos-U55 NPU 和检测算法工程。
- `cpu1/`：CPU1 LVGL、频谱/瀑布、检测结果与交互工程。
- `shared/`：双核共享内存、显示、活动检测和 SDR 协议 ABI。
- `算法设计/`：本次移植所依据的算法设计与数据集验证材料。
- `tools/`：SDR agent/adapter 源码、构建、烧录、统计和测试工具。
- `artifacts/ra8p1/`：本次实机版本对应的 CPU0/CPU1 ELF 与 MAP。
- `artifacts/sdr/persistent_current/`：可直接部署的 SDR 持久化系统。
- `evidence/sdr_persistent_20260730/`：SDR 重启后 IQ、NPU、频谱/瀑布与故障寄存器证据。
- `MANIFEST.sha256`：包内所有文件的 SHA-256 清单。

## RA8P1 构建与烧录

环境要求：Renesas FSP 6.4.0、GCC Arm 13.2.Rel1、SEGGER J-Link。建议解压到短路径后执行：

```powershell
& .\tools\verify-share-package.ps1
& .\build-solution.ps1
& .\verify-solution.ps1 -SkipBuild
& .\flash-solution.ps1 -ProbeSerial <J-Link序列号> -Run
```

CPU0 和 CPU1 必须使用 Solution 的多核配置成套构建、烧录，不能把 CPU1 当作独立工程下载。

本包绑定的板测 ELF：

- CPU0：`BB84558AAE4396478F7CD111A3EB4B413265C59876A2CD3587169C50E7D54C7C`
- CPU1：`F14BBA78BABB6F0C9D58E45AA75DE4F2DC47967424AA37E573E7D6A429C94376`

## SDR 持久化部署

将 `artifacts/sdr/persistent_current/` 完整传到 SDR 临时目录，以 root 执行：

```sh
cd /tmp/ra8p1_sdr_persistent_current
sh install.sh
```

安装器会校验全部哈希、保护内容寻址目标、备份当前启动入口，并通过 `.part + mv` 原子发布 `/mnt/jffs2/autorun.sh`。回滚执行：

```sh
sh rollback.sh
```

当前持久化制品：

- agent：`D67E6D229FBE7624E24DD0E587AF7336333C0388BF4A89143C6F3BC6E9DA7524`
- mmap adapter：`B68F277DF9BE295EEFE2906220787F124E2900920A05D5FFB8D54D27ADEE7C0F`
- autorun：`A0DF5A925D8E45A328DDD4CCE65FC5C6A9AD36942CB7392E7A2C4ABC4FE5F40B`

## 实机结果

SDR 完整 Linux 重启后 `/tmp` 部署目录已清空，新 agent 在启动第 7 秒从 `/mnt/jffs2` 自动运行。3.375 秒内完成 18 个采集窗口，新增 retry、timeout、gap、reorder、IQ invalid、CRC error 和 ring drop 均为 0。

RA8P1 同期 presented frame、content frame、waterfall tile 和 IPC tile 均继续增长；waterfall drop、IPC missed 为 0，NPU 速率为 5.17 Hz，显示为 30.303 FPS，CPU0 `CFSR/HFSR=0`。详细数据见 `evidence/sdr_persistent_20260730/verification_summary.json`。

## 交付边界

包内包含当前未提交源码改动，以 `WORKTREE_STATUS.txt` 为准。为控制体积，CPU 工程的 `Debug/Release` 中间对象、`.git`、缓存、旧交付 ZIP 和原始 IQ 数据未纳入；当前可烧录 ELF/MAP 已单独放入 `artifacts/ra8p1/`。
