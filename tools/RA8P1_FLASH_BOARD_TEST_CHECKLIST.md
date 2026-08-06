# RA8P1 dual-core flash and SDR board-test checklist

This is an execution checklist, not evidence that the steps have already run.
Do not use development-host ping as a gate anywhere in this procedure.

## Fixed identities and link boundaries

- Probe: `1082495494`
- J-Link target: `R7KA8P1KF_CPU0`
- Interface: SWD, 4000 kHz
- CPU0 ELF: `ra8p1_sdr_stft_npu_display_solution_20260719_CPU0/Debug/rtthread.elf`
- CPU1 ELF: `ra8p1_sdr_stft_npu_display_solution_20260719_CPU1/Debug/ra8p1_sdr_ai_display_solution_20260718_CPU1.elf`
- Programming link: PC -> J-Link/SWD -> RA8P1. It has no IP dependency.
- Runtime link: SDR `192.168.31.10` <-> RA8P1 `192.168.31.20`, using IQSC/UDP/5003 and SDRC/UDP/5004.
- Management link: PC -> SDR shell through COM3, USB, or a temporary same-subnet connection. It is used only for `/tmp` deployment and log retrieval.

Never connect J-Link directly to `R7KA8P1KF_CPU1` for campaign control. The
control script connects through CPU0 and writes the CPU1-owned control object;
CPU1 then issues the normal shared-memory IPC request to CPU0.

## Current pre-flash gate

The CPU1 campaign sources are newer than the current CPU1 ELF. The current ELF
does not contain `g_cpu1_campaign_control`, so it must not be flashed for the
headless campaign. A new clean dual-core build is mandatory.

The versioned SDR artifacts are already present in `/tmp`, while the old agent
continues to run. The management transcript is:

`tmp/evidence/sdr_management_upload_20260723_202841.log`

It proves these remote SHA-256 values:

- `/tmp/sdr_capture_agent_0d86a1d5`: `0D86A1D50CE96F3FE3D4A23E3814E69159900CC08BB0815A8706B465178067D9`
- `/tmp/sdr_adapter_iio_mmap_f2b9cfe1.so`: `F2B9CFE191BE5ACDCA939592B9D55C37D1F7F276AA99B1E5A1C5C58E3F9D4B6D`
- `/tmp/sdr_adapter_libiio_dcacb7c4.so`: `DCACB7C473979A303844D651B69909E4E0A06A0A94CA1BA2DC5AB18CD69F5C0A`

The transcript also proves that `/tmp/sdr_capture_agent_b6e6ff83` was still the
running process. Do not stop it until the new dual-core image has passed static
verification and the operator is ready for the 390 Mbps smoke test.

## 1. Host and probe discovery (read only)

```powershell
$root = 'C:\Users\user\Desktop\RA8_PRO\ra8p1_sdr_stft_npu_display_solution_20260719'
Set-Location $root
$env:RA8P1_PROBE_SERIAL = '1082495494'

& "$HOME\.codex\skills\ra8p1\scripts\ra8p1-env-check.ps1"
& "$HOME\.codex\skills\ra8p1\scripts\ra8p1-host-boards.ps1"
& "$HOME\.codex\skills\ra8p1\scripts\ra8p1-debug-probe.ps1" `
  -Action Status -ProbeSerial 1082495494
```

Require probe `1082495494`, VTref near 3.3 V, target
`R7KA8P1KF_CPU0`, and Cortex-M85 identification. Absence of a Windows route to
`192.168.31.0/24` is not a failure of this gate.

## 2. Clean dual-core build and artifact binding

```powershell
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$evidence = Join-Path $root "build\board-campaign\session_$stamp"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

& .\build-solution.ps1 -Configuration Debug -Clean 2>&1 |
  Tee-Object (Join-Path $evidence 'build.log')

& .\verify-solution.ps1 -SkipBuild 2>&1 |
  Tee-Object (Join-Path $evidence 'verify.log')

