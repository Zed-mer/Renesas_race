# RA8P1 SDR 无人机识别工程分享包

这是一个可交接的源码、脚本、已构建制品与有限板端证据快照。请先阅读 `INTEGRATION_GUIDE_CN.md` 和 `PACKAGE_INFO_CURRENT.txt`；它们是本包的当前状态说明。`PROJECT_HANDOFF_20260725.md` 及其他旧文档保留作历史参考。

本版本包含已提交的 SDR 换频保护源码，并在 `artifacts/sdr/current_tune_guard/` 提供了由该源码重新构建的 agent 和 adapter。`artifacts/sdr/` 根目录下带旧哈希名的 SDR 文件早于该改动，不得用于验证当前换频保护。

2026-07-29 已完成真实 SDR 到 RA8P1 的端到端板测：50 个真实采集窗口在 5.49 s 内完成，payload 为 370-393 Mbps，重试、超时、gap、reorder、IQ invalid、CRC error 和 ring drop 均为 0，CPU1 实测 28.497 FPS。当前烧录 ELF、SDR 制品哈希及验收边界见 `evidence/tune_guard_e2e_20260729/`。

## 包内内容

- `cpu0/`：CPU0 完整工程源（含 FSP 配置/生成文件，不含对象文件和 Debug 临时输出）。
- `cpu1/`：CPU1 完整工程源（含 LVGL、GLCDC、触摸和 UI 源，不含对象文件和 Debug 临时输出）。
- `shared/`：CPU0/CPU1 共享 ABI、显示帧和 SDRC 协议。
- `tools/`：SDR 代理源码、mmap adapter、协议/单元测试、板端统计与 campaign 工具。
- `artifacts/`：当前板上 CPU0/CPU1 ELF+MAP，以及 SDR 持久化 autorun、agent 和 adapter 制品。
- `evidence/`：小型、可审阅的近期 campaign 证据；完整原始构建目录没有复制进包。
- `INTEGRATION_GUIDE_CN.md`：当前 RA8P1/SDR 对接、部署与验收说明。
- `PACKAGE_INFO_CURRENT.txt`：源码基线、新 SDR 产物哈希与验证边界。
- `PROJECT_HANDOFF_20260725.md`：历史交接文档。
- `MANIFEST.sha256`：包内所有内容（不含 manifest 自身）的 SHA-256 清单。

## 快速开始

1. 解压到短路径、无中文空格的目录。
2. 安装 Renesas FSP 6.4.0、GCC Arm 13.2.Rel1、SEGGER J-Link，并安装/同步 `ra8p1` 开发技能所需脚本。
3. 从本目录执行：

   ```powershell
   & .\tools\verify-share-package.ps1
   & .\build-solution.ps1
   & .\verify-solution.ps1 -SkipBuild
   ```

4. CPU0 与 CPU1 必须作为一个多核 Solution 烧录，不能单独烧录 CPU1：

   ```powershell
   & .\flash-solution.ps1 -ProbeSerial <实际探针序列号> -Run
   ```

5. 当前换频保护版本必须按 `INTEGRATION_GUIDE_CN.md` 上传 `artifacts/sdr/current_tune_guard/` 中成对的 agent 与 mmap adapter，并在 SDR 端核对 SHA-256。不要假定设备上的历史 autorun 已包含本次修改。

## 重要边界

- 开发机→J-Link、SDR↔RA8P1、开发机→SDR 管理通道是三条独立链路。开发机不能 ping SDR 不代表运行时 IQ 链路失败。
- 当前 NPU 权重是占位模型；屏幕中的真实频谱/瀑布来自 IQ/STFT，但分类精度尚未验收。
- `artifacts/ra8p1/` 中的 ELF/MAP 是 2026-07-29 实际烧录并验收的快照。后续源代码改动后必须重新构建，并用新 SHA-256 重新绑定证据。
- 当前 tune-guard agent 和 mmap adapter 仅部署在 SDR 的 `/tmp` 中运行，尚未持久化到 `/mnt/jffs2`。
- 本包不包含大体积 toolchain、FSP 安装包、J-Link、原始 IQ 文件、调试缓存、或 SDR 固件镜像。
