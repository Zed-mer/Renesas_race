# Zynq-7020 裸机 IQSC v2 窗口发送模块

文件：

- `zynq_iqsc_sender.h/.c`：无 POSIX、无动态分配的 IQSC v2 状态机和四中心窗口采集发送桥。
- `test_zynq_iqsc_sender.c`：主机 mock，校验四中心、START/DATA/END、序列、sample index、窗口发送时序、pacing 和失败统计。
- `sdr_capture_bridge.h`：2R2T DMA 字序转换；生产默认不再预清零 DMA staging。

## 固定合同

- 四中心：2420 / 2464 / 5760 / 5816 MHz。
- 每中心 session：6,000,000 complex S16 LE RX1 IQ，60 MSPS，56 MHz bandwidth。
- CPU0 仍收到一个完整 IQSC session，因此仍按 590,336 点 tile、295,168 点 stride 严格生成 19 tile。
- UDP destination port 固定为 5003；destination IP 由调用方传入，不在模块内硬编码。
- START sequence=0；DATA 从 sequence=1 连续增长；END 的 sample index 恰好为 6,000,000。

推荐把 `request.chunk_samples` 设为 `RA8P1_ANALYSIS_TILE_SAMPLES`（590,336）：

- 60 MSPS 下采集时间约 9.839 ms。
- 2R2T DMA staging：4,722,688 bytes。
- sender 内部 RX1 packet scratch：1,440 bytes。
- 每次 DMA 完成后按 360 sample 小包直接完成 2R2T→RX1 转换并发送，之后才开始下一次 DMA；不再等待 24 MB RX1 session 全部缓存完成，也不再产生 2.36 MB RX1 窗口的 DDR 写入和读回。

相对原 48 MB staging + 24 MB RX1 cache，tile-window 默认工作集降为
4,722,688 bytes staging + 1,440 bytes scratch，减少约 93.44%。生产默认还移除了 DMA 前 staging 清零，四中心 6M cycle 少做 192 MB 的无意义写入。packet scratch 若保持在 cache 中，也避免了四中心约 96 MB RX1 大缓存写入及随后约 96 MB 封包读回；实际 DDR bus 节省量仍取决于 Zynq cache/lwIP pbuf 实现，必须板测。

这不代表真正实时。RX1 S16 IQ 原始速率是 1.92 Gbit/s，超过 1GbE；800 Mbit/s payload pacing 下，一个 590,336 点窗口仅传输数据就约需 23.61 ms。窗口化的目标是降低首个 tile 的等待时间，SDR 内部缓存和非连续采集负责吸收吞吐不匹配。跨多次 one-shot DMA 的 RF 时间连续性必须在真实 7020 上验证，host mock 不证明这一点。

## 最小集成

```c
static int32_t board_udp_send(void *ctx,
                              uint32_t destination_ipv4,
                              uint16_t destination_port,
                              const uint8_t *header,
                              uint16_t header_bytes,
                              const uint8_t *payload,
                              uint16_t payload_bytes);
static int32_t board_delay_us(void *ctx, uint32_t delay_us);
static int32_t board_time_us(void *ctx, uint64_t *time_us);

ra8p1_iqsc_transport_t transport = {
    .destination_ipv4 = board_ra8p1_ipv4,
    .target_payload_mbps = 800,
    .callback_context = &network_context,
    .udp_send = board_udp_send,
    .delay_us = board_delay_us,
    .time_us = board_time_us,
};

ra8p1_iqsc_sender_t sender;
ra8p1_sdr_capture_request_t request = {
    .sample_rate_hz = RA8P1_ANALYSIS_SOURCE_SAMPLE_RATE_HZ,
    .bandwidth_hz = RA8P1_ANALYSIS_BANDWIDTH_HZ,
    .total_samples = (uint32_t)RA8P1_ANALYSIS_TOTAL_SAMPLES,
    .chunk_samples = RA8P1_ANALYSIS_TILE_SAMPLES,
    .timeout_ms = 500,
};

ra8p1_iqsc_sender_init(&sender, &transport);
ra8p1_iqsc_capture_send_four_centers(
    &sender, &sdr_api, sdr_context, &request,
    dma_staging, sizeof(dma_staging),
    first_session_id, &scan_result);
```

`udp_send` 的 header/payload 两段内存在回调返回前有效。裸机 lwIP raw API 可在回调内：

1. 从调用方已有 `udp_pcb` 和 destination `ip_addr_t` 创建/发送 pbuf；
2. 把 32-byte header 与 payload 拷入一个 `PBUF_RAM`；或在已验证生命周期后用 pbuf chain 降低复制；
3. 调用 `udp_sendto(..., destination_port)`；
4. 在返回前释放调用方拥有的 pbuf 引用。

模块不会创建/绑定网卡，不会调用 `netif_set_addr`，也不会改 `eth0`、`usb0`、U-Boot environment 或现有 iiod。点名的 Pluto v0.38 固件实际由 `S40network` 生成接口配置：USB 默认 192.168.2.1，`eth0` 默认 DHCP、设置 `ipaddr_eth` 时才使用静态地址；`S23udc` 启动 iiod，另有 Avahi 和 Dropbear。因此应让现有网络栈继续管理地址，只把已建立的 UDP endpoint 放进 callback context。

## Pacing 与统计

- `target_payload_mbps=0`：不主动等待，按 transport 能力尽快发送。
- 非零：按累计 IQ payload bit 数进行开环 pacing；1 Mbps = 1 bit/us。
- `ra8p1_iqsc_stats_t` 记录 session、capture window、datagram、IQ bytes、延时请求、最后序列/sample index、实际 payload rate 和 transport error。
- UDP callback、time callback 或 delay callback 返回非零时，会分别映射为 `ETRANSPORT`、`ETIME`、`EPACING`，原始 callback status 保存在 `last_transport_status`。

## 可复现测试

主机：

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -Itools -Ishared tools/zynq_iqsc_sender.c \
  tools/test_zynq_iqsc_sender.c -o tools/test_zynq_iqsc_sender
./tools/test_zynq_iqsc_sender
```

Zynq freestanding compile-only gate：

```sh
arm-none-eabi-gcc -std=c11 -mcpu=cortex-a9 -marm -ffreestanding \
  -fno-builtin -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic \
  -Itools -Ishared -c tools/zynq_iqsc_sender.c
```

这些测试只证明协议、转换、窗口顺序和错误处理，不证明 SDR、AXI-DMAC、cache maintenance、PHY 或 RA8P1 实际吞吐。
