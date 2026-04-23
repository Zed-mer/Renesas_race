"""Parser for e2 Studio project metadata."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from lxml import etree


_WORKSPACE_LOC_RE = re.compile(r"\$\{workspace_loc:/([^}]+)\}")
_RA_DEVICE_RE = re.compile(r"#define\s+BSP_MCU_(R7FA[0-9A-Z]+)")
_RA_GROUP_RE = re.compile(r"#define\s+BSP_MCU_GROUP_([A-Z0-9]+)\s+\(1\)")


@dataclass
class ProjectInfo:
    name: str = ""
    eclipse_project_name: str = ""
    path: str = ""
    platform: str = ""
    device: str = ""
    device_family: str = ""
    toolchain: str = ""
    toolchain_version: str = ""
    configs: list[str] = field(default_factory=list)
    build_directory: str = ""
    has_map_file: bool = False
    last_build_time: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "eclipseProjectName": self.eclipse_project_name,
            "path": self.path,
            "platform": self.platform,
            "device": self.device,
            "deviceFamily": self.device_family,
            "toolchain": self.toolchain,
            "toolchainVersion": self.toolchain_version,
            "configs": self.configs,
            "buildDirectory": self.build_directory,
            "hasMapFile": self.has_map_file,
            "lastBuildTime": self.last_build_time,
        }


@dataclass
class ProjectConfig:
    name: str = ""
    eclipse_project_name: str = ""
    path: str = ""
    platform: str = ""
    device: str = ""
    device_family: str = ""
    isa: str = ""
    toolchain_id: str = ""
    toolchain_version: str = ""
    has_fpu: bool = False
    endian: str = ""
    build_config: str = ""
    build_directory: str = ""
    artifact_name: str = ""
    artifact_extension: str = ""
    configs: list[str] = field(default_factory=list)
    include_paths: list[str] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    compiler_options: dict[str, str] = field(default_factory=dict)
    linker_options: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "eclipseProjectName": self.eclipse_project_name,
            "path": self.path,
            "platform": self.platform,
            "device": self.device,
            "deviceFamily": self.device_family,
            "isa": self.isa,
            "toolchainId": self.toolchain_id,
            "toolchainVersion": self.toolchain_version,
            "hasFpu": self.has_fpu,
            "endian": self.endian,
            "buildConfig": self.build_config,
            "buildDirectory": self.build_directory,
            "artifactName": self.artifact_name,
            "artifactExtension": self.artifact_extension,
            "configs": self.configs,
            "includePaths": self.include_paths,
            "defines": self.defines,
            "compilerOptions": self.compiler_options,
            "linkerOptions": self.linker_options,
        }


@dataclass
class ResolvedProject:
    requested_name: str
    directory_name: str
    eclipse_project_name: str
    path: Path
    build_config: str
    build_directory: Path


def read_eclipse_project_name(project_path: Path) -> str:
    """Read the Eclipse project name from .project."""
    project_file = project_path / ".project"
    if not project_file.exists():
        return project_path.name

    try:
        tree = etree.parse(str(project_file))
        name = tree.findtext(".//name")
        return (name or project_path.name).strip()
    except Exception:
        return project_path.name


def find_project_path(workspace_path: Path, project: str | None = None) -> Path | None:
    """Resolve a project by directory name or Eclipse project name."""
    if not workspace_path.exists():
        return None

    requested = (project or "").strip()
    candidates: list[Path] = []

    if requested:
        direct = workspace_path / requested
        if direct.is_dir() and (direct / ".cproject").exists():
            return direct

    for entry in sorted(workspace_path.iterdir()):
        if not entry.is_dir() or not (entry / ".cproject").exists():
            continue
        candidates.append(entry)
        if requested and read_eclipse_project_name(entry) == requested:
            return entry

    if requested:
        return None
    return candidates[0] if candidates else None


def resolve_project(
    workspace_path: Path,
    project: str,
    build_config: str = "",
) -> ResolvedProject:
    """Resolve project path, Eclipse project name, and build directory."""
    project_path = find_project_path(workspace_path, project)
    if project_path is None:
        raise FileNotFoundError(f"Project not found in workspace: {project or '<default>'}")

    eclipse_name = read_eclipse_project_name(project_path)
    parsed = parse_cproject(project_path / ".cproject", build_config=build_config)
    selected_config = build_config or parsed.build_config or "Debug"

    build_dir = Path(parsed.build_directory) if parsed.build_directory else project_path / selected_config
    if not build_dir.is_absolute():
        build_dir = project_path / build_dir
    if not build_dir.exists():
        fallback = project_path / selected_config
        if fallback.exists():
            build_dir = fallback

    return ResolvedProject(
        requested_name=project or project_path.name,
        directory_name=project_path.name,
        eclipse_project_name=eclipse_name,
        path=project_path,
        build_config=selected_config,
        build_directory=build_dir,
    )


def _get_last_build_time(build_dir: Path) -> str:
    """Get the newest build artifact timestamp."""
    if not build_dir.exists():
        return ""

    patterns = ("*.elf", "*.axf", "*.mot", "*.hex", "*.bin", "*.srec", "*.map")
    files = [f for pattern in patterns for f in build_dir.glob(pattern)]
    if not files:
        return ""

    mtime = max(f.stat().st_mtime for f in files)
    return datetime.fromtimestamp(mtime).isoformat()


def _has_map_file(build_dir: Path) -> bool:
    return build_dir.exists() and any(build_dir.glob("*.map"))


def _find_map_file(build_dir: Path, preferred_names: list[str]) -> Path | None:
    if not build_dir.exists():
        return None

    map_files = list(build_dir.glob("*.map"))
    for name in preferred_names:
        for candidate in map_files:
            if candidate.stem == name:
                return candidate
    return map_files[0] if map_files else None


def _find_configurations(root: etree._Element) -> list[etree._Element]:
    return list(root.findall(".//storageModule[@moduleId='org.eclipse.cdt.core.settings']/cconfiguration"))


def _configuration_name(cconfig: etree._Element) -> str:
    settings = cconfig.find("storageModule[@moduleId='org.eclipse.cdt.core.settings']")
    if settings is not None:
        name = settings.get("name", "").strip()
        if name:
            return name

    build_system = cconfig.find("storageModule[@moduleId='cdtBuildSystem']/configuration")
    if build_system is not None:
        return build_system.get("name", "").strip()
    return ""


def _select_configuration(
    root: etree._Element,
    build_config: str,
) -> tuple[etree._Element | None, list[str]]:
    configs = _find_configurations(root)
    names = [_configuration_name(cfg) for cfg in configs if _configuration_name(cfg)]

    if build_config:
        for cfg in configs:
            if _configuration_name(cfg) == build_config:
                return cfg, names

    return (configs[0] if configs else None), names


def _resolve_workspace_value(value: str, project_path: Path) -> str:
    workspace_root = project_path.parent
    resolved = value.replace("${ProjName}", project_path.name)

    def repl(match: re.Match[str]) -> str:
        suffix = match.group(1).replace("/", "\\")
        return str(workspace_root / Path(suffix))

    resolved = _WORKSPACE_LOC_RE.sub(repl, resolved)
    resolved = resolved.replace("/", "\\")
    return resolved


def _detect_platform(project_path: Path, toolchain_id: str) -> str:
    lowered = toolchain_id.lower()
    if "gnuarm" in lowered or (project_path / "ra_cfg").exists() or (project_path / "configuration.xml").exists():
        return "ra"
    return "rx"


def _parse_ra_configuration(project_path: Path) -> dict[str, str]:
    config_path = project_path / "configuration.xml"
    if not config_path.exists():
        return {}

    try:
        tree = etree.parse(str(config_path))
    except Exception:
        return {}

    result: dict[str, str] = {}
    for option in tree.findall(".//option"):
        key = option.get("key", "").strip()
        value = option.get("value", "").strip()
        if key == "CPU":
            result["cpu"] = value
        elif key == "#TargetName#":
            result["target_name"] = value
        elif key == "#DeviceCommand#":
            result["device_command"] = value
    return result


def _parse_ra_family_headers(project_path: Path) -> tuple[str, str]:
    family = ""
    device = ""

    family_header = project_path / "ra_cfg" / "fsp_cfg" / "bsp" / "bsp_mcu_family_cfg.h"
    if family_header.exists():
        text = family_header.read_text(encoding="utf-8", errors="replace")
        match = _RA_GROUP_RE.search(text)
        if match:
            family = match.group(1)

    device_header = project_path / "ra_cfg" / "fsp_cfg" / "bsp" / "bsp_mcu_device_pn_cfg.h"
    if device_header.exists():
        text = device_header.read_text(encoding="utf-8", errors="replace")
        match = _RA_DEVICE_RE.search(text)
        if match:
            device = match.group(1)

    return family, device


def parse_cproject(
    cproject_path: Path | str,
    build_config: str = "",
) -> ProjectConfig:
    """Parse .cproject XML and extract the requested build configuration."""
    path = Path(cproject_path)
    project_path = path.parent
    tree = etree.parse(str(path))
    root = tree.getroot()

    selected, config_names = _select_configuration(root, build_config)
    config = ProjectConfig(
        name=project_path.name,
        eclipse_project_name=read_eclipse_project_name(project_path),
        path=str(project_path),
        configs=config_names,
    )

    if selected is None:
        return config

    config.build_config = _configuration_name(selected)

    build_system = selected.find("storageModule[@moduleId='cdtBuildSystem']/configuration")
    if build_system is not None:
        config.artifact_name = build_system.get("artifactName", "")
        config.artifact_extension = build_system.get("artifactExtension", "")
        builder = build_system.find(".//builder")
        if builder is not None:
            raw_build_path = builder.get("buildPath", "").strip()
            if raw_build_path:
                config.build_directory = _resolve_workspace_value(raw_build_path, project_path)

    tc_storage = selected.find("storageModule[@moduleId='com.renesas.cdt.managedbuild.core.toolchainInfo']")
    if tc_storage is not None:
        for opt in tc_storage.findall("option"):
            opt_id = opt.get("id", "")
            value = opt.get("value", "")
            if opt_id == "toolchain.id":
                config.toolchain_id = value
            elif opt_id == "toolchain.version":
                config.toolchain_version = value

    config.platform = _detect_platform(project_path, config.toolchain_id)
    _parse_common_options(selected, config)
    _parse_include_paths(selected, config)
    _parse_defines(selected, config)

    if config.platform == "ra":
        ra_meta = _parse_ra_configuration(project_path)
        family_from_header, device_from_header = _parse_ra_family_headers(project_path)

        if not config.device:
            config.device = ra_meta.get("device_command") or device_from_header or ra_meta.get("target_name", "")
        if not config.device_family:
            config.device_family = ra_meta.get("cpu") or family_from_header
        if not config.isa:
            config.isa = "Cortex-M"
        if not config.endian:
            config.endian = "little"
        if not config.artifact_extension:
            config.artifact_extension = "elf"

    return config


def _parse_common_options(cconfig: etree._Element, config: ProjectConfig) -> None:
    """Extract device, ISA, FPU, endian from build options."""
    for opt in cconfig.iter("option"):
        super_class = opt.get("superClass", "")
        value = opt.get("value", "")
        lowered = super_class.lower()
        lowered_value = value.lower()

        if ("devicecommand" in lowered or "devicename" in lowered) and value and not config.device:
            config.device = value
        elif "devicefamily" in lowered and value:
            config.device_family = value
        elif "common.option.isa" in lowered and "history" not in lowered:
            if "rxv2" in lowered_value:
                config.isa = "RXv2"
            elif "rxv3" in lowered_value:
                config.isa = "RXv3"
            elif "rxv1" in lowered_value:
                config.isa = "RXv1"
            else:
                config.isa = value
        elif "arm.target.family" in lowered and value:
            config.isa = value.rsplit(".", 1)[-1].replace("-", " ").title().replace("Cortex M", "Cortex-M")
        elif "hasfpu" in lowered:
            config.has_fpu = value.upper() == "TRUE"
        elif "arm.target.fpu.unit" in lowered and value:
            config.has_fpu = not lowered_value.endswith(".none")
        elif "dsp.option.endian" in lowered:
            if "big" in lowered_value:
                config.endian = "big"
            elif "little" in lowered_value:
                config.endian = "little"


def _parse_include_paths(cconfig: etree._Element, config: ProjectConfig) -> None:
    """Extract include paths from compiler options."""
    for opt in cconfig.iter("option"):
        super_class = opt.get("superClass", "").lower()
        if "include" not in super_class:
            continue
        for item in opt.findall("listOptionValue"):
            value = item.get("value", "").strip()
            if value:
                config.include_paths.append(value.strip('"'))


def _parse_defines(cconfig: etree._Element, config: ProjectConfig) -> None:
    """Extract preprocessor defines from compiler options."""
    for opt in cconfig.iter("option"):
        super_class = opt.get("superClass", "").lower()
        if "define" not in super_class and "defs" not in super_class:
            continue
        for item in opt.findall("listOptionValue"):
            value = item.get("value", "").strip()
            if value:
                config.defines.append(value)


def list_projects(workspace_path: Path) -> list[dict[str, Any]]:
    """Scan workspace for e2 Studio projects (directories with .cproject)."""
    if not workspace_path.exists():
        return []

    projects: list[ProjectInfo] = []
    for entry in sorted(workspace_path.iterdir()):
        if not entry.is_dir() or not (entry / ".cproject").exists():
            continue

        try:
            cfg = parse_cproject(entry / ".cproject")
            build_dir = Path(cfg.build_directory) if cfg.build_directory else entry / (cfg.build_config or "Debug")
            if not build_dir.is_absolute():
                build_dir = entry / build_dir
        except Exception:
            cfg = ProjectConfig(name=entry.name, eclipse_project_name=read_eclipse_project_name(entry))
            build_dir = entry / "Debug"

        info = ProjectInfo(
            name=entry.name,
            eclipse_project_name=cfg.eclipse_project_name or read_eclipse_project_name(entry),
            path=str(entry),
            platform=cfg.platform,
            device=cfg.device,
            device_family=cfg.device_family,
            toolchain=cfg.toolchain_id,
            toolchain_version=cfg.toolchain_version,
            configs=cfg.configs or ([cfg.build_config] if cfg.build_config else []),
            build_directory=str(build_dir),
            has_map_file=_has_map_file(build_dir),
            last_build_time=_get_last_build_time(build_dir),
        )
        projects.append(info)

    return [project.to_dict() for project in projects]


def get_project_config(
    workspace_path: Path,
    project: str,
    build_config: str = "HardwareDebug",
) -> dict[str, Any]:
    """Get full project configuration from .cproject and related RA metadata."""
    project_path = find_project_path(workspace_path, project)
    if project_path is None:
        return {"error": f"Project not found: {project}"}

    cproject_path = project_path / ".cproject"
    if not cproject_path.exists():
        return {"error": f".cproject not found at {cproject_path}"}

    try:
        cfg = parse_cproject(cproject_path, build_config=build_config)
        result = cfg.to_dict()

        preferred = [project_path.name]
        if cfg.eclipse_project_name and cfg.eclipse_project_name != project_path.name:
            preferred.append(cfg.eclipse_project_name)

        build_dir = Path(cfg.build_directory) if cfg.build_directory else project_path / (cfg.build_config or build_config)
        if not build_dir.is_absolute():
            build_dir = project_path / build_dir
        if not build_dir.exists():
            build_dir = project_path / (cfg.build_config or build_config)

        map_file = _find_map_file(build_dir, preferred)
        result["buildDirectory"] = str(build_dir)
        result["mapFile"] = str(map_file or "")
        result["hasMapFile"] = map_file is not None
        return result
    except Exception as exc:
        return {"error": f"Failed to parse .cproject: {exc}"}
