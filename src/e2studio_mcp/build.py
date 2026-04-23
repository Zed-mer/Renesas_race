"""Build management for e2 Studio projects — make and e2studioc backends."""

from __future__ import annotations

import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .config import Config
from . import project as project_mod


# ─── Error/Warning parsing ────────────────────────────────────

# CCRX compiler: "file.c", line 42: E0520: ...
RE_COMPILER_ERROR = re.compile(r'"(.+?)",\s*line\s+(\d+):\s+(E\d+):\s+(.+)')
RE_COMPILER_WARNING = re.compile(r'"(.+?)",\s*line\s+(\d+):\s+(W\d+):\s+(.+)')
# Linker fatal: F0553: ...
RE_LINKER_ERROR = re.compile(r'^\s*(F\d+):\s+(.+)', re.MULTILINE)
# Linker warning: W0561: ...
RE_LINKER_WARNING = re.compile(r'^\s*(W\d+):\s+(.+)', re.MULTILINE)
# GCC / GNU Arm: file:line:col: error|warning: ...
RE_GCC_ERROR = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+)(?::\d+)?:\s+(?P<kind>fatal error|error):\s+(?P<msg>.+)$",
    re.MULTILINE,
)
RE_GCC_WARNING = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+)(?::\d+)?:\s+warning:\s+(?P<msg>.+)$",
    re.MULTILINE,
)
RE_GNU_LINKER_ERROR = re.compile(r"^(?:collect2|ld(?:\.exe)?):\s+(.+)$", re.MULTILINE)
# Summary line: "3 Error(s), 1 Warning(s)"
RE_SUMMARY = re.compile(r'(\d+)\s+Error\(s\),\s+(\d+)\s+Warning\(s\)')


@dataclass
class BuildDiagnostic:
    file: str = ""
    line: int = 0
    code: str = ""
    message: str = ""
    severity: str = "error"  # "error" | "warning"

    def to_dict(self) -> dict[str, Any]:
        return {
            "file": self.file,
            "line": self.line,
            "code": self.code,
            "message": self.message,
            "severity": self.severity,
        }


@dataclass
class BuildResult:
    success: bool = False
    exit_code: int = -1
    duration_ms: int = 0
    errors: list[BuildDiagnostic] = field(default_factory=list)
    warnings: list[BuildDiagnostic] = field(default_factory=list)
    output: str = ""
    output_file: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "success": self.success,
            "exitCode": self.exit_code,
            "durationMs": self.duration_ms,
            "totalErrors": len(self.errors),
            "totalWarnings": len(self.warnings),
            "errors": [e.to_dict() for e in self.errors],
            "warnings": [w.to_dict() for w in self.warnings],
            "outputFile": self.output_file,
            "output": self.output[-4000:] if len(self.output) > 4000 else self.output,
        }


def parse_build_output(output: str) -> tuple[list[BuildDiagnostic], list[BuildDiagnostic]]:
    """Parse CCRX compiler/linker output for errors and warnings."""
    errors: list[BuildDiagnostic] = []
    warnings: list[BuildDiagnostic] = []

    for m in RE_COMPILER_ERROR.finditer(output):
        errors.append(BuildDiagnostic(
            file=m.group(1), line=int(m.group(2)),
            code=m.group(3), message=m.group(4), severity="error",
        ))

    for m in RE_COMPILER_WARNING.finditer(output):
        warnings.append(BuildDiagnostic(
            file=m.group(1), line=int(m.group(2)),
            code=m.group(3), message=m.group(4), severity="warning",
        ))

    for m in RE_LINKER_ERROR.finditer(output):
        errors.append(BuildDiagnostic(
            code=m.group(1), message=m.group(2), severity="error",
        ))

    for m in RE_LINKER_WARNING.finditer(output):
        # Avoid double-matching compiler warnings that also match W\d+
        code = m.group(1)
        msg = m.group(2)
        if not any(w.code == code and w.message == msg for w in warnings):
            warnings.append(BuildDiagnostic(
                code=code, message=msg, severity="warning",
            ))

    for match in RE_GCC_ERROR.finditer(output):
        errors.append(BuildDiagnostic(
            file=match.group("file"),
            line=int(match.group("line")),
            code="GCC",
            message=match.group("msg"),
            severity="error",
        ))

    for match in RE_GCC_WARNING.finditer(output):
        warnings.append(BuildDiagnostic(
            file=match.group("file"),
            line=int(match.group("line")),
            code="GCC",
            message=match.group("msg"),
            severity="warning",
        ))

    for match in RE_GNU_LINKER_ERROR.finditer(output):
        message = match.group(1)
        if not any(err.message == message for err in errors):
            errors.append(BuildDiagnostic(
                code="LD",
                message=message,
                severity="error",
            ))

    return errors, warnings


