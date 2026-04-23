"""RA-specific launch parsing, flashing, and ELF memory analysis."""

from __future__ import annotations

import re
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from xml.etree import ElementTree

from .config import Config
from .mapfile import MapSection, MapSummary
from . import project as project_mod


_MEMORY_ASSIGN_RE = re.compile(r"^([A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+);")
_OBJDUMP_SECTION_RE = re.compile(
    r"^\s*\d+\s+(?P<name>\S+)\s+(?P<size>[0-9A-Fa-f]+)\s+(?P<vma>[0-9A-Fa-f]+)\s+(?P<lma>[0-9A-Fa-f]+)"
)


@dataclass
class RALaunchConfig:
    launch_file: str = ""
    launch_type: str = ""
    backend: str = ""
    project_name: str = ""
    program_name: str = ""
    gdb_name: str = ""
    gdb_server_executable: str = ""
    gdb_server_other: str = ""
    gdb_server_target_name: str = ""
    gdb_server_board_id: str = ""
    gdb_port: int = 3333
    telnet_port: int = 4444
    bus_speed: int = 0
    do_continue: bool = False
    stop_at: str = ""


@dataclass
class RADebugSession:
    process: subprocess.Popen | None = None
    backend: str = ""
    project: str = ""
    port: int = 0
    command: list[str] | None = None
    launch_file: str = ""

    @property
    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None


_session: RADebugSession | None = None


def find_launch_file(project_path: Path, preferred_name: str = "") -> Path | None:
    """Find the most relevant RA launch file for a project."""
    launch_files = sorted(project_path.glob("*.launch"))
    if not launch_files:
        return None

    if preferred_name:
        for launch in launch_files:
            if launch.name == preferred_name:
                return launch

    preferred_stems = [
        project_path.name,
        project_mod.read_eclipse_project_name(project_path),
    ]
    for stem in preferred_stems:
        if not stem:
            continue
        for launch in launch_files:
            if launch.stem == stem or launch.stem.startswith(f"{stem} "):
                return launch

    for launch in launch_files:
        if "Debug" in launch.name:
            return launch
    return launch_files[0]


def parse_launch_file(launch_path: Path) -> RALaunchConfig:
    """Parse an RA launch file."""
    tree = ElementTree.parse(launch_path)
    root = tree.getroot()
    cfg = RALaunchConfig(
        launch_file=str(launch_path),
        launch_type=root.get("type", ""),
    )

    for elem in root:
        key = elem.get("key", "")
        value = elem.get("value", "")
        if key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerExecutable":
            cfg.gdb_server_executable = value
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerOther":
            cfg.gdb_server_other = value
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerTargetName":
            cfg.gdb_server_target_name = value
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerBoardId":
            cfg.gdb_server_board_id = value
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerGdbPortNumber":
            cfg.gdb_port = int(value or "3333")
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerTelnetPortNumber":
            cfg.telnet_port = int(value or "4444")
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerBusSpeed":
            cfg.bus_speed = int(value or "0")
        elif key == "ilg.gnumcueclipse.debug.gdbjtag.pyocd.doContinue":
            cfg.do_continue = value.lower() == "true"
        elif key == "org.eclipse.cdt.debug.gdbjtag.core.stopAt":
            cfg.stop_at = value
        elif key == "org.eclipse.cdt.launch.PROGRAM_NAME":
            cfg.program_name = value
        elif key == "org.eclipse.cdt.launch.PROJECT_ATTR":
            cfg.project_name = value
        elif key == "org.eclipse.cdt.dsf.gdb.DEBUG_NAME":
            cfg.gdb_name = value

    cfg.backend = _detect_backend(cfg)
    return cfg


def _detect_backend(cfg: RALaunchConfig) -> str:
    launch_type = cfg.launch_type.lower()
    if "pyocd" in launch_type:
        return "pyocd"
    if "jlink" in launch_type:
        return "jlink"
    if "e2lite" in launch_type or "renesas" in launch_type:
        return "renesas-ra"
    return "unknown"


