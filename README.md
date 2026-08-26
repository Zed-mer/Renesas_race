# RA8P1 Drone Detection

Dual-core firmware and host-side tooling for an RA8P1 SDR drone-detection
demonstrator. The target is the Renesas CPKHMI RA8P1 competition board
(`R7KA8P1KFLCAC`) with an external SDR data source.

This repository is a cleaned source snapshot of the Z6 workspace. It contains
the files needed to inspect, configure, build, and verify the CPU0/CPU1
Solution, but intentionally excludes local build products, firmware images,
captured IQ data, runtime logs, and historical delivery archives.

## Repository layout

```text
firmware/                 RA8P1 e2 studio Solution root
  cpu0/                   RT-Thread, SDR transport, inference, and control
  cpu1/                   LVGL user interface and display application
  shared/                 CPU0/CPU1 shared-memory ABI headers
  tools/                  Build, verification, SDR, and diagnostic utilities
  solution.xml            Dual-core Solution configuration
  build-solution.ps1      Reproducible dual-core build entry point
  verify-solution.ps1     Post-build structural and ABI checks
  flash-solution.ps1      Dual-core programming entry point
docs/                     Design notes and algorithm integration references
```

## Prerequisites

- Renesas e2 studio with FSP 6.4.0
- GCC Arm Embedded 13.2.Rel1 supplied with e2 studio
- CPKHMI RA8P1 competition board and SEGGER J-Link for hardware programming
- PowerShell 7 or Windows PowerShell

The checked-in project configuration is the source of truth. Generated FSP
content is retained as a reproducible project snapshot; put application changes
in `firmware/cpu0/src`, `firmware/cpu1/src`, or the project-owned shared and
tooling directories. Do not edit `ra/`, `ra_cfg/`, or `ra_gen/` by hand.

## Build and verify

From the repository root:

```powershell
Set-Location .\firmware
.\build-solution.ps1
.\verify-solution.ps1 -SkipBuild
```

The Solution must be built and programmed as a CPU0/CPU1 pair. Do not flash the
CPU1 ELF as an independent image. After a successful verified build, create a
versioned firmware release containing both core images and their SHA-256
manifest; publish that archive through GitHub Releases instead of committing it
to the source tree.

## Documentation

- `docs/reference/`: project snapshots, package notes, and panel configuration
  records.
- `docs/algorithm/`: RF detection and integration design notes.
- `firmware/INTEGRATION_GUIDE_CN.md`: integration guidance from the source
  workspace.
- `firmware/PROVENANCE.md`: source and build provenance record.

## Repository policy

Generated build output, ELF/HEX/BIN files, debugger logs, SDR deployment
binaries, captures, and experimental evidence are ignored by Git. Keep large
datasets and releases outside the main history, using GitHub Releases or Git
LFS only when a source-controlled large asset is genuinely required.

No license is declared yet. Until a license is selected after reviewing the
third-party dependencies, all rights are reserved by default.
