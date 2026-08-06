# Historical IQ replay

`replay_iq_capture.py` replays the Windows SDR-Dataset-Collector captures to
the current CPU0 fast Ethernet receiver. It is a host-side diagnostic tool;
it does not change either child project. The default destination is UDP/5003
and the default capture root is:

```text
D:\Renesas_race\SDR-Dataset-Collector-Windows-x64\SDR-Dataset-Collector\data\captures
```

## Commands

List the 12 discovered captures and the selected labels:

```powershell
python .\tools\replay_iq_capture.py list
```

Replay a 10 ms capture at a deliberately slower wall-clock rate. The raw S16
samples and 60,003,333 Hz logical RF timebase are not changed, so the file
still produces one 600,033-sample analysis window:

```powershell
python .\tools\replay_iq_capture.py replay 192.168.31.20 `
  20260717T085827Z_54ccb8e6 --mode slow --payload-mbps 80
```

Use the 63-tap windowed-sinc factor-3 decimator. NumPy is required for this
mode; the output rate for the current 60,003,333 Hz files is 20,001,111 Hz.

```powershell
python .\tools\replay_iq_capture.py replay 192.168.31.20 `
  20260717T085827Z_54ccb8e6 --mode factor3 --payload-mbps 80
```

The deterministic synthetic fallback is useful without a capture or SDR:

```powershell
python .\tools\replay_iq_capture.py synthetic 192.168.31.20 `
  --sample-rate-hz 12500000 --duration-ms 100 --tone-hz 1000000
```

Add `--dry-run --no-pace` to validate metadata, hashes, packet counts, and
the output digest without opening a socket. The integrated CPU0 currently
accepts S16 channel A only, so `--channel` is intentionally limited to `a`.
`--no-control` omits BEGIN/END and is not a valid full-pipeline session.

## Wire format

Every datagram is an IPv4 UDP payload with a 32-byte little-endian header:

| Offset | Field | Value |
| ---: | --- | --- |
| 0 | `magic` | `0x5149504B` |
| 4 | `sequence` | Sender resets it at STREAM_START (`0`); the value does not identify a session |
| 8 | `data_length` | 0..1440, a multiple of 4 for S16 |
| 12 | `flags` | synthetic, discontinuity, channel-B, START/END, valid-12-bit |
| 16 | `sample_index` | 64-bit little-endian complex-sample index |
| 24 | `sender_us` | Low 32 bits of the host monotonic microsecond clock |
| 28 | `format` | `1` = S16 LE interleaved `I0,Q0,...` |

The first packet has `STREAM_START` and carries exactly 68 bytes of `IQSC`
configuration. Its packed layout is `<IHHIIIQIIIIIIIIII` (no native padding):

| Offset | Field |
| ---: | --- |
| 0 | magic `0x49515343` |
| 4 | version, size (`1`, `68`) |
| 8 | session ID (uint32) |
| 12 | source sample rate (uint32) |
| 16 | logical/analysis sample rate (uint32) |
| 20 | center frequency (uint64) |
| 28 | RF bandwidth (uint32) |
| 32 | analysis window samples (uint32) |
| 36, 40 | total complex samples low/high (uint32 each) |
| 44 | decimation factor |
| 48 | format (`1`) |
| 52 | valid bits (`12`) |
| 56 | channel mask (`1`, channel A) |
| 60 | stream flags |
| 64 | reserved (`0`) |

The final packet has `STREAM_END`, carries the same 68-byte IQSC configuration,
and uses the next sequence number. CPU0 validates the START/END session ID and
total sample count, then closes only after the ring drains and the last ingress
has been quiet for at least 20 ms. If the received sample frontier is still
short of the declared total, or a sequence/sample discontinuity or ring drop
was observed, END must also be at least 100 ms old before the session is closed.
The 100 ms value is an additional earliest-close guard, not a global timeout.
The ordinary data header has no session ID; it relies on sequence/sample
continuity, which cannot guarantee absolute isolation from late packets of an
older session. No UDP/5004 control plane is used.

## Modes and caveats

- **slow** preserves every input byte and defaults the logical rate to capture
  metadata. Pacing defaults to 80 Mbps, independently of that RF timebase. Use
  `--logical-rate-hz` only when intentionally changing the analysis timebase.
- **factor3** applies a 63-tap anti-alias FIR and keeps every third complex
  sample. The default passband cutoff is 0.30 of the input Nyquist; bandwidth
  is reduced accordingly. It is a real decimation, not merely slower pacing;
  the logical rate is 20,001,111 Hz while wall-clock pacing defaults to 80 Mbps.
- Current board timing measured about 173.25 ms STFT plus 4.47 ms NPU for one
  600,033-sample window. The compute-only ceiling is about 3.37 MSPS / 108 Mbps,
  before Ethernet and RTOS overhead, so 80 Mbps is the conservative replay
  start point. Explicit 400/640 Mbps values are stress tests, not proven
  sustainable settings.
- Pacing is open loop. CPU0 reports ring watermarks and drops but does not send
  UDP credits, ACKs, or retransmission requests; the sender cannot automatically
  slow down in response to a transient queue buildup.
- The collector files are headerless `int16_le`, `I,Q`, with AD936x signed
  12-bit values stored in an S16 container. CPU0 receives `valid_bits=12`.
  Treating these samples as full-scale 16-bit values without that metadata
  makes a full-scale signal appear about 24.1 dB too small.
- Files whose byte count is not divisible by 1440 end with a valid partial
  packet. The tool keeps the final whole complex sample and reports
  `tail_bytes` in dry-run output; it rejects an odd I/Q byte tail.
- Folder labels are preferred in `auto` mode when they disagree with the JSON
  label. Four `djmini3pro` folders retain `bg` in JSON, so `list` marks the
  mismatch and selects `djmini3pro`.
