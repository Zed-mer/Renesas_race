# Codex RA Setup

This repository can now run as a standalone `stdio` MCP server for Renesas RA projects without the VS Code bridge.

## Validated Local Setup

- e2 Studio: `C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\eclipse`
- Workspace: `C:\Users\user\e2_studio\workspace`
- Example project directory: `ra6m5_eeg_imu_y2`
- Example launch file: `ra6m5_eeg_imu_y2 Debug.launch`
- Validated launch backend: `pyOCD`

## Required Environment

Set these before starting the MCP server:

```powershell
$env:E2MCP_PLATFORM = "ra"
$env:E2MCP_WORKSPACE = "C:\Users\user\e2_studio\workspace"
$env:E2MCP_PROJECT = "ra6m5_eeg_imu_y2"
$env:E2MCP_BUILD_CONFIG = "Debug"
$env:E2MCP_E2STUDIO_PATH = "C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\eclipse"
```

Optional overrides:

```powershell
$env:E2MCP_GCC_ARM_PATH = "C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\toolchains\gcc_arm\gcc-arm-none-eabi-10.3-2021.10\bin"
$env:E2MCP_RA_DEBUGCOMP_PATH = "C:\Users\user\.eclipse\com.renesas.platform_1016876100\DebugComp\RA"
$env:E2MCP_PYOCD_PATH = "C:\Users\user\AppData\Roaming\Python\Python311\Scripts"
```

## Start The Server

From the repository root:

```powershell
$env:PYTHONPATH = "src"
python -m e2studio_mcp.server
```

If you prefer an installed package:

```powershell
python -m pip install -e .
python -m e2studio_mcp.server
```

## Codex MCP Entry

If your Codex MCP UI asks for command, args, cwd, and env, use:

- command: `python`
- args: `["-m", "e2studio_mcp.server"]`
- cwd: `C:\Users\user\Desktop\E2MCP\e2studio-mcp`
- env:
  - `PYTHONPATH=src`
  - `E2MCP_PLATFORM=ra`
  - `E2MCP_WORKSPACE=C:\Users\user\e2_studio\workspace`
  - `E2MCP_PROJECT=ra6m5_eeg_imu_y2`
  - `E2MCP_BUILD_CONFIG=Debug`
  - `E2MCP_E2STUDIO_PATH=C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\eclipse`

## Available RA-Focused Tools

- `list_projects`
- `get_project_config`
- `build_project`
- `clean_project`
- `rebuild_project`
- `get_build_status`
- `get_build_artifacts`
- `get_build_size`
- `get_map_summary`
- `get_linker_sections`
- `flash_firmware`
- `debug_start`
- `debug_stop`
- `debug_status`

## Current Backend Status

- `pyOCD`: implemented and locally validated for launch parsing, ELF resolution, memory analysis, and command generation
- `J-Link`: launch-driven backend detection exists, direct automation is not implemented yet
- `E2 / E2 Lite`: launch-driven backend detection exists, direct automation is not implemented yet

## Notes

- RA artifact selection prefers the active `.launch` file's `PROGRAM_NAME`.
- RA memory analysis uses the generated `memory_regions.ld` and `arm-none-eabi-objdump -h`.
- Some local projects have mismatches between directory names and Eclipse project names in `.project`; the server now resolves both forms.
