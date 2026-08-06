# SDR iiod fixed-byte benchmark

The CPU0 boot-time iiod probe is a deterministic transport benchmark. It does
not configure the SDR RF path and does not change the SDR firmware or IP.

## Workload

- Window: 590,336 complex S16 IQ samples, 2,361,344 bytes.
- Run: 5 windows, exactly 11,806,720 payload bytes.
- Maximum `READBUF`: 1,048,576 bytes.
- Request schedule: 11 full requests plus one 272,384-byte request.
- Timing starts immediately before the first `READBUF` and stops after the
  final payload byte. Setup, ping, connect, VERSION, OPEN and CLOSE are outside
  the measured interval.
- The rolling payload checksum remains enabled in every benchmark build.

`g_sdr_iiod_perf_result` schema 2 records the target and received byte counts,
elapsed time, payload rate, rolling checksum, `READBUF` request/chunk counts,
timed-stream socket `recv` attempts, successful timed-stream cache fills, and
the return value from the `SO_RCVBUF` `setsockopt` call. A completed result is
valid only when state is 7, errors and the socket option result are zero, and
`bytes_received` equals `target_bytes`.

## Boot switch

The production default is disabled so the diagnostic TCP connection cannot
contend with the IQSC/UDP data path:

```c
SDR_IIOD_PERF_BOOT_ENABLE=0
```

For an explicit diagnostic benchmark build, set
`SDR_IIOD_PERF_BOOT_ENABLE=1` in the CPU0 compiler definitions. Do not use that
diagnostic build for formal IQSC streaming throughput or loss measurements.
In e2 studio this is the CPU0 configuration's **C compiler -> Defined symbols
(-D)** entry; do not edit the checked-in default or the SDR firmware. Record the
resulting CPU0 ELF SHA-256 as a diagnostic-only artifact. Removing the symbol
(or defining it as `0`) restores the production path.
The production image still retains a zeroed result object for the read-only
CPU0 statistics sampler; retaining that ABI does not create a socket or start
the iiod thread. Any non-zero `IiodPerf` state/byte counter in a formal
acceptance artifact is treated as diagnostic contention and fails that session.

## Capture

After building and flashing the exact CPU0 ELF, capture its fixed-byte result
and the RMAC/lwIP counters with:

```powershell
.\tools\ra8p1-cpu0-net-stats.ps1 `
  -Cpu0Elf <cpu0.elf> `
  -ProbeSerial <serial> `
  -Json
```

For A/B comparisons, keep the target byte count and SDR configuration fixed,
run one warm-up followed by at least five measured resets, and bind every JSON
file to the ELF SHA-256 reported by the script. The new
`LwipTcpipInpktAllocFail` and `LwipTcpipInpktMboxFail` counters distinguish
lwIP input-queue loss from RMAC overflow or descriptor loss.

The script is read-only but briefly halts CPU0, so capture after the benchmark
has reached DONE or FAILED rather than during the timed interval.

## Offline self-test

```powershell
.\tools\ra8p1-cpu0-net-stats.ps1 -SelfTest
```

This checks the fixed request schedule, schema-2 offsets, signed result fields,
64-bit byte counters, and the appended lwIP diagnostic counters without J-Link
or hardware.
