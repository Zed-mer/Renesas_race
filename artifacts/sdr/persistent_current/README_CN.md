# SDR 持久化部署包

本目录绑定当前 RA8P1 固件所使用的 SDR capture agent 和 IIO mmap adapter。文件名采用 SHA-256 前 8 位内容寻址，安装后保存在 SDR 的 `/mnt/jffs2/ra8p1`，启动入口为 `/mnt/jffs2/autorun.sh`。

## 固定运行参数

- RA8P1 目标地址：`192.168.31.20`
- SDR `eth0` 地址：`192.168.31.10/24`
- 默认日志模式：`--no-trace`
- AD9361 RX0/RX1：`manual` 固定增益 `25 dB`，每次启动 agent 前写入并回读校验
- `RA8P1_SDR_FIXED_GAINS_DB=25,25,25,25`
- `RA8P1_IIO_TUNE_SETTLE_US=1000`
- `RA8P1_IIO_TUNE_DISCARD_SAMPLES=4096`
- `RA8P1_SDR_UDP_GSO=1`
- `RA8P1_SDR_CRC_BACKEND=nibble`
- UDP 控制端口：`5004`

`/mnt/jffs2/ra8p1/sdr_trace.enable` 存在时 supervisor 会显式切换为 trace 模式；正常持久化运行应保持该文件不存在。

## 安装

将本目录完整传到 SDR 的临时目录后，以 root 执行：

```sh
cd /tmp/ra8p1_sdr_persistent_current
sh install.sh
```

安装程序会先执行 `SHA256SUMS` 校验，对内容寻址二进制执行冲突保护，再备份当前 `autorun.sh`，最后通过 `.part + mv` 原子发布新入口。它不会删除任何既有备份或旧版二进制。

## 回滚

安装程序把本次被替换的入口路径写入 `/mnt/jffs2/ra8p1/autorun_previous_path`。默认回滚命令为：

```sh
sh rollback.sh
```

也可显式指定一个由安装程序创建的备份：

```sh
sh rollback.sh /mnt/jffs2/autorun.sh.backup.<sha-prefix>
```

回滚只切换启动入口，不删除当前或历史制品，并会先保存被回滚的入口副本。

## 文件职责

- `autorun.sh`：PID/锁校验、旧进程清理、UDP 5004 所有权检查、日志轮转、监督重启和默认 no-trace 启动。
- `install.sh`：哈希门禁、内容寻址发布、入口备份、原子安装和运行态参数校验。
- `rollback.sh`：从安装时记录的入口备份原子恢复。
- `SHA256SUMS`：部署包离线完整性清单。

硬件冷启动验证和 RA8P1 运行计数见 `evidence/sdr_persistent_20260730`。
