# RA8P1/SDR low-latency board campaign

`ra8p1_board_campaign.py` is the executable board-test coordinator for the
590,336-complex-sample production path. It complements, but does not replace,
the older 6,000,000-sample/19-tile acceptance merger.

Use `README_CPU1_CAMPAIGN.md` and `ra8p1-cpu1-campaign.ps1` for the formal
headless trigger. The tool resolves symbols from the exact CPU1 ELF but uses
probe `1082495494` with the CPU0 J-Link target; CPU1 still publishes every
capture request through the normal IPC mailbox.

Do not use `ra8p1_acceptance_report.py` to accept this low-latency campaign:
that tool intentionally enforces the compatibility-mode 6M/19-tile contract.

## Three independent links

1. Programming link: development PC -> J-Link/SWD -> RA8P1 CPU0/CPU1.
   `flash-solution.ps1` owns this path. It does not require Ethernet or ping.
2. Runtime link: SDR `192.168.31.10` <-> RA8P1 `192.168.31.20`.
   IQSC data uses UDP/5003, SDRC control uses UDP/5004, and the legacy
   diagnostic ACK service remains UDP/5002. Only board/agent evidence decides
   whether this link works.
3. Development management link: development PC -> SDR Linux shell. It is used
   only to upload/start the temporary `/tmp` agent and retrieve its log. A
   missing route on the PC is a deployment-channel issue, not a runtime-link
   failure. Temporarily join the same switch/subnet, use USB/serial, or use an
   existing remote shell.

The campaign tool never pings `192.168.31.10` or `192.168.31.20`.

## Fixed runtime contract

- CPU1 owns UI and high-level scheduling.
- CPU0 owns SDRC requests, IQSC/RMAC receive, hardware CRC32C, STFT, NPU, and
  result publication.
- The SDR agent passively waits for CPU0 `CAPTURE_REQ`; it never scans by
  itself.
- Center order is 2420, 2464, 5760, 5816 MHz.
- Every request is 590,336 complex S16 IQ samples at 60 MSPS and 56 MHz RF
  bandwidth. The RF span is 9.838933 ms (derived); the payload is 2,361,344
  bytes.
- The SDR agent and adapters remain in `/tmp`. Re-upload and manually restart
  them after every SDR reboot. Do not change firmware, FPGA, U-Boot, rootfs,
  startup hooks, or the SDR IP.

## Start the temporary SDR agent

Upload versioned artifacts so the currently running `/tmp` process can be
restored during A/B testing. Validate the newly built mmap adapter with one
390 Mbps window before the campaign; it is the production path because it
preserves two-block IIO ping-pong capture without file staging or repeated
small reads. Keep libiio only as a deployment/debug fallback if that smoke test
fails.

```sh
chmod +x /tmp/sdr_capture_agent_0d86a1d5
RA8P1_SDR_UDP_GSO=1 RA8P1_SDR_CRC_BACKEND=nibble \
  /tmp/sdr_capture_agent_0d86a1d5 192.168.31.20 \
  --adapter /tmp/sdr_adapter_iio_mmap_f2b9cfe1.so --trace \
  >/tmp/sdr_capture_agent.log 2>&1 &
```

The sender may use `nibble` or `slice8`; record the chosen value in each
`SDRC window_trace`. CPU0 still must report hardware CRC backend `2` and a
passing hardware CRC self-test.

The mmap adapter guards the first capture after an actual center-frequency
change. It keeps DMA disabled until at least 1,000 us after tuning, captures
4,096 extra complex samples (about 68.3 us at 60 MSPS), and omits that prefix
from the published 590,336-sample window. Same-center captures do not pay this
cost. Override the defaults for a controlled board A/B with
`RA8P1_IIO_TUNE_SETTLE_US` and
`RA8P1_IIO_TUNE_DISCARD_SAMPLES`; setting both to `0` disables the guard.
Successful guarded windows report `last_capture_tune_guarded=1` in
`SDRC window_trace`.

The campaign manifest records the full SHA-256 values; the filenames use only
short prefixes for operator clarity. Do not mix a fallback adapter's results
into the formal mmap campaign.

`--trace` is mandatory for formal campaign evidence. It emits
`SDRC control_trace` and `SDRC window_trace` lines after the timing-sensitive
path. These lines prove that the SDR received CPU0's request, performed tune
and capture, used UDP GSO, received the window ACK, and accepted credit. The
window trace separates IIO block setup, pre-enable tune guard, DMA wait, buffer
disable, and RX1 copy time so capture optimization is based on a measured
substage rather than the aggregate `capture_elapsed_us` value.

## Create the full plan

```powershell
python .\tools\ra8p1_board_campaign.py init `
  --production-rate-mbps 800 `
  --output .\build\board-campaign\plan.json
```

The generated plan contains all mandatory tests:

- one 2420 MHz run with 100 completed windows;
- four-center rate steps at 390/500/600/700/800 Mbps;
- ten complete four-center rounds;
- CRC, dropped DATA, request-timeout, ACK-timeout, and duplicate-request
  injection;
- ten serial and ten overlapped four-center rounds.

Show the exact CPU1 request contract for one scenario:

```powershell
python .\tools\ra8p1_board_campaign.py commands `
  --plan .\build\board-campaign\plan.json `
  --scenario rate-800
```

The request must originate in CPU1's high-level scheduler and cross the
CPU1->CPU0 mailbox. Do not replace that step with a UDP packet sent by the
Windows host. With no physical screen connected, invoke the existing
`display_app_request_capture_with_controls()` path from the CPU1 test/control
entry used for the build. The campaign output gives its rate and fault fields.