def resolve_program_path(project_path: Path, launch_cfg: RALaunchConfig) -> Path | None:
    """Resolve the launch PROGRAM_NAME to an absolute file path."""
    if not launch_cfg.program_name:
        return None

    candidate = Path(launch_cfg.program_name)
    if not candidate.is_absolute():
        candidate = project_path / candidate
    if candidate.exists():
        return candidate
    return None


def resolve_launch(
    cfg: Config,
    project: str,
    build_config: str = "",
    launch_file: str = "",
) -> tuple[project_mod.ResolvedProject, RALaunchConfig]:
    resolved = project_mod.resolve_project(cfg.workspace_path, project, build_config)
    launch_path = find_launch_file(resolved.path, launch_file)
    if launch_path is None:
        raise FileNotFoundError(f"No .launch file found in {resolved.path}")
    launch_cfg = parse_launch_file(launch_path)
    return resolved, launch_cfg


def parse_memory_regions(memory_regions_path: Path) -> dict[str, tuple[int, int]]:
    """Parse generated memory_regions.ld into region -> (start, length)."""
    values: dict[str, int] = {}
    text = memory_regions_path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        match = _MEMORY_ASSIGN_RE.match(line.strip())
        if not match:
            continue
        key, raw = match.groups()
        values[key] = int(raw, 0)

    regions: dict[str, tuple[int, int]] = {}
    for name in [
        "FLASH",
        "RAM",
        "DATA_FLASH",
        "OPTION_SETTING",
        "OPTION_SETTING_S",
        "ID_CODE",
        "ITCM",
        "DTCM",
        "SDRAM",
        "QSPI_FLASH",
        "OSPI_DEVICE_0",
        "OSPI_DEVICE_1",
    ]:
        start_key = f"{name}_START"
        length_key = f"{name}_LENGTH"
        if start_key in values and length_key in values:
            regions[name] = (values[start_key], values[length_key])
    return regions


def _classify_region(address: int, regions: dict[str, tuple[int, int]]) -> str:
    for name in ("DATA_FLASH",):
        start, length = regions.get(name, (0, 0))
        if length and start <= address < start + length:
            return "DATA_FLASH"

    for name in ("RAM", "ITCM", "DTCM", "SDRAM"):
        start, length = regions.get(name, (0, 0))
        if length and start <= address < start + length:
            return "RAM"

    for name in ("FLASH", "OPTION_SETTING", "OPTION_SETTING_S", "ID_CODE", "QSPI_FLASH", "OSPI_DEVICE_0", "OSPI_DEVICE_1"):
        start, length = regions.get(name, (0, 0))
        if length and start <= address < start + length:
            return "ROM"

    return "OTHER"


def parse_objdump_sections(text: str, regions: dict[str, tuple[int, int]]) -> MapSummary:
    """Convert GNU objdump section headers into a MapSummary."""
    sections: list[MapSection] = []
    section_regions: list[tuple[str, str, int]] = []
    lines = text.splitlines()

    for index, line in enumerate(lines):
        match = _OBJDUMP_SECTION_RE.match(line)
        if not match:
            continue

        flags_line = lines[index + 1].strip() if index + 1 < len(lines) else ""
        flags = {flag.strip() for flag in flags_line.split(",") if flag.strip()}
        name = match.group("name")
        size = int(match.group("size"), 16)
        vma = int(match.group("vma"), 16)
        lma = int(match.group("lma"), 16)

        if size == 0:
            continue
        if "DEBUGGING" in flags or name.startswith(".debug") or name in {".comment", ".ARM.attributes", ".stab", ".stabstr"}:
            continue

        resident_region = _classify_region(vma, regions)
        load_region = _classify_region(lma, regions) if "LOAD" in flags else resident_region
        if resident_region == "OTHER" and load_region != "OTHER":
            resident_region = load_region

        sections.append(MapSection(
            name=name,
            start=vma,
            end=vma + size,
            size=size,
            align=0,
            region="ROM+RAM" if resident_region == "RAM" and load_region == "ROM" else resident_region,
        ))
        section_regions.append((resident_region, load_region, size))

    summary = MapSummary(
        sections=sections,
        rom_capacity=regions.get("FLASH", (0, 0))[1],
        ram_capacity=regions.get("RAM", (0, 0))[1],
        data_flash_capacity=regions.get("DATA_FLASH", (0, 0))[1],
    )

    for section, (resident_region, load_region, size) in zip(sections, section_regions):
        if resident_region == "RAM":
            summary.total_ram += size
        elif resident_region == "ROM":
            summary.total_rom += size
        elif resident_region == "DATA_FLASH":
            summary.total_data_flash += size

        if load_region == "ROM" and resident_region != "ROM":
            summary.total_rom += size
        elif load_region == "DATA_FLASH" and resident_region != "DATA_FLASH":
            summary.total_data_flash += size

    return summary


