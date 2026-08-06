from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping


CONFIG_SYMBOL = "g_rf_v25_activity_config"
RUNTIME_SYMBOLS = (
    "rf_v25_activity_fusion_init",
    "rf_v25_activity_fusion_apply_round",
    "rf_v25_activity_fusion_get",
    "rf_v25_activity_fusion_output_generation",
)
OBJECT_ORDER = ("DJI", "AT9S", "T12", "XIAOBAWANG")
PROFILE_Q12_FIELDS = (
    "on_hit_leak_q12",
    "on_miss_decay_q12",
    "weak_on_scale_q12",
    "on_enter_threshold_q12",
    "on_dual_enter_threshold_q12",
    "on_evidence_cap_q12",
    "off_miss_llr_q12",
    "off_exit_threshold_q12",
    "off_dual_exit_threshold_q12",
    "off_evidence_cap_q12",
    "weak_miss_scale_q12",
    "normal_off_decay_q12",
    "multi_hit_scale_q12",
    "multi_hit_bonus_cap_q12",
    "support_llr_q12",
    "strong_llr_q12",
)
PROFILE_U8_FIELDS = (
    "support_window_rounds",
    "single_support_rounds",
    "single_strong_rounds",
    "dual_support_rounds",
    "dual_strong_rounds",
    "uncertain_exit_rounds",
    "working_min_hold_rounds",
)
PROFILE_U16_FIELDS = ("exit_miss_rounds", "dual_exit_miss_rounds")
EVIDENCE_FIELDS = (
    "leak_q12",
    "minimum_q12",
    "maximum_q12",
    "working_enter_q12",
    "working_exit_q12",
    "miss_q12",
    "unknown_roi_scale_q12",
    "maximum_period_bonus_q12",
)
ELF_TYPE_NAMES = {1: "relocatable_object", 2: "executable", 3: "shared_object"}


@dataclass(frozen=True)
class Section:
    name: str
    section_type: int
    address: int
    offset: int
    size: int
    link: int
    entry_size: int


@dataclass(frozen=True)
class Symbol:
    name: str
    value: int
    size: int
    section_index: int


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(path)
    return value


