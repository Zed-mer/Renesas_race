# SDR IQ UDP sender

`sdr_iq_udp_stream.c` is the project-owned sender for the integrated Solution. It does not modify the proven sender in `git_renesas/ra8p1_competition_eth_test`.

For historical SDR-Dataset-Collector files, use `replay_iq_capture.py` and
`README_IQ_REPLAY.md`. That tool adds the current CPU0 inline IQSC
`STREAM_START`/`STREAM_END` metadata packets; this small C stress sender keeps
the older raw-data-only behavior and is useful only for RMAC transport stress.
The integrated analysis pipeline now requires a valid S16 channel-A IQSC
START/session/END, so raw sender packets are rejected by the STFT path.

The sender uses:

- UDP destination port 5003.
- Standard MTU 1500 IPv4 datagrams: 1472-byte UDP payload, including the 32-byte IQ header.
- IQ format field 1 at header offset 28 for little-endian interleaved signed 16-bit `I0,Q0,I1,Q1,...`.
- 1440 IQ bytes, or 360 complex samples, per packet.
- Header flag bit 2 for frequency/channel B.
- Per-channel sample indices, so alternating A/B packets do not corrupt continuity accounting.

The current FSP/RMAC path is configured for frames up to 1514 bytes and does not accept MTU 9000 Jumbo frames. Jumbo support requires a separate RMAC/FSP configuration and hardware test.

Build on the SDR Linux target or with its ARM cross compiler:

```sh
gcc -O3 -Wall -Wextra -o sdr_iq_udp_stream sdr_iq_udp_stream.c
```

Send synthetic channel A at 700 Mbps for 10 seconds (transport stress only):

```sh
./sdr_iq_udp_stream 192.168.31.20 synthetic a 700 10
```

Send channel B from stdin (transport stress only; integrated analysis rejects it):

```sh
iq_source | ./sdr_iq_udp_stream 192.168.31.20 stdin b 400 0
```

Alternate channel A and B packets (transport stress only):

```sh
dual_channel_source | ./sdr_iq_udp_stream 192.168.31.20 stdin alternate 850 0
```

For `alternate`, every 1440-byte stdin block belongs alternately to A and B. The stdin source must already provide little-endian signed 16-bit interleaved IQ. This mode is a transport/framework test. A production dual-frequency implementation should perform DDC/filter/decimation on the SDR and mux the two resulting subbands explicitly.

S16 IQ uses 32 bits per complex sample. A 700 Mbps IQ payload rate therefore represents 21.875 MSPS across all transmitted channels. A 40 MSPS stream requires 1.28 Gbps before Ethernet/IP/UDP overhead and cannot fit on one 1 GbE link.

The current board measured about 173.25 ms STFT plus 4.47 ms NPU for a
600,033-sample window, a compute-only ceiling near 108 Mbps before receiver and
RTOS overhead. Start functional capture replay at 80 Mbps. The 400/700/850 Mbps
examples above are deliberate RX stress cases and are not sustainable-analysis
claims; this raw sender also omits IQSC, so use `replay_iq_capture.py` when 10 ms
window metadata and stream boundaries matter.