def analyze_project_memory(
    cfg: Config,
    project: str,
    build_config: str = "",
    launch_file: str = "",
) -> dict[str, Any]:
    """Analyze an RA ELF using objdump + memory_regions.ld."""
    resolved, launch_cfg = resolve_launch(cfg, project, build_config, launch_file)
    program = resolve_program_path(resolved.path, launch_cfg)
    if program is None:
        return {"error": f"Program not found for launch {launch_cfg.launch_file}"}

    memory_regions = resolved.build_directory / "memory_regions.ld"
    if not memory_regions.exists():
        candidate = program.parent / "memory_regions.ld"
        if candidate.exists():
            memory_regions = candidate
    if not memory_regions.exists():
        return {"error": f"memory_regions.ld not found in {resolved.build_directory}"}

    objdump = cfg.toolchain.get_gcc_tool("arm-none-eabi-objdump.exe")
    if not objdump.exists():
        return {"error": f"arm-none-eabi-objdump.exe not found at {objdump}"}

    proc = subprocess.run(
        [str(objdump), "-h", str(program)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        return {"error": proc.stderr.strip() or "objdump failed"}

    summary = parse_objdump_sections(proc.stdout, parse_memory_regions(memory_regions))
    return {
        "program": str(program),
        "memoryRegions": str(memory_regions),
        "summary": summary,
    }


def _tokenize_other_args(other: str) -> list[str]:
    if not other.strip():
        return []
    return shlex.split(other, posix=False)


def _pyocd_load_command(cfg: Config, launch_cfg: RALaunchConfig, image_path: Path) -> list[str]:
    pyocd = cfg.toolchain.get_pyocd()
    if pyocd is None:
        raise FileNotFoundError("pyocd.exe not found")

    cmd = [str(pyocd), "load"]
    extra = _tokenize_other_args(launch_cfg.gdb_server_other)
    cmd.extend(extra)

    if launch_cfg.gdb_server_board_id and not any(arg in {"-u", "--uid", "--probe"} for arg in extra):
        cmd.extend(["-u", launch_cfg.gdb_server_board_id])
    if launch_cfg.gdb_server_target_name and not any(arg in {"-t", "--target"} for arg in extra):
        cmd.extend(["-t", launch_cfg.gdb_server_target_name])
    if launch_cfg.bus_speed > 0 and not any(arg in {"-f", "--frequency"} for arg in extra):
        cmd.extend(["-f", str(launch_cfg.bus_speed)])

    cmd.append(str(image_path))
    return cmd


def flash_firmware(
    cfg: Config,
    project: str = "",
    file: str = "",
    build_config: str = "",
    launch_file: str = "",
) -> dict[str, Any]:
    """Flash an RA project using the backend described by its .launch file."""
    resolved, launch_cfg = resolve_launch(cfg, project or cfg.default_project, build_config, launch_file)

    image_path: Path | None = None
    if file:
        candidate = Path(file)
        if candidate.exists():
            image_path = candidate
    if image_path is None:
        image_path = resolve_program_path(resolved.path, launch_cfg)

    if image_path is None or not image_path.exists():
        return {"success": False, "error": "No flashable image was found for the selected launch configuration."}

    if launch_cfg.backend != "pyocd":
        return {
            "success": False,
            "backend": launch_cfg.backend,
            "error": f"Launch backend '{launch_cfg.backend}' is not implemented yet.",
            "launchFile": launch_cfg.launch_file,
        }

    cmd = _pyocd_load_command(cfg, launch_cfg, image_path)
    t0 = time.monotonic()
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=300,
        cwd=str(resolved.path),
    )
    duration_ms = int((time.monotonic() - t0) * 1000)
    combined = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")

    return {
        "success": proc.returncode == 0,
        "backend": launch_cfg.backend,
        "command": cmd,
        "durationMs": duration_ms,
        "project": resolved.directory_name,
        "launchFile": launch_cfg.launch_file,
        "flashedFile": str(image_path),
        "output": combined[-4000:],
    }


def _pyocd_gdbserver_command(cfg: Config, launch_cfg: RALaunchConfig, program: Path | None) -> list[str]:
    executable = cfg.toolchain.get_pyocd_gdbserver()
    if executable is None:
        raise FileNotFoundError("pyocd-gdbserver.exe not found")

    cmd = [str(executable), "-p", str(launch_cfg.gdb_port), "-T", str(launch_cfg.telnet_port)]
    extra = _tokenize_other_args(launch_cfg.gdb_server_other)
    cmd.extend(extra)

    if launch_cfg.gdb_server_board_id and not any(arg in {"-b", "--board"} for arg in extra):
        cmd.extend(["-b", launch_cfg.gdb_server_board_id])
    if launch_cfg.gdb_server_target_name and not any(arg in {"-t", "--target"} for arg in extra):
        cmd.extend(["-t", launch_cfg.gdb_server_target_name])
    if launch_cfg.bus_speed > 0 and not any(arg in {"-f", "--frequency"} for arg in extra):
        cmd.extend(["-f", str(launch_cfg.bus_speed)])
    if program is not None:
        cmd.extend(["--elf", str(program)])
    return cmd


def debug_connect(
    cfg: Config,
    project: str = "",
    build_config: str = "",
    launch_file: str = "",
) -> dict[str, Any]:
    """Start the backend debug server described by an RA launch file."""
    global _session

    if _session is not None and _session.running:
        return {
            "connected": True,
            "backend": _session.backend,
            "project": _session.project,
            "port": _session.port,
            "message": "Debug backend already running.",
        }

    resolved, launch_cfg = resolve_launch(cfg, project or cfg.default_project, build_config, launch_file)
    if launch_cfg.backend != "pyocd":
        return {
            "connected": False,
            "backend": launch_cfg.backend,
            "error": f"Launch backend '{launch_cfg.backend}' is not implemented yet.",
            "launchFile": launch_cfg.launch_file,
        }

    program = resolve_program_path(resolved.path, launch_cfg)
    cmd = _pyocd_gdbserver_command(cfg, launch_cfg, program)

    process = subprocess.Popen(
        cmd,
        cwd=str(resolved.path),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(2)
    if process.poll() is not None:
        return {
            "connected": False,
            "backend": launch_cfg.backend,
            "error": f"pyocd-gdbserver exited with rc={process.returncode}",
            "command": cmd,
        }

    _session = RADebugSession(
        process=process,
        backend=launch_cfg.backend,
        project=resolved.directory_name,
        port=launch_cfg.gdb_port,
        command=cmd,
        launch_file=launch_cfg.launch_file,
    )
    return {
        "connected": True,
        "backend": launch_cfg.backend,
        "project": resolved.directory_name,
        "launchFile": launch_cfg.launch_file,
        "port": launch_cfg.gdb_port,
        "command": cmd,
        "program": str(program) if program else "",
    }


def debug_disconnect() -> dict[str, Any]:
    """Stop the active RA debug backend process."""
    global _session

    if _session is None:
        return {"disconnected": True, "message": "No active RA debug session."}

    if _session.running and _session.process is not None:
        _session.process.terminate()
        try:
            _session.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _session.process.kill()

    result = {
        "disconnected": True,
        "backend": _session.backend,
        "project": _session.project,
        "launchFile": _session.launch_file,
    }
    _session = None
    return result


def debug_status() -> dict[str, Any]:
    """Report the current RA debug backend status."""
    if _session is None:
        return {"serverRunning": False, "backend": "", "gdbConnected": False}

    if not _session.running:
        return {"serverRunning": False, "backend": _session.backend, "gdbConnected": False}

    return {
        "serverRunning": True,
        "backend": _session.backend,
        "gdbConnected": True,
        "project": _session.project,
        "port": _session.port,
        "launchFile": _session.launch_file,
        "command": _session.command or [],
    }
