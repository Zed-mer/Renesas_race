#!/usr/bin/env python3
"""Generate the checked-in C blob for the V27 Absolute Ethos-U model."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def bytes_literal(data: bytes) -> str:
    values = [f"0x{value:02x}" for value in data]
    lines = []
    for start in range(0, len(values), 12):
        lines.append("    " + ", ".join(values[start : start + 12]) + ",")
    return "\n".join(lines)


def scalar(array: np.ndarray) -> int:
    return int(np.asarray(array).reshape(-1)[0])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    package = np.load(args.model, allow_pickle=False)
    command = package["cmd_data"].tobytes()
    weights = package["weight_data"].tobytes()
    output_offsets = [int(value) for value in package["output_offset"].tolist()]
    output_regions = [int(value) for value in package["output_region"].tolist()]
    output_bytes = [
        int(np.prod(shape)) * int(elem_size)
        for shape, elem_size in zip(
            package["output_shape"].tolist(),
            package["output_elem_size"].tolist(),
        )
    ]

    if len(command) != 19768 or len(weights) != 16960:
        raise ValueError("unexpected V27 command/weight size")
    if output_offsets != [23680, 17760, 11840, 5920, 0]:
        raise ValueError(f"unexpected output offsets: {output_offsets}")
    if output_regions != [1, 1, 1, 1, 1] or output_bytes != [5916] * 5:
        raise ValueError("unexpected V27 output layout")

    text = """#include \"rf_v27_model_data.h\"\n\n#define RF_V27_ALIGN __attribute__((aligned(16)))\n\nstatic const uint8_t g_rf_v27_absolute_command[RF_V27_ABSOLUTE_COMMAND_BYTES]\n    RF_V27_ALIGN = {\n%s\n};\n\nstatic const uint8_t g_rf_v27_absolute_weights[RF_V27_ABSOLUTE_WEIGHT_BYTES]\n    RF_V27_ALIGN = {\n%s\n};\n\nconst rf_v27_model_blob_t g_rf_v27_absolute_model = {\n    .name = \"v27_absolute_dji_state\",\n    .command = g_rf_v27_absolute_command,\n    .weights = g_rf_v27_absolute_weights,\n    .command_bytes = RF_V27_ABSOLUTE_COMMAND_BYTES,\n    .weight_bytes = RF_V27_ABSOLUTE_WEIGHT_BYTES,\n    .weight_region = %du,\n    .scratch_region = %du,\n    .scratch_bytes = %du,\n    .input_region = %du,\n    .input_offset = RF_V27_ABSOLUTE_INPUT_OFFSET,\n    .input_bytes = RF_V27_ABSOLUTE_INPUT_BYTES,\n    .output_count = RF_V27_ABSOLUTE_OUTPUT_COUNT,\n    .output_region = {%s},\n    .output_offset = {%s},\n    .output_bytes = {%s},\n};\n""" % (
        bytes_literal(command),
        bytes_literal(weights),
        scalar(package["weight_region"]),
        scalar(package["scratch_region"]),
        scalar(package["scratch_size"]),
        scalar(package["input_region"]),
        ", ".join(f"{value}u" for value in output_regions),
        ", ".join(f"{value}u" for value in output_offsets),
        ", ".join(f"{value}u" for value in output_bytes),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
