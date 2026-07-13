# RA8P1 Competition Board to 7020 AD936X SDR Ethernet Results

Test date: 2026-07-13

## Configuration

- RA8P1: `192.168.31.20/24`
- SDR: `192.168.31.10/24`
- Link: 1000 Mbps full duplex on both endpoints
- UDP payload: 1472 bytes
- Measurement duration: 10 seconds per run
- RGMII/PHY locked: RTL8211F TX delay off, RX delay on
- RMAC descriptor count unchanged: 64 RX, 64 TX
- RX driver: one-copy RMAC-to-lwIP pbuf path
- Socket UDP service: port 5001
- High-rate raw UDP RX sink: port 5002
- lwIP: 128 pbufs, TCP receive mailbox 64, tcpip mailbox 32
- Thread priorities: tcpip 10, Ethernet RX 12, iiod consumer 9

## Zero-Loss UDP Results

| Direction | Run | Sent packets | Received packets | Payload bytes | Elapsed ms | Payload Mbps |
|---|---:|---:|---:|---:|---:|---:|
| SDR to RA8P1 | 1 | 61,991 | 61,991 | 91,250,752 | 10,000 | 73.000 |
| SDR to RA8P1 | 2 | 61,986 | 61,986 | 91,243,392 | 10,000 | 72.994 |
| SDR to RA8P1 | 3 | 61,989 | 61,989 | 91,247,808 | 10,000 | 72.998 |
| RA8P1 to SDR | 1 | 66,542 | 66,542 | 97,949,824 | 10,000 | 78.359 |
| RA8P1 to SDR | 2 | 66,538 | 66,538 | 97,943,936 | 9,999 | 78.362 |
| RA8P1 to SDR | 3 | 66,537 | 66,537 | 97,942,464 | 9,999 | 78.361 |

This table is the original socket-path baseline. The optimized raw RX path is
reported below.

## Optimized SDR to RA8P1 Raw UDP

The raw UDP callback consumes packets in the lwIP tcpip thread and avoids the
socket receive mailbox and recvfrom copy. Each run uses a clean RA reset.

| Target Mbps | Run | Sent packets | Received packets | Lost packets | Result |
|---:|---:|---:|---:|---:|---|
| 106 | 1 | 90,010 | 90,010 | 0 | zero loss |
| 106 | 2 | 90,010 | 90,010 | 0 | zero loss |
| 106 | 3 | 90,007 | 90,007 | 0 | zero loss |
| 106 | final regression | 90,013 | 90,013 | 0 | zero loss |
| 107 | 1 | 90,856 | 90,852 | 4 | loss begins |
| 107 | 2 | 90,857 | 90,856 | 1 | loss begins |
| 108 | 2 | 91,705 | 91,703 | 2 | unstable edge |
| 108 | 3 | 91,710 | 91,706 | 4 | unstable edge |
| 109 | 1 | 92,558 | 92,530 | 28 | loss |
| 110 | 1 | 93,410 | 93,353 | 57 | loss |

Stable SDR-to-RA zero-loss UDP payload rate: **106 Mbps**.

The optimized RA-to-SDR socket path final regression was **78.507 Mbps**:
66,661 packets and 98,124,992 payload bytes with zero reported send errors.

## Original Socket RX Boundary Sweep

| Target Mbps | Sent packets | Received packets | Lost packets | Loss |
|---:|---:|---:|---:|---:|
| 50 | 42,457 | 42,457 | 0 | 0% |
| 60 | 50,951 | 50,951 | 0 | 0% |
| 65 | 55,195 | 55,195 | 0 | 0% |
| 70 | 59,443 | 59,443 | 0 | 0% |
| 72 | 61,138 | 61,138 | 0 | 0% |
| 73 | 61,991 | 61,991 | 0 | 0% |
| 74 | 62,838 | 60,278 | 2,560 | 4.07% |
| 75 | 63,688 | 55,432 | 8,256 | 12.96% |

## Optimized iiod IQ Stream

- Protocol: iiod 0.25, TCP port 30431, `cf-ad9361-lpc`, RX0 I/Q mask `0x00000003`
- SDR sample rate during verification: 30.72 MSPS
- IIO open buffer: 131,072 samples
- READBUF request: 1 MiB
- RA I/O chunk: 64 KiB
- Socket receive buffer: 128 KiB
- TCP receive mailbox: 64 messages

| Run | Bytes | Elapsed ms | Payload Mbps | Errors |
|---:|---:|---:|---:|---:|
| 1 | 66,060,288 | 10,090 | 52.376 | 0 |
| 2 | 65,011,712 | 10,043 | 51.786 | 0 |
| 3 | 62,914,560 | 10,026 | 50.201 | 0 |
| final regression | 66,060,288 | 10,229 | 51.665 | 0 |

Stable measured iiod payload range: **50.201 to 52.376 Mbps**. This is about
1.57 to 1.64 million complex int16 IQ samples/s at four payload bytes per
complex sample. The original RA iiod client measured about 1.304 Mbps.

At four bytes per complex int16 sample, 30.72 MSPS requires 983.04 Mbps of IQ
payload before Ethernet/TCP framing. A 60 MSPS complex stream requires 1.92
Gbps and cannot fit on a single 1 GbE link without decimation, channel
selection, reduced sample width, compression, or FPGA-side processing.