def _find_output_file(build_dir: Path, project_names: list[str]) -> str:
    """Find the most relevant build artifact in a build directory."""
    if not build_dir.exists():
        return ""

    extensions = (".elf", ".axf", ".mot", ".hex", ".bin", ".srec")
    preferred_names = [name for name in dict.fromkeys(project_names) if name]

    for name in preferred_names:
        for ext in extensions:
            candidate = build_dir / f"{name}{ext}"
            if candidate.exists():
                return str(candidate)

    for ext in extensions:
        matches = sorted(build_dir.glob(f"*{ext}"))
        if matches:
            return str(matches[0])
    return ""


def _find_busybox_bin(cfg: Config) -> str | None:
    """Find the Renesas busybox bin directory that provides sed/sh for makefiles."""
    if not cfg.toolchain.e2studio_path:
        return None

    plugins_dir = Path(cfg.toolchain.e2studio_path) / "plugins"
    if not plugins_dir.exists():
        return None

    for entry in sorted(plugins_dir.iterdir()):
        if not entry.is_dir():
            continue
        if not entry.name.startswith("com.renesas.ide.exttools.busybox.win32"):
            continue
        bin_dir = entry / "bin"
        if (bin_dir / "sed.exe").exists() and (bin_dir / "sh.exe").exists():
            return str(bin_dir)

    return None


def _find_ccrx_utilities_dir() -> str | None:
    """Find the user-scoped Renesas utility directory that provides renesas_cc_converter."""
    home_dir = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if not home_dir:
        return None

    eclipse_dir = Path(home_dir) / ".eclipse"
    if not eclipse_dir.exists():
        return None

    for entry in sorted(eclipse_dir.iterdir()):
        if not entry.is_dir():
            continue
        if not entry.name.startswith("com.renesas.platform_"):
            continue
        util_dir = entry / "Utilities" / "ccrx"
        if (util_dir / "renesas_cc_converter.exe").exists():
            return str(util_dir)

    return None


def _make_env(cfg: Config) -> dict[str, str]:
    """Build PATH expected by e2 Studio generated makefiles."""
    env = os.environ.copy()
    path_key = next((key for key in env if key.lower() == "path"), "PATH")
    current_path = env.get(path_key, "")
    entries: list[str] = []

    for candidate in [
        cfg.toolchain.ccrx_path,
        cfg.toolchain.make_path,
        _find_busybox_bin(cfg),
        _find_ccrx_utilities_dir(),
    ]:
        if candidate and Path(candidate).exists():
            entries.append(str(candidate))

    merged = entries + [part for part in current_path.split(os.pathsep) if part]
    env[path_key] = os.pathsep.join(dict.fromkeys(merged))
    return env


def _detect_build_jobs(cfg: Config) -> int:
    """Resolve configured parallel jobs. 0 means auto-detect from logical CPUs."""
    if cfg.build_jobs > 0:
        return cfg.build_jobs

    cpu_count = os.cpu_count() or 1
    return max(1, min(16, cpu_count))


def _make_args(build_dir: Path, target: str, cfg: Config) -> list[str]:
    """Build make arguments, enabling parallel compilation when configured."""
    args = ["-C", str(build_dir)]
    build_jobs = _detect_build_jobs(cfg)
    if target == "all" and build_jobs > 1:
        args.extend([f"-j{build_jobs}", "--output-sync=target"])
    args.append(target)
    return args


def _run_make(
    project_path: Path,
    build_dir: Path,
    target: str,
    make_cmd: str,
    cfg: Config,
    project_names: list[str],
) -> BuildResult:
    """Run build via make."""
    if not build_dir.exists():
        return BuildResult(
            success=False, output=f"Build directory not found: {build_dir}",
        )

    cmd = [make_cmd, *_make_args(build_dir, target, cfg)]
    t0 = time.monotonic()

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=str(project_path),
            env=_make_env(cfg),
        )
        duration = int((time.monotonic() - t0) * 1000)
        combined = proc.stdout + "\n" + proc.stderr
        errors, warnings = parse_build_output(combined)

        return BuildResult(
            success=proc.returncode == 0,
            exit_code=proc.returncode,
            duration_ms=duration,
            errors=errors,
            warnings=warnings,
            output=combined,
            output_file=_find_output_file(build_dir, project_names) if proc.returncode == 0 else "",
        )
    except subprocess.TimeoutExpired:
        duration = int((time.monotonic() - t0) * 1000)
        return BuildResult(
            success=False, duration_ms=duration,
            output="Build timed out after 300 seconds",
        )
    except FileNotFoundError:
        return BuildResult(
            success=False,
            output=f"make not found. Tried: {make_cmd}. Set toolchain.makePath in config.",
        )


