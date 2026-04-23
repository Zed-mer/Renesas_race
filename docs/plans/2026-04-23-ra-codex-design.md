# RA Codex Design

## Goal

Adapt the repository so Codex can use it as a standalone `stdio` MCP server for Renesas RA development with the local e2 Studio install at `C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\eclipse`.

## Decisions

- Keep the existing RX implementation intact where practical.
- Add RA support as a parallel path instead of rewriting the whole codebase around a single abstraction.
- Make Codex usage independent from the VS Code bridge.
- Drive RA flashing from `.launch` files so the MCP server follows the same project-level debug intent as e2 Studio.
- Validate first against the real local `pyOCD` launches present in `C:\Users\user\e2_studio\workspace`.

## Scope

Included in this change:

- RA platform detection and toolchain discovery
- RA project parsing from `.cproject`, `.project`, and `configuration.xml`
- GCC/GNU Arm build diagnostic parsing
- RA artifact discovery with `.launch`-first ELF resolution
- RA memory analysis from `memory_regions.ld` and `arm-none-eabi-objdump`
- RA `flash_firmware`, `debug_start`, `debug_stop`, and `debug_status` for `pyOCD`
- Codex setup documentation

Explicitly not completed in this change:

- Direct `J-Link` automation
- Direct `E2 / E2 Lite` automation
- RA ADM / virtual console capture
- Full interactive GDB orchestration inside MCP

## Architecture

### Configuration

- `config.py` now detects both RX and RA installs.
- Added environment/config support for:
  - `E2MCP_PLATFORM`
  - `E2MCP_GCC_ARM_PATH`
  - `E2MCP_RA_DEBUGCOMP_PATH`
  - `E2MCP_PYOCD_PATH`

### Project Resolution

- `project.py` now resolves projects by either directory name or Eclipse project name.
- RA device and family metadata falls back to `configuration.xml` and generated FSP headers.
- Build directory resolution keeps the parsed `buildPath` but falls back to the project-local configuration directory when needed.

### Build

- `build.py` still supports `make` and `e2studioc`.
- GNU Arm diagnostics are parsed alongside existing CCRX diagnostics.
- Output artifact selection no longer assumes `.mot`; it now prefers `.elf`, `.axf`, `.mot`, `.hex`, `.bin`, and `.srec`.

### RA Runtime

- `ra.py` owns:
  - RA launch parsing
  - `pyOCD` command generation
  - debug backend process lifecycle
  - ELF memory analysis

### MCP Surface

- `server.py` routes RA projects to the RA runtime.
- Added `get_build_artifacts`.
- Added standalone `flash_firmware` for RA.
- Existing debug tools now use the RA backend when `platform=ra`.

## Validation

Validated locally against:

- Workspace: `C:\Users\user\e2_studio\workspace`
- Project directory: `ra6m5_eeg_imu_y2`
- Launch file: `ra6m5_eeg_imu_y2 Debug.launch`
- Backend: `pyOCD`
- Program resolved from launch: `C:\Users\user\e2_studio\workspace\ra6m5_eeg_imu_y2\Debug\ra6m5_eeg_imu_y2.elf`

Non-hardware validation completed:

- project resolution
- launch parsing
- ELF memory analysis
- `pyOCD` flash command generation
- unit tests for RX and RA parsing paths