$cpu0Elf = Join-Path $root 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU0\Debug\rtthread.elf'
$cpu0Map = Join-Path $root 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU0\Debug\rtthread.map'
$cpu1Elf = Join-Path $root 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'
$cpu1Map = Join-Path $root 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.map'

Get-Item $cpu0Elf,$cpu0Map,$cpu1Elf,$cpu1Map |
  Select-Object FullName,Length,LastWriteTimeUtc |
  ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $evidence 'artifacts.json')
Get-FileHash $cpu0Elf,$cpu0Map,$cpu1Elf,$cpu1Map -Algorithm SHA256 |
  Format-Table -AutoSize | Out-String |
  Set-Content -Encoding ascii (Join-Path $evidence 'artifact-sha256.txt')

& .\tools\ra8p1-cpu1-campaign.ps1 -SelfTest
& .\tools\ra8p1-cpu1-campaign.ps1 `
  -Action FourOverlap -Iterations 1 -PayloadMbps 390 `
  -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494 -DryRun -Json |
  Set-Content -Encoding utf8 (Join-Path $evidence 'cpu1-campaign-dry-run.json')
```

Require fresh ELF and MAP timestamps, successful cross-core ABI and Solution
memory checks, the configured MVE/NPU checks, and both CPU1 campaign symbols in
the dry-run output. Do not proceed if the build returned zero but either exact
ELF was stale.

## 3. Official dual-core e2/GDB flash

`flash-solution.ps1` imports the Solution and both child projects, selects
`ra8p1_sdr_ai_display_solution_20260718_CPU0 Debug_Flat`, and downloads both
ELFs. The launch is pinned to CPU0/SWD/4000 kHz/probe `1082495494`; its
`downloadImages` section includes the CPU1 ELF. The `.jlink` settings enable
`VerifyDownload=1`.

```powershell
$flashLog = Join-Path $evidence 'flash-program-verify.log'
try {
  & .\flash-solution.ps1 -ProbeSerial 1082495494 -Run 2>&1 |
    Tee-Object $flashLog
  Add-Content -Encoding ascii $flashLog `
    'PROGRAM_AND_VERIFY=PASS (e2/GDB load returned decisive completion; J-Link VerifyDownload=1; no rejected failure token)'
}
catch {
  Add-Content -Encoding ascii $flashLog 'PROGRAM_AND_VERIFY=FAIL'
  throw
}
```

Keep the complete log. Require the JSON result to contain `status: success`,
the selected `Debug_Flat` launch, GDB `Loading section` evidence, and the J-Link
log path. Reject `Verification failed`, `Timeout while calculating CRC`,
`Error while programming flash`, or `Cannot connect to target`. The PASS line
must only be appended after `flash-solution.ps1` returns successfully.

After reset, use CPU0 shared evidence, not a direct CPU1 attach, to prove CPU1
startup.

## 4. Switch the temporary SDR agent only when ready

This is an SDR management operation, independent of flashing and runtime
Ethernet. It must be run in the SDR shell. It does not modify persistent SDR
storage or startup hooks.

```sh
kill <old-agent-pid>
chmod +x /tmp/sdr_capture_agent_0d86a1d5
RA8P1_SDR_UDP_GSO=1 RA8P1_SDR_CRC_BACKEND=nibble \
  /tmp/sdr_capture_agent_0d86a1d5 192.168.31.20 \
  --adapter /tmp/sdr_adapter_iio_mmap_f2b9cfe1.so --trace \
  >/tmp/sdr_capture_agent.log 2>&1 &
```

`nibble` and `slice8` are both valid sender CRC backends. The selected backend
must be explicit and emitted as `crc_backend` in the window trace; it does not
replace the CPU0 hardware CRC backend/self-test requirement.

Record the old PID and command before stopping it. If the new smoke test fails,
stop the new process and restore the versioned old command. After every SDR
reboot, upload and start the `/tmp` artifacts again.

## 5. Generate campaign plan

```powershell
python .\tools\ra8p1_board_campaign.py init `
  --production-rate-mbps 800 `
  --output (Join-Path $evidence 'plan.json')
python .\tools\ra8p1_board_campaign.py self-test
```

## 6. Run the 390 Mbps smoke test through CPU1

Take the read-only `before` capture first. Use the exact newly built ELFs and
the exact local files whose hashes match the versioned `/tmp` artifacts.

```powershell
python .\tools\ra8p1_board_campaign.py capture `
  --plan (Join-Path $evidence 'plan.json') --scenario rate-390 --phase before `
  --evidence-root (Join-Path $evidence 'campaign') `
  --probe-serial 1082495494 --cpu0-elf $cpu0Elf --cpu1-elf $cpu1Elf `
  --sdr-agent-artifact .\tmp\build_crc_ab\sdr_capture_agent `
  --sdr-adapter-artifact .\tmp\build_capture_agent_fastcopy\sdr_adapter_iio_mmap.so `
  --management-method serial

& .\tools\ra8p1-cpu1-campaign.ps1 `
  -Action FourOverlap -Iterations 1 -PayloadMbps 390 `
  -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494 -Json |
  Set-Content -Encoding utf8 (Join-Path $evidence 'rate-390-request.json')
```

Do not poll CPU1 status repeatedly while timed traffic is active because each
SWD read briefly halts CPU0. Wait for a conservative completion interval, then
read once:

```powershell
Start-Sleep -Seconds 5
& .\tools\ra8p1-cpu1-campaign.ps1 `
  -Action ReadStatus -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494 -Json |
  Set-Content -Encoding utf8 (Join-Path $evidence 'rate-390-status.json')
```

Retrieve `/tmp/sdr_capture_agent.log` through COM3 or another management path,
then take the `after` capture with `--agent-log`, `--flash-log`, and
`--management-log`. Finally run `ra8p1_board_campaign.py verify` exactly as
documented in `README_BOARD_CAMPAIGN.md`.

Require all four windows, CPU1 IPC visibility, hardware CRC backend 2 and self
test, zero gap/drop/CRC/ring/RMAC error deltas, complete ACK/CREDIT, complete
STFT/NPU timing, and measured payload Mbps. Do not relabel a derived or
estimated number as measured.

## 7. Campaign request mapping

Use the same `before -> CPU1 request -> wait -> one ReadStatus -> retrieve SDR
log -> after -> verify` sequence for every scenario.

```powershell
# One 2420 MHz window sequence, repeated 100 times
& .\tools\ra8p1-cpu1-campaign.ps1 -Action Single -CenterIndex 0 `
  -Iterations 100 -PayloadMbps 800 -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494

# Ten four-center rounds with CPU0 prefetch
& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourOverlap -Iterations 10 `
  -PayloadMbps 800 -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494

# Ten four-center rounds without overlap
& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourSerial -Iterations 10 `
  -PayloadMbps 800 -Cpu1Elf $cpu1Elf -ProbeSerial 1082495494
```

For the rate ladder, run a fresh one-round `FourOverlap` campaign at 390, 500,
600, 700, and 800 Mbps. Stop increasing the production rate at the first
persistent gap/drop/CRC/ring/RMAC error or ACK timeout. Fault flags are CRC=1,
drop=2, request-timeout=4, and ACK-timeout=8. The duplicate-request case needs
an external controlled datagram duplicate; it is not a firmware flag.

## Primary risks

- A stale CPU1 ELF can pass unrelated static checks but lacks the campaign ABI.
- Direct CPU1 J-Link attachment can enable or disturb the secondary core and
  create false startup/fault evidence.
- Read-only SWD still halts CPU0 briefly and can perturb a live 800 Mbps stream.
- The new agent is uploaded but not active; old-agent results cannot be bound
  to the new agent hash.
- GSO support and zero loss at 800 Mbps are not proven until the new agent and
  new dual-core ELF pass the rate ladder on the real runtime link.
- A PC management route failure blocks `/tmp` deployment/log retrieval only;
  it does not prove the SDR-to-RA8P1 runtime link is down.
- The mmap adapter is the production candidate. Results produced with the
  libiio fallback must be labeled separately.
