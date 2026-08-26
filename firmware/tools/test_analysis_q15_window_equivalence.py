#!/usr/bin/env python3
"""Check scalar Q15 Hann multiplication against Arm MVE VQDMULH semantics."""

from __future__ import annotations

import numpy as np


def scalar_q15(samples: np.ndarray, coefficient: int) -> np.ndarray:
    product = samples.astype(np.int64) * np.int64(coefficient)
    return (product >> np.int64(15)).astype(np.int16)


def vqdmulh_q15(samples: np.ndarray, coefficient: int) -> np.ndarray:
    product = (samples.astype(np.int64) * np.int64(coefficient) * np.int64(2)) >> np.int64(16)
    product = np.clip(product, -32768, 32767)
    return product.astype(np.int16)


def scalar_s12_to_q15(samples: np.ndarray) -> np.ndarray:
    shifted = samples.astype(np.int64) << np.int64(4)
    shifted[shifted > 32767] = 32767
    shifted[shifted < -32768] = -32768
    return shifted.astype(np.int16)


def vqshl4_q15(samples: np.ndarray) -> np.ndarray:
    shifted = samples.astype(np.int64) << np.int64(4)
    return np.clip(shifted, -32768, 32767).astype(np.int16)


def fft_bin_valid(display_bin: int, sample_rate_hz: int, bandwidth_hz: int) -> bool:
    distance = abs(display_bin - 512)
    bandwidth = bandwidth_hz if bandwidth_hz else 56_000_000
    return distance * sample_rate_hz <= bandwidth * 512


def check_fft_valid_masks() -> None:
    for sample_rate_hz, bandwidth_hz in (
        (60_000_000, 56_000_000),
        (60_000_000, 60_000_000),
        (60_000_000, 1),
        (1_024, 0),
    ):
        direct = [fft_bin_valid(i, sample_rate_hz, bandwidth_hz) for i in range(1024)]
        reconstructed: list[bool] = []
        for group in range(128):
            mask = 0
            for lane_index in range(8):
                if fft_bin_valid(group * 8 + lane_index, sample_rate_hz, bandwidth_hz):
                    mask |= 1 << lane_index
            reconstructed.extend(bool(mask & (1 << lane_index)) for lane_index in range(8))
        if reconstructed != direct:
            raise AssertionError(
                f"FFT validity mask mismatch for Fs={sample_rate_hz}, BW={bandwidth_hz}"
            )


def main() -> None:
    samples = np.arange(-32768, 32768, dtype=np.int32)
    if not np.array_equal(scalar_s12_to_q15(samples), vqshl4_q15(samples)):
        raise AssertionError("12-bit-to-Q15 VQSHL equivalence failed")
    check_fft_valid_masks()

    # Exercise every input value against each coefficient in the 1024-point
    # Hann table. float32 mirrors the firmware's float-domain table creation;
    # the arithmetic proof below covers any small implementation difference in
    # arm_cos_f32 as long as the Hann coefficient remains in 0..32767.
    index = np.arange(1024, dtype=np.float32)
    angle = np.float32(2.0 * np.pi) * index / np.float32(1024.0)
    hann = np.asarray((np.float32(0.5) - np.float32(0.5) * np.cos(angle)) * np.float32(32767.0),
                      dtype=np.int32)
    coefficients = np.unique(hann)
    if coefficients[0] < 0 or coefficients[-1] > 32767:
        raise AssertionError("Hann coefficients left the proven non-negative Q15 range")

    for coefficient in coefficients:
        scalar = scalar_q15(samples, int(coefficient))
        vector = vqdmulh_q15(samples, int(coefficient))
        if not np.array_equal(scalar, vector):
            mismatch = int(np.flatnonzero(scalar != vector)[0])
            raise AssertionError(
                f"coefficient={coefficient}, sample={samples[mismatch]}, "
                f"scalar={scalar[mismatch]}, vqdmulh={vector[mismatch]}"
            )

    # Algebraically, VQDMULH is floor((2*a*b)/2^16), which is the same as
    # floor((a*b)/2^15). Saturation can only change the Q15 result at
    # a=b=-32768. A Hann coefficient is non-negative, so cover every possible
    # coefficient with the input extrema where overflow or sign errors appear.
    boundary_samples = np.asarray([-32768, -32767, -1, 0, 1, 32766, 32767], dtype=np.int32)
    for coefficient in range(0, 32768):
        if not np.array_equal(
            scalar_q15(boundary_samples, coefficient),
            vqdmulh_q15(boundary_samples, coefficient),
        ):
            raise AssertionError(f"boundary mismatch for coefficient={coefficient}")

    print(
        f"PASS: {samples.size} inputs x {coefficients.size} Hann coefficients; "
        "all 32768 non-negative Q15 coefficients covered at arithmetic boundaries; "
        "all 65536 int16 inputs covered for saturating << 4; FFT masks match per-bin predicate"
    )


if __name__ == "__main__":
    main()
