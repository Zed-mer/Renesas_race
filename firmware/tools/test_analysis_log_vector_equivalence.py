#!/usr/bin/env python3
"""Offline proof for the MVE log path and its scalar boundary fallback."""

from __future__ import annotations

from pathlib import Path

import numpy as np

from project_layout import resolve_cpu0


INPUT_SCALE = np.float32(7.8431377187e-3)
INPUT_ZERO_POINT = np.float32(-1.0)
POWER_SCALE = np.float32(2.0**-26)
POWER_EPSILON = np.float32(1.0e-12)
POWER_LOG_EPSILON = np.float32(-39.863136)
FALLBACK_MARGIN = np.float32(6.0e-5)
LOG2_POLY = np.asarray(
    [
        -2.295614848256274,
        -2.470711633419806,
        -5.686926051100417,
        -0.165253547131978,
        5.175912446351073,
        0.844006986174912,
        4.584458825456749,
        0.014127821926000,
    ],
    dtype=np.float32,
)


def fast_log2(values: np.ndarray) -> np.ndarray:
    values = np.asarray(values, dtype=np.float32)
    bits = values.view(np.uint32)
    exponent = (bits >> 23).astype(np.int32) - 127
    mantissa_bits = bits - (exponent.astype(np.uint32) << 23)
    mantissa = mantissa_bits.view(np.float32)
    xx = mantissa * mantissa
    a = LOG2_POLY[0] + mantissa * LOG2_POLY[4]
    b = LOG2_POLY[2] + mantissa * LOG2_POLY[6]
    c = LOG2_POLY[1] + mantissa * LOG2_POLY[5]
    d = LOG2_POLY[3] + mantissa * LOG2_POLY[7]
    a = a + b * xx
    c = c + d * xx
    xx = xx * xx
    a = a + c * xx
    a = a + exponent.astype(np.float32) * np.float32(0.693147180)
    return a * np.float32(1.4426950408889634)


def power_input(power: np.ndarray, divisor: np.ndarray) -> np.ndarray:
    return (
        power.astype(np.float32) / divisor.astype(np.float32) * POWER_SCALE
        + POWER_EPSILON
    )


def max_power_operands(power: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Return the old and new float operands supplied to the same target FMA."""
    power_u32 = np.asarray(power, dtype=np.uint32)
    old = power_u32.astype(np.uint64).astype(np.float32)
    old = old / np.float32(1.0)
    new = power_u32.astype(np.float32)
    return old.astype(np.float32), new.astype(np.float32)


def max_power_inputs(power: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Host model for the common scale-plus-epsilon operation before vlog."""
    old, new = max_power_operands(power)
    old = old * POWER_SCALE
    old = old + POWER_EPSILON
    new = new * POWER_SCALE
    new = new + POWER_EPSILON
    return old.astype(np.float32), new.astype(np.float32)


def exact_log2(power: np.ndarray, divisor: np.ndarray) -> np.ndarray:
    values = power_input(power, divisor).astype(np.float64)
    return np.log2(values).astype(np.float32)


def quantize(values: np.ndarray) -> np.ndarray:
    scaled = values / INPUT_SCALE + INPUT_ZERO_POINT
    rounded = np.where(scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5))
    return np.clip(rounded, -128, 127).astype(np.int32)


def near_boundary(values: np.ndarray) -> np.ndarray:
    scaled = values / INPUT_SCALE + INPUT_ZERO_POINT
    centre = quantize(values).astype(np.float32)
    boundary = centre + np.where(scaled >= centre, 0.5, -0.5)
    return np.abs(scaled - boundary) <= FALLBACK_MARGIN / INPUT_SCALE