def _cstring(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        return ""
    end = table.find(b"\0", offset)
    if end < 0:
        end = len(table)
    return table[offset:end].decode("ascii", errors="replace")


def _parse_elf(raw: bytes) -> tuple[int, list[Section], dict[str, Symbol]]:
    if len(raw) < 52 or raw[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    if raw[4] != 1:
        raise ValueError("V25 verifier requires ELF32")
    if raw[5] != 1:
        raise ValueError("V25 verifier requires little-endian ELF")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", raw, 0)
    elf_type = int(header[1])
    section_offset = int(header[6])
    section_entry_size = int(header[11])
    section_count = int(header[12])
    section_name_index = int(header[13])
    if section_entry_size < 40 or section_count == 0:
        raise ValueError("ELF section table is missing or malformed")
    unpacked = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        if offset + 40 > len(raw):
            raise ValueError("ELF section table exceeds file size")
        unpacked.append(struct.unpack_from("<IIIIIIIIII", raw, offset))
    if section_name_index >= len(unpacked):
        raise ValueError("invalid ELF section-name table index")
    names_header = unpacked[section_name_index]
    names = raw[names_header[4] : names_header[4] + names_header[5]]
    sections = [
        Section(
            name=_cstring(names, int(item[0])),
            section_type=int(item[1]),
            address=int(item[3]),
            offset=int(item[4]),
            size=int(item[5]),
            link=int(item[6]),
            entry_size=int(item[9]),
        )
        for item in unpacked
    ]
    symbols: dict[str, Symbol] = {}
    for section in sections:
        if section.section_type not in (2, 11) or section.entry_size < 16:
            continue
        if section.link >= len(sections):
            continue
        strings_section = sections[section.link]
        strings = raw[
            strings_section.offset : strings_section.offset + strings_section.size
        ]
        for offset in range(
            section.offset,
            section.offset + section.size,
            section.entry_size,
        ):
            if offset + 16 > len(raw):
                break
            name_offset, value, size, _, _, section_index = struct.unpack_from(
                "<IIIBBH", raw, offset
            )
            name = _cstring(strings, int(name_offset))
            if name:
                symbols[name] = Symbol(
                    name=name,
                    value=int(value),
                    size=int(size),
                    section_index=int(section_index),
                )
    return elf_type, sections, symbols


def _symbol_bytes(
    raw: bytes,
    sections: list[Section],
    symbol: Symbol,
) -> bytes:
    if symbol.section_index <= 0 or symbol.section_index >= len(sections):
        raise ValueError(f"symbol {symbol.name} has no concrete section")
    section = sections[symbol.section_index]
    relative = symbol.value - section.address
    if relative < 0 or relative + symbol.size > section.size:
        raise ValueError(f"symbol {symbol.name} exceeds its section")
    offset = section.offset + relative
    return raw[offset : offset + symbol.size]


def _q12(item: Mapping[str, Any], key: str) -> int:
    return int(item[key])


def _expected_config_bytes(config: Mapping[str, Any]) -> bytes:
    output = bytearray()
    evidence = config["evidence"]
    output.extend(
        struct.pack("<8i", *(_q12(evidence, name) for name in EVIDENCE_FIELDS))
    )
    for table in config["class_tables"]:
        bins = list(table["bins"])
        if len(bins) > 8:
            raise ValueError("class LLR table exceeds 8 bins")
        output.extend(struct.pack("<B3x", len(bins)))
        for item in bins:
            output.extend(
                struct.pack(
                    "<Hh",
                    round(float(item["minimum_score"]) * 32767),
                    round(float(item["llr"]) * 4096),
                )
            )
        output.extend(b"\0" * ((8 - len(bins)) * 4))
    for name in OBJECT_ORDER:
        profile = config["profiles"][name]
        output.extend(
            struct.pack(
                "<16i",
                *(_q12(profile, field) for field in PROFILE_Q12_FIELDS),
            )
        )
        output.extend(
            struct.pack(
                "<8B",
                *(int(profile[field]) for field in PROFILE_U8_FIELDS),
                0,
            )
        )
        output.extend(
            struct.pack(
                "<2H", *(int(profile[field]) for field in PROFILE_U16_FIELDS)
            )
        )
    output.extend(struct.pack("<i", int(config["t12_hop_bonus_q12"])))
    if len(output) != 520:
        raise AssertionError(f"encoded config is {len(output)} bytes, expected 520")
    return bytes(output)


def verify(
    binary: Path,
    config_path: Path,
    *,
    require_executable: bool,
    require_runtime_symbols: bool,
    arena_symbol: str | None,
    expected_arena_bytes: int,
) -> dict[str, Any]:
    binary = binary.resolve()
    config_path = config_path.resolve()
    raw = binary.read_bytes()
    elf_type, sections, symbols = _parse_elf(raw)
    errors: list[str] = []
    if require_executable and elf_type not in (2, 3):
        errors.append("binary is not a final executable/shared FSP ELF")
    config_symbol = symbols.get(CONFIG_SYMBOL)
    observed = b""
    if config_symbol is None:
        errors.append(f"missing symbol: {CONFIG_SYMBOL}")
    else:
        if config_symbol.size != 520:
            errors.append(
                f"{CONFIG_SYMBOL} size is {config_symbol.size}, expected 520"
            )
        observed = _symbol_bytes(raw, sections, config_symbol)
    expected = _expected_config_bytes(_json(config_path))
    mismatched_offsets = [
        index
        for index in range(min(len(observed), len(expected)))
        if observed[index] != expected[index]
    ]
    if len(observed) != len(expected):
        errors.append(
            f"config byte length is {len(observed)}, expected {len(expected)}"
        )
    elif mismatched_offsets:
        errors.append(
            "embedded V25 config differs from release config at byte offsets "
            + ",".join(str(value) for value in mismatched_offsets[:16])
        )
    missing_runtime = [name for name in RUNTIME_SYMBOLS if name not in symbols]
    if require_runtime_symbols and missing_runtime:
        errors.append("missing runtime symbols: " + ", ".join(missing_runtime))
    arena_result: dict[str, Any] | None = None
    if arena_symbol is not None:
        symbol = symbols.get(arena_symbol)
        if symbol is None:
            errors.append(f"missing arena symbol: {arena_symbol}")
        else:
            arena_result = {
                "symbol": arena_symbol,
                "observed_size_bytes": symbol.size,
                "expected_size_bytes": expected_arena_bytes,
                "passed": symbol.size == expected_arena_bytes,
            }
            if symbol.size != expected_arena_bytes:
                errors.append(
                    f"arena symbol size is {symbol.size}, expected {expected_arena_bytes}"
                )
    return {
        "schema_version": "25.0.0",
        "pipeline": "v25_final_elf_embedded_parameter_verification",
        "passed": not errors,
        "binary": str(binary),
        "binary_sha256": _sha256_bytes(raw),
        "elf_type": ELF_TYPE_NAMES.get(elf_type, str(elf_type)),
        "final_fsp_link_verified": elf_type in (2, 3) and not errors,
        "config": str(config_path),
        "config_json_sha256": _sha256_bytes(config_path.read_bytes()),
        "config_symbol": CONFIG_SYMBOL,
        "config_symbol_size": None if config_symbol is None else config_symbol.size,
        "expected_config_bytes_sha256": _sha256_bytes(expected),
        "observed_config_bytes_sha256": (
            None if not observed else _sha256_bytes(observed)
        ),
        "config_exact_byte_match": observed == expected,
        "mismatched_offsets": mismatched_offsets[:32],
        "runtime_symbols_required": require_runtime_symbols,
        "missing_runtime_symbols": missing_runtime,
        "arena": arena_result,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify V25 constants embedded in an ELF or object."
    )
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--config",
        type=Path,
        default=Path("output/ra8p1_v25_firmware/activity_v25_config.json"),
    )
    parser.add_argument("--require-executable", action="store_true")
    parser.add_argument("--require-runtime-symbols", action="store_true")
    parser.add_argument("--arena-symbol")
    parser.add_argument("--expected-arena-bytes", type=int, default=192176)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(
        args.binary,
        args.config,
        require_executable=args.require_executable,
        require_runtime_symbols=args.require_runtime_symbols,
        arena_symbol=args.arena_symbol,
        expected_arena_bytes=args.expected_arena_bytes,
    )
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
