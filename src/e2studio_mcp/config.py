"""Configuration loader for e2studio-mcp.

Settings come from environment variables plus local auto-detection.
The MCP server now supports both Renesas RX and RA workflows.

Environment variables:
  E2MCP_PLATFORM           - "rx", "ra", or "auto"
  E2MCP_WORKSPACE          - Root folder containing e2 Studio projects
  E2MCP_PROJECT            - Default project directory or Eclipse project name
  E2MCP_BUILD_CONFIG       - Build configuration (default: HardwareDebug)
  E2MCP_BUILD_MODE         - Build backend: make or e2studioc (default: make)
  E2MCP_BUILD_JOBS         - Parallel build jobs, 0 = auto (default: 0)
  E2MCP_E2STUDIO_PATH      - Path to the e2 Studio eclipse folder
  E2MCP_CCRX_PATH          - Path to the CCRX compiler bin folder
  E2MCP_GCC_ARM_PATH       - Path to the GNU Arm Embedded bin folder
  E2MCP_MAKE_PATH          - Path to the GNU make folder
  E2MCP_RA_DEBUGCOMP_PATH  - Path to ~/.eclipse/.../DebugComp/RA
  E2MCP_PYOCD_PATH         - Path to pyocd.exe or its Scripts directory
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ─── Known Renesas RX devices ────────────────────────────────

_KNOWN_RX_DEVICES: dict[str, dict[str, Any]] = {
    "R5F5651E": {"family": "RX651", "romSize": 2097152, "ramSize": 655360, "dataFlashSize": 32768},
    "R5F565NE": {"family": "RX65N", "romSize": 2097152, "ramSize": 655360, "dataFlashSize": 32768},
    "R5F572NNDxBD": {"family": "RX72N", "romSize": 4194304, "ramSize": 1048576, "dataFlashSize": 32768},
}


@dataclass
class ToolchainConfig:
    ccrx_path: str = ""
    gcc_arm_path: str = ""
    e2studio_path: str = ""
    make_path: str | None = None
    ra_debug_comp_path: str = ""
    pyocd_path: str = ""

    def get_gcc_tool(self, tool: str) -> Path:
        return Path(self.gcc_arm_path) / tool

    def get_pyocd(self) -> Path | None:
        return _resolve_program(self.pyocd_path, "pyocd.exe")

    def get_pyocd_gdbserver(self) -> Path | None:
        return _resolve_program(self.pyocd_path, "pyocd-gdbserver.exe")


@dataclass
class DeviceInfo:
    family: str = ""
    rom_size: int = 0
    ram_size: int = 0
    data_flash_size: int = 0


@dataclass
class Config:
    workspace: str = ""
    default_project: str = ""
    build_config: str = "HardwareDebug"
    build_mode: str = "make"
    build_jobs: int = 0
    platform: str = "auto"
    toolchain: ToolchainConfig = field(default_factory=ToolchainConfig)

    @property
    def workspace_path(self) -> Path:
        return Path(self.workspace)

    @property
    def platform_name(self) -> str:
        return self.platform.lower().strip() or "auto"

    @property
    def is_ra(self) -> bool:
        return self.platform_name == "ra"

    @property
    def is_rx(self) -> bool:
        return self.platform_name == "rx"

    def get_project_path(self, project: str | None = None) -> Path:
        name = project or self.default_project
        return self.workspace_path / name

    def get_device_info(self, device: str | None = None) -> DeviceInfo | None:
        """Look up RX device info from the built-in table."""
        dev = device or ""
        if dev and dev in _KNOWN_RX_DEVICES:
            return _parse_device_entry(_KNOWN_RX_DEVICES[dev])
        for info in _KNOWN_RX_DEVICES.values():
            return _parse_device_entry(info)
        return None

    def get_ccrx_bin(self, tool: str) -> Path:
        return Path(self.toolchain.ccrx_path) / tool

    def get_e2studio_exe(self) -> Path:
        return Path(self.toolchain.e2studio_path) / "e2studio.exe"

    def get_e2studioc(self) -> Path:
        return Path(self.toolchain.e2studio_path) / "e2studioc.exe"

    def get_make(self) -> str:
        if self.toolchain.make_path:
            return str(Path(self.toolchain.make_path) / "make")
        return "make"


# ─── Auto-detection ──────────────────────────────────────────

def _candidate_e2studio_paths() -> list[Path]:
    candidates = [
        Path("C:/Renesas/e2_studio/eclipse"),
        Path("C:/Renesas/e2studio/eclipse"),
    ]

    ra_root = Path("C:/Renesas/RA")
    if ra_root.exists():
        candidates.extend(sorted(ra_root.glob("*/eclipse"), reverse=True))

    return candidates


def _detect_e2studio_path() -> str:
    """Auto-detect the e2 Studio eclipse folder."""
    for candidate in _candidate_e2studio_paths():
        if (candidate / "e2studioc.exe").exists():
            return str(candidate)
    return ""


def _detect_ccrx_path() -> str:
    """Auto-detect CCRX compiler: newest version under Program Files (x86)/Renesas/RX."""
    base = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Renesas" / "RX"
    try:
        for ver in sorted(base.iterdir(), reverse=True):
            bin_dir = ver / "bin"
            if (bin_dir / "ccrx.exe").exists():
                return str(bin_dir)
    except (FileNotFoundError, PermissionError):
        pass
    return ""


def _detect_gcc_arm_path(e2studio_path: str) -> str:
    """Auto-detect GNU Arm Embedded bin folder from RA installs."""
    search_roots: list[Path] = []
    if e2studio_path:
        e2_root = Path(e2studio_path)
        search_roots.append(e2_root.parent)

    ra_root = Path("C:/Renesas/RA")
    if ra_root.exists():
        search_roots.extend(sorted(ra_root.glob("*"), reverse=True))

    seen: set[Path] = set()
    for root in search_roots:
        if root in seen or not root.exists():
            continue
        seen.add(root)
        toolchains = root / "toolchains" / "gcc_arm"
        if not toolchains.exists():
            continue
        for candidate in sorted(toolchains.glob("*/bin"), reverse=True):
            if (candidate / "arm-none-eabi-gcc.exe").exists():
                return str(candidate)
    return ""


def _detect_make_path(e2studio_path: str) -> str:
    """Auto-detect GNU make bundled with e2 Studio plugins."""
    if not e2studio_path:
        return ""
    plugins = Path(e2studio_path) / "plugins"
    try:
        for entry in sorted(plugins.iterdir(), reverse=True):
            if entry.name.startswith("com.renesas.ide.exttools.gnumake") and entry.is_dir():
                mk = entry / "mk"
                if (mk / "make.exe").exists():
                    return str(mk)
    except (FileNotFoundError, PermissionError):
        pass
    return ""


def _detect_ra_debug_comp_path() -> str:
    """Auto-detect ~/.eclipse/.../DebugComp/RA."""
    eclipse_dir = Path.home() / ".eclipse"
    try:
        for entry in sorted(eclipse_dir.iterdir(), reverse=True):
            candidate = entry / "DebugComp" / "RA"
            if entry.is_dir() and entry.name.startswith("com.renesas.platform_") and candidate.exists():
                return str(candidate)
    except (FileNotFoundError, PermissionError):
        pass
    return ""


def _detect_pyocd_path() -> str:
    """Auto-detect pyocd.exe from the user's Python Scripts directories or PATH."""
    direct = shutil.which("pyocd")
    if direct:
        return str(Path(direct).parent)

    appdata = Path(os.environ.get("APPDATA", ""))
    if appdata:
        python_root = appdata / "Python"
        if python_root.exists():
            for scripts_dir in sorted(python_root.glob("Python*/Scripts"), reverse=True):
                if (scripts_dir / "pyocd.exe").exists():
                    return str(scripts_dir)
    return ""


