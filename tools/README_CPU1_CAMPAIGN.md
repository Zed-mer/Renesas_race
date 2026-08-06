# CPU1 headless campaign control

`ra8p1-cpu1-campaign.ps1` is the no-display entry point for the SDR hardware
campaign. It resolves two symbols from the exact CPU1 ELF, but J-Link always
connects to probe `1082495494` as `R7KA8P1KF_CPU0`. Direct CPU1 attach is not
used because it can change secondary-core state.

The three links remain independent:

- Programming/control: development PC -> J-Link/SWD -> RA8P1 CPU0 debug port.
- Runtime: SDR `192.168.31.10` <-> RA8P1 `192.168.31.20`.
- Management: development PC -> SDR shell, only for staging `/tmp` agents.

The host writes only `g_cpu1_campaign_control`. CPU1 consumes that object and
calls `ipc_bridge_cpu1_command_send()`. CPU0 then consumes the normal shared
command mailbox and owns SDRC/UDP/5004. The host never writes the CPU0 command
mailbox and never sends an SDR capture request itself.

## Fixed ABI

Both objects are 32-byte aligned and use an even begin/end seqlock. Multi-byte
fields use the RA8P1 little-endian memory representation.

| Symbol | Owner | Bytes | Magic | End sequence offset |
|---|---|---:|---:|---:|
| `g_cpu1_campaign_control` | host writes, CPU1 reads | 64 | `0x51525043` (`CPRQ`) | 60 |
| `g_cpu1_campaign_proof` | CPU1 writes, host reads | 128 | `0x46525043` (`CPRF`) | 124 |

Request modes are `STOP=1`, `SINGLE=2`, `FOUR_OVERLAP=3`, and
`FOUR_SERIAL=4`. For single mode, `Iterations` is the number of windows. For a
four-center mode it is the number of complete `0,1,2,3` rounds, so 10 means 40
visible inference results.

CPU1 first sends STOP through IPC for every new request. In serial mode it
sends one single-center command only after the prior result is CPU1-visible.
In overlap mode it sends one continuous `SCAN_ALL` command for all requested
rounds; CPU0 retains ownership of center prefetch and ACK/CREDIT, including the
center 3 -> 0 boundary. A result increments proof counters only after
`ipc_bridge_cpu1_display_visible()` accepts it.

## Commands

Run the offline checks first:

```powershell
& .\tools\ra8p1-cpu1-campaign.ps1 -SelfTest
python -m unittest tools.test_cpu1_campaign_control
```

Inspect commands without touching the target:

```powershell
& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourOverlap `
  -Iterations 10 -PayloadMbps 800 -Cpu1Elf C:\exact\CPU1.elf -DryRun -Json
```

Required campaign examples:

```powershell
# Repeat with 390, 500, 600, 700 and 800 Mbps.
& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourOverlap `
  -Iterations 1 -PayloadMbps 800 -Cpu1Elf C:\exact\CPU1.elf

& .\tools\ra8p1-cpu1-campaign.ps1 -Action Single -CenterIndex 0 `
  -Iterations 100 -PayloadMbps 800 -Cpu1Elf C:\exact\CPU1.elf

& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourOverlap `
  -Iterations 10 -PayloadMbps 800 -Cpu1Elf C:\exact\CPU1.elf
& .\tools\ra8p1-cpu1-campaign.ps1 -Action FourSerial `
  -Iterations 10 -PayloadMbps 800 -Cpu1Elf C:\exact\CPU1.elf

# Fault bits: CRC=1, DATA drop=2, first request timeout=4, first ACK timeout=8.
& .\tools\ra8p1-cpu1-campaign.ps1 -Action Single -CenterIndex 0 `
  -Iterations 1 -PayloadMbps 390 -FaultFlags 1 -Cpu1Elf C:\exact\CPU1.elf

& .\tools\ra8p1-cpu1-campaign.ps1 -Action ReadStatus `
  -Cpu1Elf C:\exact\CPU1.elf -Json
& .\tools\ra8p1-cpu1-campaign.ps1 -Action Stop `
  -Cpu1Elf C:\exact\CPU1.elf
```

The script reports the exact CPU1 ELF SHA-256 and symbol addresses. Addresses
are build artifacts, not constants; never interpret proof memory with another
ELF. SWD can perturb timing, so trigger before the measured interval and read
final proof after it completes.