def guarded_log(power: np.ndarray, divisor: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    approximate = fast_log2(power_input(power, divisor))
    fallback = near_boundary(approximate)
    result = approximate.copy()
    exact = exact_log2(power, divisor)
    result[fallback] = exact[fallback]
    return result, fallback


def check_max_path_equivalence(rng: np.random.Generator, count: int) -> None:
    powers_of_two = np.left_shift(
        np.uint64(1), np.arange(0, 32, dtype=np.uint64)
    )
    rounding_boundary = np.arange(
        (1 << 24) - 8, (1 << 24) + 9, dtype=np.uint64
    )
    physical_maximum = np.arange(
        (1 << 29) - 8, (1 << 29) + 9, dtype=np.uint64
    )
    uint32_maximum = np.arange(
        np.uint64(np.iinfo(np.uint32).max) - 8,
        np.uint64(np.iinfo(np.uint32).max) + 1,
        dtype=np.uint64,
    )
    zero_masks = np.asarray(
        [
            0 if mask & (1 << lane) else (1 << (lane + 20)) + mask
            for mask in range(16)
            for lane in range(4)
        ],
        dtype=np.uint64,
    )
    boundary = np.unique(
        np.concatenate(
            (
                np.asarray(
                    [0, 1, 2, 3, np.iinfo(np.uint32).max], dtype=np.uint64
                ),
                powers_of_two,
                np.maximum(powers_of_two, 1) - 1,
                np.minimum(powers_of_two + 1, np.iinfo(np.uint32).max),
                rounding_boundary,
                physical_maximum,
                uint32_maximum,
                zero_masks,
            )
        )
    ).astype(np.uint32)
    random_power = rng.integers(
        0, np.uint64(1) << np.uint64(32), size=count, dtype=np.uint64
    ).astype(np.uint32)
    maximum = np.concatenate((boundary, random_power))

    if POWER_SCALE.view(np.uint32) != np.uint32(0x32800000):
        raise AssertionError("power scale is no longer exactly 2^-26")
    old_operand, new_operand = max_power_operands(maximum)
    if not np.array_equal(old_operand.view(np.uint32), new_operand.view(np.uint32)):
        raise AssertionError("uint64/divisor-one and uint32 FMA operands differ")

    old_input, new_input = max_power_inputs(maximum)
    if not np.array_equal(old_input.view(np.uint32), new_input.view(np.uint32)):
        mismatch = int(
            np.flatnonzero(old_input.view(np.uint32) != new_input.view(np.uint32))[0]
        )
        raise AssertionError(
            "max-path vlog input mismatch at power %d: 0x%08X != 0x%08X"
            % (
                int(maximum[mismatch]),
                int(old_input.view(np.uint32)[mismatch]),
                int(new_input.view(np.uint32)[mismatch]),
            )
        )

    old_log = fast_log2(old_input)
    new_log = fast_log2(new_input)
    zero = maximum == 0
    old_log[zero] = POWER_LOG_EPSILON
    new_log[zero] = POWER_LOG_EPSILON
    if not np.array_equal(old_log.view(np.uint32), new_log.view(np.uint32)):
        raise AssertionError("specialized max-path log output is not bit-exact")

    old_fallback = near_boundary(old_log)
    new_fallback = near_boundary(new_log)
    if not np.array_equal(old_fallback, new_fallback):
        raise AssertionError("specialized max-path fallback decision changed")
    exact = exact_log2(
        maximum.astype(np.uint64), np.ones(maximum.size, dtype=np.uint32)
    )
    exact[zero] = POWER_LOG_EPSILON
    old_log[old_fallback] = exact[old_fallback]
    new_log[new_fallback] = exact[new_fallback]
    if not np.array_equal(quantize(old_log), quantize(new_log)):
        raise AssertionError("specialized max-path guarded quantization changed")

    # Exercise every four-lane zero pattern in the same lane order as the MVE
    # helper.  This catches a lane-mask bit shift or reversed load contract.
    for mask in range(16):
        lanes = np.asarray(
            [0 if mask & (1 << lane) else (1 << (lane + 20)) + mask
             for lane in range(4)],
            dtype=np.uint32,
        )
        old_lane_input, new_lane_input = max_power_inputs(lanes)
        old_lanes = fast_log2(old_lane_input)
        new_lanes = fast_log2(new_lane_input)
        zero = lanes == 0
        old_lanes[zero] = POWER_LOG_EPSILON
        new_lanes[zero] = POWER_LOG_EPSILON
        if not np.array_equal(old_lanes.view(np.uint32), new_lanes.view(np.uint32)):
            raise AssertionError(f"zero-lane mask mismatch for 0x{mask:02X}")


def check_max_path_source_contract() -> None:
    source = (
        resolve_cpu0(Path(__file__).resolve().parents[1])
        / "src"
        / "framework"
        / "analysis_pipeline.c"
    ).read_text(encoding="utf-8")
    required = (
        "analysis_log_power_max_mve4",
        "uint32_t max_powers[4]",
        "vcvtq_f32_u32(powers)",
        "vfmaq_n_f32",
        "analysis_log_power_max_mve4(max_powers, c1_values)",
    )
    for marker in required:
        if marker not in source:
            raise AssertionError(f"missing specialized max-path marker: {marker}")
    if "(const uint32_t[4]){1U, 1U, 1U, 1U}" in source:
        raise AssertionError("generic divisor-one max path returned")


def main() -> None:
    rng = np.random.default_rng(0x590336)
    count = 500_000
    power = rng.integers(0, 1 << 40, size=count, dtype=np.uint64)
    divisor = rng.integers(1, 10, size=count, dtype=np.uint32)

    check_max_path_source_contract()
    check_max_path_equivalence(rng, count)

    exact = exact_log2(power, divisor)
    approximate = fast_log2(power_input(power, divisor))
    error = np.abs(approximate.astype(np.float64) - exact.astype(np.float64))
    if float(error.max()) > 2.0e-5:
        raise AssertionError(f"MVE log error bound exceeded: {error.max()}")

    guarded, fallback = guarded_log(power, divisor)
    if not np.array_equal(quantize(guarded), quantize(exact)):
        raise AssertionError("guarded c0/c1 quantization mismatch")

    previous_power = rng.integers(0, 1 << 40, size=count, dtype=np.uint64)
    previous_divisor = rng.integers(1, 10, size=count, dtype=np.uint32)
    previous, previous_fallback = guarded_log(previous_power, previous_divisor)
    current = guarded
    exact_previous = exact_log2(previous_power, previous_divisor)
    exact_current = exact_log2(power, divisor)
    delta = np.maximum(current - previous, 0.0)
    exact_delta = np.maximum(exact_current - exact_previous, 0.0)
    delta_fallback = near_boundary(delta)
    guarded_delta = delta.copy()
    guarded_delta[delta_fallback] = exact_delta[delta_fallback]
    if not np.array_equal(quantize(guarded_delta), quantize(exact_delta)):
        raise AssertionError("guarded c2 quantization mismatch")

    total_fallbacks = int(fallback.sum() + previous_fallback.sum() + delta_fallback.sum())
    total_values = 3 * count
    if total_fallbacks >= total_values // 100:
        raise AssertionError(f"fallback rate unexpectedly high: {total_fallbacks}/{total_values}")
    print(
        "PASS: uint32 max path is bit-exact to uint64/divisor-one for boundaries "
        "and %d random values; MVE log2 max error %.3g; guarded c0/c1/c2 "
        "quantization is bit-exact; fallbacks %d/%d (%.4f%%)"
        % (count, float(error.max()), total_fallbacks, total_values,
           100.0 * total_fallbacks / total_values)
    )


if __name__ == "__main__":
    main()