For a serial comparison, CPU1 must wait for prior `CREDIT_ACCEPTED` before
requesting the next center. For the overlapped comparison, one continuous scan
lets CPU0 prefetch every next `CAPTURE_REQ` before prior `WINDOW_ACK`, including
center 3 -> 0 between rounds. The verifier determines the mode from SDR agent
event order; a label in the plan is not accepted as evidence.

The duplicate-request test requires an external duplicate of the same
SDRC/5004 datagram (for example a controlled switch/netem tap). It must not
modify SDR firmware. The verifier requires two identical CAPTURE_REQ events
but exactly one capture/send.

## Capture one scenario

Take a read-only SWD baseline immediately before issuing the CPU1 request:

```powershell
python .\tools\ra8p1_board_campaign.py capture `
  --plan .\build\board-campaign\plan.json `
  --scenario rate-800 --phase before `
  --evidence-root .\build\board-campaign\evidence `
  --probe-serial <serial> `
  --cpu0-elf .\ra8p1_sdr_stft_npu_display_solution_20260719_CPU0\Debug\rtthread.elf `
  --cpu1-elf .\ra8p1_sdr_stft_npu_display_solution_20260719_CPU1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf `
  --sdr-agent-artifact .\tmp\build_capture_agent_armhf\sdr_capture_agent `
  --sdr-adapter-artifact .\tmp\build_capture_agent_armhf\sdr_adapter_iio_mmap.so `
  --management-method serial
```

Issue the CPU1 request, wait for the requested completed-window count, then
retrieve `/tmp/sdr_capture_agent.log` over the independent management path.
Capture the final evidence only after timed traffic has stopped. J-Link briefly
halts CPU0, so a mid-stream sample would perturb Ethernet timing.

```powershell
python .\tools\ra8p1_board_campaign.py capture `
  --plan .\build\board-campaign\plan.json `
  --scenario rate-800 --phase after `
  --evidence-root .\build\board-campaign\evidence `
  --probe-serial <serial> `
  --cpu0-elf .\ra8p1_sdr_stft_npu_display_solution_20260719_CPU0\Debug\rtthread.elf `
  --cpu1-elf .\ra8p1_sdr_stft_npu_display_solution_20260719_CPU1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf `
  --sdr-agent-artifact .\tmp\build_capture_agent_armhf\sdr_capture_agent `
  --sdr-adapter-artifact .\tmp\build_capture_agent_armhf\sdr_adapter_iio_mmap.so `
  --management-method serial `
  --agent-log .\build\retrieved-sdr-agent.log `
  --flash-log .\build\flash.log `
  --management-log .\build\agent-deployment.log
```

Each phase invokes the existing read-only collectors:

- `ra8p1-cpu0-net-stats.ps1` for PHY/RMAC/IQ/CRC/ring/SDRC counters;
- `ra8p1-cpu0-trace.ps1` for the 128 exact per-window timelines;
- `ra8p1-runtime-sampler.ps1` for CPU1 IPC/UI-visible state.

Use `capture ... --dry-run` to inspect all J-Link commands without accessing a
probe. Every artifact is bound to exact CPU0/CPU1 ELF SHA-256 values.

Use a fresh scenario directory and an isolated agent-log interval for every
rate or fault. A trace overwrite fails acceptance rather than silently using
the latest 128 records.

## Verify

```powershell
python .\tools\ra8p1_board_campaign.py verify `
  --manifest .\build\board-campaign\evidence\rate-800\manifest.json `
  --output-json .\build\board-campaign\evidence\rate-800\report.json `
  --output-md .\build\board-campaign\evidence\rate-800\report.md
```

The clean-run gates include:

- PHY link-up; measured MDIO reg9/reg10 gigabit capability plus measured
  payload above 100 Mbps derives a 1 Gbps link. It is labeled `derived`, not
  presented as a direct speed measurement.
- CPU0 request/session IDs are unique; center order and 590,336-sample
  contracts match.
- SDR log contains CAPTURE_REQ, tune, capture, UDP GSO send, WINDOW_ACK, and
  CREDIT_ACCEPTED for every selected request/session.
- sequence gap, reorder, invalid packet, ring drop, RMAC overflow/FCS/error,
  driver allocation failure, and message-lost deltas are zero.
- CPU0 reports hardware CRC backend 2, a passed hardware self-test, and valid
  END-to-CRC timing.
- STFT, NPU, ACK, and CPU1-visible timestamps are complete.
- CPU1 reports CPU0 ready, IPC data seen, and received frames even when the
  physical display is absent/headless.

Reported timing labels are strict:

- SDR payload Mbps, CPU0 request->NPU, first-packet->NPU, STFT, NPU, steady
  inference FPS, and first-request->fourth-NPU coverage are `measured`.
- Exact SDR capture-start->CPU0 NPU time is not available without a common
  cross-device clock. `capture_start_to_npu_upper_ms` is therefore `derived`:
  CPU0 request->NPU minus SDR request-receive->capture-start. It includes the
  unknown one-way control delay and is never relabeled as measured.
- RF window span is `derived` from sample count/rate.

## Offline tests

```powershell
python .\tools\ra8p1_board_campaign.py self-test
python -m unittest .\tools\test_ra8p1_board_campaign.py -v
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\ra8p1-cpu0-trace.ps1 -SelfTest
```

Synthetic unit-test evidence only tests parsing and gates. It is never board
performance evidence.
