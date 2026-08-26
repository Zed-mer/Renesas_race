"""Resolve CPU project directories in a source tree or a share package."""

from __future__ import annotations

from pathlib import Path


_PROJECT_NAMES = {
    "cpu0": ("cpu0", "ra8p1_sdr_stft_npu_display_solution_20260719_CPU0"),
    "cpu1": ("cpu1", "ra8p1_sdr_stft_npu_display_solution_20260719_CPU1"),
}


def resolve_project(root: Path, core: str) -> Path:
    """Return the packaged or legacy child-project directory for *core*."""
    try:
        candidates = _PROJECT_NAMES[core.lower()]
    except KeyError as exc:
        raise ValueError(f"unsupported core: {core}") from exc

    for name in candidates:
        candidate = root / name
        if candidate.is_dir():
            return candidate

    expected = ", ".join(str(root / name) for name in candidates)
    raise FileNotFoundError(f"{core.upper()} project directory not found; tried: {expected}")


def resolve_cpu0(root: Path) -> Path:
    return resolve_project(root, "cpu0")


def resolve_cpu1(root: Path) -> Path:
    return resolve_project(root, "cpu1")