def _run_e2studioc(
    project_path: Path,
    project_names: list[str],
    config: str,
    e2studioc: Path,
    workspace: Path,
    build_dir: Path,
    project_names_for_output: list[str],
    clean_build: bool = False,
) -> BuildResult:
    """Run build via e2studioc headless CLI."""
    if not e2studioc.exists():
        return BuildResult(
            success=False,
            output=f"e2studioc not found at: {e2studioc}",
        )

    action = "-cleanBuild" if clean_build else "-build"
    last_result = BuildResult(
        success=False,
        output="No e2studioc project name candidates were available.",
    )

    for project_name in dict.fromkeys(name for name in project_names if name):
        cmd = [
            str(e2studioc),
            "--launcher.suppressErrors",
            "-nosplash",
            "-application", "org.eclipse.cdt.managedbuilder.core.headlessbuild",
            "-data", str(workspace),
            action, f"{project_name}/{config}",
        ]

        t0 = time.monotonic()
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600,
            )
            duration = int((time.monotonic() - t0) * 1000)
            combined = proc.stdout + "\n" + proc.stderr
            errors, warnings = parse_build_output(combined)

            result = BuildResult(
                success=proc.returncode == 0,
                exit_code=proc.returncode,
                duration_ms=duration,
                errors=errors,
                warnings=warnings,
                output=f"[e2studioc project={project_name}]\n{combined}",
                output_file=_find_output_file(build_dir, project_names_for_output) if proc.returncode == 0 else "",
            )
            if result.success:
                return result
            last_result = result
        except subprocess.TimeoutExpired:
            duration = int((time.monotonic() - t0) * 1000)
            last_result = BuildResult(
                success=False,
                duration_ms=duration,
                output=f"e2studioc build timed out after 600 seconds for project '{project_name}'",
            )

    return last_result


# ─── Public API ───────────────────────────────────────────────

# Store last build output for get_build_status
_last_build_output: dict[str, BuildResult] = {}


def build_project(
    cfg: Config,
    project: str | None = None,
    config: str | None = None,
    mode: str | None = None,
) -> dict[str, Any]:
    """Build a project. Returns JSON-serializable result."""
    proj_name = project or cfg.default_project
    build_cfg = config or cfg.build_config
    build_mode = mode or cfg.build_mode
    try:
        resolved = project_mod.resolve_project(cfg.workspace_path, proj_name, build_cfg)
    except FileNotFoundError as exc:
        return {"success": False, "error": str(exc)}
    proj_path = resolved.path
    output_names = [resolved.directory_name, resolved.eclipse_project_name, proj_name]

    if build_mode == "e2studioc":
        result = _run_e2studioc(
            proj_path,
            [resolved.directory_name, resolved.eclipse_project_name, proj_name],
            build_cfg,
            cfg.get_e2studioc(), cfg.workspace_path,
            resolved.build_directory,
            output_names,
        )
    else:
        result = _run_make(proj_path, resolved.build_directory, "all", cfg.get_make(), cfg, output_names)

    _last_build_output[resolved.directory_name] = result
    _last_build_output[resolved.eclipse_project_name] = result
    return result.to_dict()


def clean_project(
    cfg: Config,
    project: str | None = None,
    config: str | None = None,
    mode: str | None = None,
) -> dict[str, Any]:
    """Clean a project build directory."""
    proj_name = project or cfg.default_project
    build_cfg = config or cfg.build_config
    build_mode = mode or cfg.build_mode
    try:
        resolved = project_mod.resolve_project(cfg.workspace_path, proj_name, build_cfg)
    except FileNotFoundError as exc:
        return {"success": False, "error": str(exc)}
    proj_path = resolved.path

    if build_mode == "e2studioc":
        result = _run_e2studioc(
            proj_path,
            [resolved.directory_name, resolved.eclipse_project_name, proj_name],
            build_cfg,
            cfg.get_e2studioc(), cfg.workspace_path,
            resolved.build_directory,
            [resolved.directory_name, resolved.eclipse_project_name, proj_name],
            clean_build=True,
        )
    else:
        result = _run_make(
            proj_path,
            resolved.build_directory,
            "clean",
            cfg.get_make(),
            cfg,
            [resolved.directory_name, resolved.eclipse_project_name, proj_name],
        )

    return result.to_dict()


def rebuild_project(
    cfg: Config,
    project: str | None = None,
    config: str | None = None,
    mode: str | None = None,
) -> dict[str, Any]:
    """Clean and rebuild a project."""
    clean_result = clean_project(cfg, project, config, mode)
    build_result = build_project(cfg, project, config, mode)

    # Merge: keep build result, prepend clean output
    build_result["cleanOutput"] = clean_result.get("output", "")
    build_result["cleanSuccess"] = clean_result.get("success", False)
    return build_result


def get_build_status(cfg: Config, project: str | None = None) -> dict[str, Any]:
    """Get status of last build (errors/warnings parsed from output)."""
    proj_name = project or cfg.default_project
    result = _last_build_output.get(proj_name)
    if result is None and proj_name:
        try:
            resolved = project_mod.resolve_project(cfg.workspace_path, proj_name, cfg.build_config)
            result = _last_build_output.get(resolved.directory_name) or _last_build_output.get(
                resolved.eclipse_project_name
            )
        except FileNotFoundError:
            result = None

    if result is None:
        return {
            "project": proj_name,
            "hasBuild": False,
            "message": "No build has been run yet for this project.",
        }

    return {
        "project": proj_name,
        "hasBuild": True,
        "success": result.success,
        "totalErrors": len(result.errors),
        "totalWarnings": len(result.warnings),
        "errors": [e.to_dict() for e in result.errors],
        "warnings": [w.to_dict() for w in result.warnings],
        "durationMs": result.duration_ms,
    }
