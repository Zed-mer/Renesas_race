# RA8P1 SDR final acceptance report

`ra8p1_acceptance_report.py` is an offline evidence merger. It does not access
the SDR, RA8P1, J-Link, network, or ELF files. It only accepts claims already
bound to exact ELF SHA-256 values in the supplied artifacts.

## Final gates

The report is `PASS` only when the supplied evidence proves all of these:

- Every formal sender session has 6,000,000 complex S16 IQ samples, a unique
  nonzero session ID, the fixed center/index mapping, stride 295,168, and
  exactly 19 tiles.
- Every clean rate has balanced evidence for centers 2420, 2464, 5760, and
  5816 MHz.
- CPU0 reports exactly 24,000,000 IQ payload bytes and the sender's data packet
  count, with zero sequence gaps, reordered packets, invalid packets, ring-full
  drops, and oversize drops.
- If network evidence includes `IiodPerf`, its diagnostic result must remain
  uninitialized with zero state/byte counters; a boot-time iiod benchmark run
  invalidates the formal IQSC session because it contends for the SDR path.
- The final runtime slot for every clean session is tile 18 of 19, contains
  590,336 samples and 1,152 STFT frames, has valid STFT/NPU/E2E timing, and is
  visible through a valid CPU1 tile/runtime snapshot.
- Per-window latency evidence has tile indices 0 through 18, nineteen samples
  for every latency series, and zero packet-loss, reorder, and backpressure
  events. The 590,336-sample RF span at 60 MSPS is 9.838933 ms and must remain
  at or below the 10 ms window gate.
- NPU proof and benchmark magic are valid, at least five timed samples agree
  with the reported min/median/max, `RM_ETHOSU_Open` is zero, checksum fields
  agree, and CFSR/HFSR are zero.
- All CPU0 evidence has one SHA-256 and all CPU1 runtime/latency evidence has
  one SHA-256. Optional expected hashes can bind the result to release ELFs.
- The highest complete four-center zero-loss target rate is found. The exact
  integer `floor(peak * 0.90)` rate is separately present and passes. A failed
  exploratory rate above the peak is allowed only when its failure evidence is
  decisive; missing evidence is never treated as a measured failure.

The 10 ms figure is the RF sample span, not a claim that cached UDP transfer is
real-time. The report separately calculates the 2,361,344-byte window payload
time at both the configured and sender-measured Ethernet rates, then reports
measured first-packet-to-window-complete and post-window publish latency.

## Evidence capture layout

Keep one file per hardware observation so session IDs cannot be accidentally
reused or overwritten:

```text
evidence/
  sender.log
  net/session-1000.json
  runtime/session-1000.json
  ...
  latency/window-latency.json
  npu/reset-1.json
  npu/reset-2.json
```

After each sender session finishes, collect `ra8p1-cpu0-net-stats.ps1 -Json`
and `ra8p1-runtime-sampler.ps1 -Json`. A single snapshot after the END packet
is sufficient; a two-snapshot interval is additional evidence but is not a
replacement for the completed session totals. The merger accepts UTF-8 and
Windows PowerShell 5 UTF-16LE redirection output.

The latency collector must use a documented common monotonic timebase and emit
this schema. The current runtime sampler's final-slot cycles do not contain all
three timestamps, so those values must not be inferred from it:

```json
{
  "Tool": "ra8p1-window-latency",
  "ToolVersion": "1.0",
  "Elf": {
    "Cpu0": {"Sha256": "<64 hex>"},
    "Cpu1": {"Sha256": "<64 hex>"}
  },
  "ClockContract": "all millisecond fields use one monotonic board timebase",
  "Sessions": [{
    "SessionId": 1000,
    "CenterIndex": 0,
    "TileIndices": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18],
    "FirstPacketToWindowCompleteMs": ["19 numeric values"],
    "WindowCompleteToNpuPublishMs": ["19 numeric values"],
    "WindowCompleteToCpu1PublishMs": ["19 numeric values"],
    "PacketLossEvents": 0,
    "ReorderEvents": 0,
    "BackpressureEvents": 0
  }]
}
```

NPU proof JSON uses the schema demonstrated by
`acceptance_fixtures/pass/npu-proof.json`. Repeat `--npu-proof` for reset-stable
runs; every supplied run must pass and use the same CPU0 ELF hash.

## Run

PowerShell does not expand wildcard arguments for native programs, so quote
the globs and let the Python tool expand them:

```powershell
python .\tools\ra8p1_acceptance_report.py `
  --sender-log .\evidence\sender.log `
  --net-stats '.\evidence\net\*.json' `
  --runtime '.\evidence\runtime\*.json' `
  --latency .\evidence\latency\window-latency.json `
  --npu-proof '.\evidence\npu\*.json' `
  --expected-cpu0-sha256 7F86A893FABCDFD617B8020289F88AD27AD7EF50A7CE30FA0FD4C01383601EB1 `
  --expected-cpu1-sha256 07533FB7C172804DF32CDAAF9B203F50AE3224EA6DC1133878127C93EBFE1D16 `
  --output-json .\evidence\final-acceptance.json `
  --output-md .\evidence\final-performance-report.md
```

Exit code `0` means PASS, `1` means evidence was parsed but one or more gates
failed, and `2` means the input itself was unreadable or structurally invalid.

## Offline self-test

```powershell
python .\tools\test_ra8p1_acceptance_report.py
```

The checked-in fixtures are synthetic parser/decision tests only. They must
never be cited as SDR or RA8P1 hardware evidence.
