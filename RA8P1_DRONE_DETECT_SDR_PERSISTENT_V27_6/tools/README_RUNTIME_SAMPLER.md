# Runtime sampler

`ra8p1-runtime-sampler.ps1` reads shared SRAM through the CPU0 J-Link target. It
does not write firmware memory and never attaches directly to CPU1. The default
read pass uses these Commander commands:

```text
mem32 0x220E2200 1664
mem32 0x220E3F00 32
```

The first command covers display control, all four 512-byte display slots, and
all 16 cache-aligned 256-byte STFT row slots from display tile ABI v6. Each tile
carries one 128-bin waterfall row; the configured 56 MHz mask currently yields
about 120 independent pooled frequency estimates. The second command covers the 128-byte
CPU1 runtime status ABI v3, including IPC/session/command identity and runtime flags.
The tool briefly halts around each coherent snapshot and always issues
`go` before the measurement interval and again before exiting. It does not
reset the target or write firmware memory. Every display slot, tile slot, and
runtime object is checked with its begin/end seqlock.
The newest display slot is selected only when its session matches the display
control session; the newest valid tile is selected across all retained slots.

One snapshot reports the latest window sequence, sample/frame counts,
STFT/NPU/end-to-end cycles, timing-valid flags, cumulative inference count,
tile sequence, display state, and CPU1 board-self-timed FPS/window/inference/
tile/underflow rates. Use a positive window only when CPU0 counter deltas are
required:

```powershell
Set-Location D:\Renesas_race\RA8P1_SDR_DRONE_SHARE_20260725_final\RA8P1_SDR_DRONE_SHARE_20260725_final
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-runtime-sampler.ps1
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-runtime-sampler.ps1 -WindowSeconds 15
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-runtime-sampler.ps1 -WindowSeconds 15 -Json
```

The script reads `RA8P1_PROBE_SERIAL`, `RA8P1_JLINK_ROOT`, or the host
configuration at `%USERPROFILE%\.codex\ra8p1.json`; `-ProbeSerial` and
`-JLinkExe` override those values. It records SHA-256 hashes for the newest
non-`before-*` CPU0 and CPU1 ELF in the sibling project Debug directories.

Run the offline parser and layout test without a probe:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ra8p1-runtime-sampler.ps1 -SelfTest
```

Cycle conversion is fixed to CPU0 DWT at 1 GHz for this Solution: one million
cycles is one millisecond. The firmware rechecks `TRCENA/CYCCNTENA`, restores
the counter after a debugger changes trace state, and publishes independent
STFT/NPU/E2E valid bits. A zero cycle field is usable only with its valid bit.

On this RA8P1 multicore target, any Commander timing window can halt or phase-
shift CPU1 even when access is through CPU0. `-WindowSeconds` is therefore an
intrusive CPU0 window/inference/frame/tile-rate test; its CPU1 heartbeat and
underflow deltas are diagnostic only. For display acceptance, let the board run
without a debugger and use the default one-snapshot command. CPU1 computes its
own rates every second before the snapshot, so no host-side timing window is
needed. Accepted ABI v6 post-flash snapshots reported 23.346..23.369
presented/content FPS, 7.72..7.78 window/inference Hz, 125.0..129.41 tile Hz,
and 0 underflows, tile misses, or waterfall drops after 119,812 consumed tiles.
Later intrusive sessions are not comparable.