def _auto_detect_toolchain(tc: ToolchainConfig) -> ToolchainConfig:
    """Fill in missing toolchain paths via auto-detection."""
    e2 = tc.e2studio_path or _detect_e2studio_path()
    ccrx = tc.ccrx_path or _detect_ccrx_path()
    gcc_arm = tc.gcc_arm_path or _detect_gcc_arm_path(e2)
    make = tc.make_path or _detect_make_path(e2) or None
    ra_debug = tc.ra_debug_comp_path or _detect_ra_debug_comp_path()
    pyocd_path = tc.pyocd_path or _detect_pyocd_path()
    return ToolchainConfig(
        ccrx_path=ccrx,
        gcc_arm_path=gcc_arm,
        e2studio_path=e2,
        make_path=make,
        ra_debug_comp_path=ra_debug,
        pyocd_path=pyocd_path,
    )


# ─── Parsing helpers ─────────────────────────────────────────

def _parse_device_entry(info: dict[str, Any]) -> DeviceInfo:
    return DeviceInfo(
        family=info.get("family", ""),
        rom_size=info.get("romSize", 0),
        ram_size=info.get("ramSize", 0),
        data_flash_size=info.get("dataFlashSize", 0),
    )


def _resolve_program(configured_path: str, program_name: str) -> Path | None:
    path = Path(configured_path) if configured_path else None
    if path:
        if path.is_file():
            return path
        candidate = path / program_name
        if candidate.exists():
            return candidate

    discovered = shutil.which(program_name)
    if discovered:
        return Path(discovered)
    return None


def _detect_platform(tc: ToolchainConfig, requested: str) -> str:
    explicit = requested.lower().strip()
    if explicit in {"rx", "ra"}:
        return explicit

    e2_path = tc.e2studio_path.replace("\\", "/").lower()
    if "/renesas/ra/" in e2_path or tc.gcc_arm_path or tc.ra_debug_comp_path:
        return "ra"
    if tc.ccrx_path:
        return "rx"
    return "auto"


def load_config() -> Config:
    """Load configuration from environment variables + auto-detection."""
    tc = _auto_detect_toolchain(ToolchainConfig(
        ccrx_path=os.environ.get("E2MCP_CCRX_PATH", ""),
        gcc_arm_path=os.environ.get("E2MCP_GCC_ARM_PATH", ""),
        e2studio_path=os.environ.get("E2MCP_E2STUDIO_PATH", ""),
        make_path=os.environ.get("E2MCP_MAKE_PATH", "") or None,
        ra_debug_comp_path=os.environ.get("E2MCP_RA_DEBUGCOMP_PATH", ""),
        pyocd_path=os.environ.get("E2MCP_PYOCD_PATH", ""),
    ))

    requested_platform = os.environ.get("E2MCP_PLATFORM", "auto")
    platform = _detect_platform(tc, requested_platform)
    configured_build_config = os.environ.get("E2MCP_BUILD_CONFIG", "").strip()
    default_build_config = configured_build_config or ("Debug" if platform == "ra" else "HardwareDebug")

    return Config(
        workspace=os.environ.get("E2MCP_WORKSPACE", ""),
        default_project=os.environ.get("E2MCP_PROJECT", ""),
        build_config=default_build_config,
        build_mode=os.environ.get("E2MCP_BUILD_MODE", "make"),
        build_jobs=max(0, int(os.environ.get("E2MCP_BUILD_JOBS", "0"))),
        platform=platform,
        toolchain=tc,
    )