## Post-Test Health

- SDR `eth0`: errors 0, dropped 0, overruns 0, frame 0, carrier 0, collisions 0
- Final UDP regression: both directions completed with matching packet counts
- SDR IP and iiod boot configuration were not changed persistently
- Final ELF: text 166,016, data 2,228, bss 671,020 bytes

## Direct IQ Fast Path Final Configuration (2026-07-14)

The RA8P1 boot failure after chip erase was caused by an empty generated
`bsp_mcu_ofs_cfg.h`. The project had all normal option-setting records disabled,
so the old image depended on option settings left by an earlier program. The
FSP 6.4 configuration now generates the same normal OFS values as the known-good
CPKHMI template:

- OFS0 and OFS2 enabled
- OFS1_SEC/OFS1_SEL enabled
- OFS3_SEC/OFS3_SEL enabled
- No BPS, OTP, or PBPS records in the ELF/HEX

The final receive configuration is:

- RMAC zero-copy read plus immediate `BufferRelease`
- direct UDP port 5003 IQ parsing before lwIP pbuf/socket processing
- two RX queues, queue length 63, 128 RX descriptors, 193 buffer nodes
- Ethernet RX thread priority 5
- static RA address `192.168.31.20/24`, gateway `192.168.31.1`
- old RA iiod benchmark client retained in source but no longer auto-started
- RTL8211F TX delay off and RX delay on, unchanged

The SDR sender uses UDP GSO, CPU1 affinity, 1472-byte UDP payloads, a 32-packet
GSO batch, and a default payload cap of 890 Mbps. Passing an explicit rate of
zero still selects the unlimited boundary-test mode. Final sender MD5:

```text
43d3444f76875c9db6f90e9ad1f09100  /tmp/sdr_iq_udp
```

### Stable SDR to RA8P1 IQ Results

Each final qualification run lasted 30 seconds. IQ data excludes the 32-byte
application header in every UDP packet.

| Run | Sent packets | Received packets | Lost | UDP payload Mbps | IQ Mbps |
|---:|---:|---:|---:|---:|---:|
| 1 | 2,267,136 | 2,267,136 | 0 | 889.916 | 870.570 |
| 2 | 2,267,328 | 2,267,328 | 0 | 889.998 | 870.650 |
| 3 | 2,267,328 | 2,267,328 | 0 | 889.998 | 870.651 |

Final stable qualification: **6,801,792 packets, zero loss, zero reordering,
and zero invalid packets**. The average rates were 889.971 Mbps UDP payload,
870.624 Mbps IQ data, and an estimated 929.874 Mbps on the Ethernet wire. For
complex int16 I/Q, this is approximately 27.207 MSPS.

The measured stability boundary is narrow:

| Test | Packets | Lost | Result |
|---|---:|---:|---|
| 895 Mbps target, 30 s | 2,278,112 | 1 | not stable |
| 32-packet GSO unlimited, 30 s | 2,285,120 | 2 | not stable |

Therefore 890 Mbps UDP payload is the fixed zero-loss operating point. The
897 Mbps range is useful only as a short-duration peak result.

## 802.3x PAUSE Tuning (2026-07-14)

Both link partners now advertise symmetric PAUSE. The SDR reports
`Link is Up - 1Gbps/Full - flow control rx/tx`. RA8P1 automatic PAUSE uses:

- `PT=0x0800`, `PFRT=0x80`
- COMA port 0 `PPDL=8`, `PPAL=15`
- automatic PAUSE generation enabled
- `PFTTZ=0`, avoiding a TIME=0 PAUSE whenever pressure is released

Disabling `PFTTZ` reduced PAUSE generation from about 1.98 million to about
30,332 frames per 30-second run. Tests with `PPAL=17` and `PPAL=16`
showed occasional message-lost IRQs at the upper boundary. `PPAL=15`
removed those IRQ increments in steady state.

Three 898 Mbps target qualification runs completed with no loss, reordering,
invalid packets, RMAC overflow, FCS error, or message-lost IRQ increment:

| Run | Sent packets | Received packets | Lost | UDP payload Mbps | IQ Mbps |
|---:|---:|---:|---:|---:|---:|
| 1 | 2,277,568 | 2,277,568 | 0 | 894.015 | 874.570 |
| 2 | 2,274,880 | 2,274,880 | 0 | 892.963 | 873.529 |
| 3 | 2,272,384 | 2,272,384 | 0 | 891.981 | 872.575 |

The resulting stable average is **892.986 Mbps UDP payload** and
**873.558 Mbps pure IQ**, approximately 27.299 MSPS for complex int16 IQ.
This improves the previous 870.624 Mbps qualification by 2.934 Mbps.

The next rate step is not stable: a 899 Mbps target produced 894.183 Mbps UDP
and 874.738 Mbps IQ but lost 5 packets. Unlimited mode reached 897.559 Mbps UDP
and 878.011 Mbps IQ but lost 5 packets. At this point the stable boundary is
set by SDR userspace UDP-GSO pacing/burst jitter rather than PHY errors or
RMAC overflow.
