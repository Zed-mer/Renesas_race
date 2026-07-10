# Known-Good Ethernet Baseline (2026-07-11)

This file records the RA8P1 + RTL8211F-CG Ethernet configuration that passed
the no-packet-loss long-ping test. Treat these values as a locked baseline.
If any listed value changes, repeat the long-ping test before calling the new
configuration stable.

## Hardware and Toolchain

- MCU: Renesas RA8P1, `R7KA8P1KFLCAC`
- PHY: RTL8211F-CG over RGMII
- FSP: 6.4.0
- Toolchain: GNU Arm Embedded 13.2.1 (`13.2.rel1`)
- Build configuration: `Debug`
- Debug launch: DAPLink/PyOCD metadata is present

## Verified RGMII Delay Combination

Source: `board/ports/drv_rtl8211.c`

- RTL8211F internal TX delay: OFF (`RTL_8211F_ENABLE_TX_DELAY = 0`)
- RTL8211F internal RX delay: ON (`RTL_8211F_ENABLE_RX_DELAY = 1`)
- Test label: `TX0_RX1`

Do not enable both delays or disable both delays without rerunning the error
counter and long-ping tests.

## Network and lwIP

Sources: `.config` and `rtconfig.h`

- Netdev: `e0`
- Board IPv4: `169.254.139.109`
- Peer PC / gateway: `169.254.139.8`
- Netmask: `255.255.0.0`
- DHCP: disabled
- lwIP: 2.0.3
- PBUF count: 128
- TCP segments: 256
- TCP send buffer: 65535
- TCP window: 65535
- Ethernet thread priority: 12
- Ethernet thread stack: 4096 bytes
- Ethernet mailbox: 32
- lwIP TCP/IP thread priority: 10
- lwIP TCP/IP thread stack: 4096 bytes
- lwIP TCP/IP mailbox: 32
- TX thread: disabled (`LWIP_NO_TX_THREAD`)
- Hardware checksum: disabled

## FSP RMAC and PHY

Source of truth: `configuration.xml`; generated mirror: `ra_gen/common_data.c`

- MAC: `00:11:22:33:44:55`
- RMAC TX queue length: 15
- RMAC RX queue length: 15
- RMAC TX queues: 2
- RMAC RX queues: 2
- TX descriptors: 64
- RX descriptors: 64
- Ethernet buffer size: 1514 bytes
- RX buffer allocation: enabled
- Layer3 switch interrupt priority: 12
- PHY interface: RGMII
- MDC clock: 2.5 MHz
- PHY0 address: 0
- PHY1 address: 1
- Default PHY: PHY0 / channel 0
- PHY reference clock: enabled
- Flow control: disabled

## Test Evidence

Source: `long_ping_500_summary.json`

- Duration: 504 seconds
- Replies: 500
- Timeouts: 0
- Unreachable: 0
- RX unicast delta: 611
- RX error delta: 0
- RX discard delta: 0
- TX error delta: 0

The shorter RGMII delay sweep in `delay_test_results_short.json` also identifies
`TX0_RX1` as the only tested combination with 8/8 replies and zero RX errors or
discards.

## Build Record

The baseline was rebuilt on 2026-07-11 in an isolated e2 studio workspace.

- Result: `Build Finished. 0 errors, 161 warnings.`
- ELF: `Debug/rtthread.elf`
- HEX: `Debug/rtthread.hex`
- ELF text/data/bss: 157240 / 2212 / 581224 bytes
- ROM total: 159452 bytes
- RAM total: 583436 bytes

The e2ra wrapper reported three Eclipse error markers, but the build log had
zero compiler/linker errors and produced current ELF/HEX artifacts. The marker
status is tool noise, not a failed build.

## Files That Define the Baseline

- `.config`
- `rtconfig.h`
- `configuration.xml`
- `.cproject`
- `.settings/`
- `board/ports/drv_rtl8211.c`
- `libraries/HAL_Drivers/drv_eth.c`
- `ra_cfg/`
- `ra_gen/`
- `src/eth_test.c`
- `src/hal_entry.c`

The sibling snapshot archive and its SHA-256 file are the recovery copy for
this baseline.
