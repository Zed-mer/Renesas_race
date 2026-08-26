# RA8P1 RTL8211F-CG Ethernet Test

This project is a small RT-Thread + lwIP Ethernet smoke test for a Renesas
RA8P1 core board connected to an external RTL8211F-CG Ethernet module through a
custom adapter board.

It was created from the existing RA8P1 RMAC/RTL8211F base project and stripped
down to Ethernet validation only. The FSP RMAC, Layer3 Switch, RGMII PHY, lwIP,
SAL, and netdev configuration are preserved.

## Default Network

- Netdev name: `e0`
- Board IP: `169.254.139.109`
- Peer PC / gateway: `169.254.139.8`
- Netmask: `255.255.0.0`
- MAC address in FSP config: `00:11:22:33:44:55`
- PHY chip: RTL8211F-CG
- PHY interface: RGMII + MDIO/MDC

The static IP values are in `rtconfig.h`:

```c
#define RT_LWIP_IPADDR "169.254.139.109"
#define RT_LWIP_GWADDR "169.254.139.8"
#define RT_LWIP_MSKADDR "255.255.0.0"
```

## Important Hardware Assumptions

The current FSP pin configuration is copied from the working RA8P1 Titan
Ethernet setup. If your adapter board uses different RA8P1 pins for RGMII, MDC,
MDIO, PHY reset, or PHY interrupt, update `configuration.xml` in e2 studio and
regenerate the FSP code before testing hardware.

Current PHY-LSI addresses in `configuration.xml`:

- `g_rmac_phy_lsi0`: address `0`
- `g_rmac_phy_lsi1`: address `1`

If the purchased RTL8211F-CG module straps a different PHY address, change the
matching `phy_lsi_address` in FSP.

Current Ethernet signal assignment names in `configuration.xml`:

- `ETHERNET_MDIO`, `ETHERNET_MDC`, `ETHERNET_MDINT`, `ETHERNET_RST`
- TX: `ETHERNET_TXD0`..`ETHERNET_TXD3`, `ETHERNET_TCLK`, `ETHERNET_TCTL`
- RX: `ETHERNET_RXD0`..`ETHERNET_RXD3`, `ETHERNET_RXC`, `ETHERNET_RXCTL`

Competition board helper output:

- `P714` (`BSP_IO_PORT_07_PIN_14`, symbolic name `PARLCD_D22R6`) is configured
  by `src/hal_entry.c` as GPIO output HIGH at startup.

## Shell Commands

After flashing, open the RT-Thread serial shell and run:

```text
ifconfig
eth_status
eth_wait 10000
ping 169.254.139.8
eth_ping 169.254.139.8 4
```

For a direct PC-to-board cable, set the PC Ethernet adapter manually:

```text
PC IP:      169.254.139.8
Netmask:   255.255.0.0
Gateway:   blank
Board IP:  169.254.139.109
```

Then test:

```text
ping 169.254.139.8
eth_ping 169.254.139.8 4
```

## Known-Good No-Loss Baseline

The configuration frozen on 2026-07-11 uses RTL8211F internal TX delay OFF
and RX delay ON. A 500-packet long ping completed with 500 replies, 0
timeouts, 0 RX errors, and 0 RX discards. See
`KNOWN_GOOD_NO_LOSS_BASELINE.md` for the complete parameter and build record.

## Source Map

- `src/hal_entry.c`: prints startup information and starts the link monitor.
- `src/eth_test.c`: adds `eth_status`, `eth_wait`, and `eth_ping`.
- `board/ports/drv_rtl8211.c`: RTL8211F target initialization hook.
- `libraries/HAL_Drivers/drv_eth.c`: RT-Thread Ethernet device driver over RA RMAC.

## Expected Runtime Signs

Good signs:

- `ifconfig` shows `e0` with `UP LINK_UP`.
- `eth_wait` reports `link ready`.
- `ping` or `eth_ping` receives replies.

Common failure clues:

- No `e0`: RMAC driver did not register. Check build config and generated FSP code.
- `e0` exists but `LINK_DOWN`: check RJ45 module power, 25/50 MHz clock mode, RGMII pins, reset, and PHY strap pins.
- `LINK_UP` but ping fails: check IP subnet, PC firewall, cable/router, and MAC/IP conflict.
