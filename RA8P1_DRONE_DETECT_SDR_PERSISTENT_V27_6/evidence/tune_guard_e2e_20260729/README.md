# 2026-07-29 tune-guard 端到端板测

目标为 `R7KA8P1KFLCAC`，使用 FSP 6.4.0 和 Solution `Debug_Multicore` 成套构建、烧录 CPU0/CPU1。跨核 ABI、Solution 内存分区、CPU0 MVE、CPU1 D/AVE2D 及烧录验证均通过。

## 绑定制品

| 对象 | SHA-256 |
|---|---|
| CPU0 ELF | `80A17C2A8DC0B90B9305AFFC066B83DE0CE2FBF0E5BC95501F53D46F0BFAC57E` |
| CPU1 ELF | `921635F71E1E799723063E542F0B483C7637EC381DF5653E3C34C3E8EDA7C4A3` |
| SDR capture agent | `D67E6D229FBE7624E24DD0E587AF7336333C0388BF4A89143C6F3BC6E9DA7524` |
| SDR mmap adapter | `B68F277DF9BE295EEFE2906220787F124E2900920A05D5FFB8D54D27ADEE7C0F` |

## 运行结果

- 5.49 s 内完成 50 个真实 IQ 采集窗口。
- payload 为 370-393 Mbps。
- retry、timeout、gap、reorder、IQ invalid、CRC error、ring drop 增量均为 0。
- CPU1 presented 速率为 28.497 FPS。
- 用户现场验收通过。

本目录保存结构化验收摘要，不包含原始实时 trace。SDR agent 和 mmap adapter 本次运行于 `/tmp`，尚未持久化到 `/mnt/jffs2`；SDR 重启后需重新部署。NPU 权重仍为 placeholder，本结果不构成分类准确率验收。
