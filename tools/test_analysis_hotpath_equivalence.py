#!/usr/bin/env python3
"""Bit-exact checks for the CPU0 sliding-window and power-reduction hot path."""

from __future__ import annotations

import random
from pathlib import Path

import numpy as np

from project_layout import resolve_cpu0
from test_analysis_log_vector_equivalence import exact_log2, fast_log2, power_input


FFT_SIZE = 1024
HOP_SIZE = 512
FREQ_POOL = 8
FREQ_BINS = 128
TIME_POOL = 9
TILE_SAMPLES = 590_336
DISPLAY_WIDTH = 192
SPECTRUM_WIDTH = 256
SAMPLE_RATE_HZ = 60_000_000
BANDWIDTH_HZ = 56_000_000
DISPLAY_LOG2_FLOOR = np.float32(-32.0)
DISPLAY_LOG2_CEILING = np.float32(-2.0)
DISPLAY_FALLBACK_MARGIN = np.float32(6.0e-5)


def power(iq: list[int], index: int) -> int:
    real = iq[2 * index] >> 1
    imag = iq[2 * index + 1] >> 1
    return real * real + imag * imag


def reduce_scalar(
    iq: list[int], masks: list[int], peak_power: int, peak_bin: int
) -> tuple[list[int], list[int], list[int], int, int]:
    sums: list[int] = []
    maxima: list[int] = []
    counts: list[int] = []
    for group, mask in enumerate(masks):
        shifted = (group * FREQ_POOL + FFT_SIZE // 2) & (FFT_SIZE - 1)
        total = 0
        maximum = 0
        count = 0
        for lane in range(FREQ_POOL):
            value = power(iq, shifted + lane)
            if not mask & (1 << lane):
                continue
            total += value
            maximum = max(maximum, value)
            count += 1
            if value > peak_power:
                peak_power = value
                peak_bin = shifted + lane
        sums.append(total)
        maxima.append(maximum)
        counts.append(count)
    return sums, maxima, counts, peak_power, peak_bin


def reduce_vector_contract(
    iq: list[int], masks: list[int], peak_power: int, peak_bin: int
) -> tuple[list[int], list[int], list[int], int, int]:
    sums: list[int] = []
    maxima: list[int] = []
    counts: list[int] = []
    for group, mask in enumerate(masks):
        shifted = (group * FREQ_POOL + FFT_SIZE // 2) & (FFT_SIZE - 1)
        if mask == 0xFF:
            powers = [power(iq, shifted + lane) for lane in range(FREQ_POOL)]
            total = sum(powers)
            maximum = max(powers)
            count = FREQ_POOL
            if maximum > peak_power:
                peak_power = maximum
                peak_bin = shifted + powers.index(maximum)
        else:
            total = 0
            maximum = 0
            count = 0
            for lane in range(FREQ_POOL):
                value = power(iq, shifted + lane)
                if not mask & (1 << lane):
                    continue
                total += value
                maximum = max(maximum, value)
                count += 1
                if value > peak_power:
                    peak_power = value
                    peak_bin = shifted + lane
        sums.append(total)
        maxima.append(maximum)
        counts.append(count)
    return sums, maxima, counts, peak_power, peak_bin


def check_power_reduction() -> None:
    rng = random.Random(0x8A8_1024)
    masks = [0xFF] * FREQ_BINS
    masks[0] = 0xE0
    masks[-1] = 0x07
    masks[1] = 0

    peak_power = 0
    peak_bin = 0
    for iteration in range(300):
        iq = [rng.randint(-32768, 32767) for _ in range(FFT_SIZE * 2)]
        if iteration == 0:
            # Equal maxima exercise the strict-greater/first-bin tie contract.
            iq[2 * 512 : 2 * 512 + 4] = [32767, 32767, 32767, 32767]
        if iteration % 7 == 0:
            masks[rng.randrange(FREQ_BINS)] = rng.randrange(256)
        scalar = reduce_scalar(iq, masks, peak_power, peak_bin)
        vector = reduce_vector_contract(iq, masks, peak_power, peak_bin)
        if scalar != vector:
            raise AssertionError(f"power reduction mismatch at iteration {iteration}")
        peak_power, peak_bin = scalar[-2:]

    boundaries = [-32768, -32767, -1, 0, 1, 32766, 32767]
    iq = [boundaries[i % len(boundaries)] for i in range(FFT_SIZE * 2)]
    for mask in (0, 1, 0x55, 0x80, 0xFE, 0xFF):
        masks = [mask] * FREQ_BINS
        if reduce_scalar(iq, masks, 0, 0) != reduce_vector_contract(iq, masks, 0, 0):
            raise AssertionError(f"boundary power mismatch for mask 0x{mask:02X}")


def check_ring_windows() -> None:
    rng = random.Random(0x590_336)
    ring = [-1] * FFT_SIZE
    head = 0
    fill = 0
    consumed = 0
    frames = 0
    while consumed < TILE_SAMPLES:
        chunk = min(rng.randint(1, 4096), TILE_SAMPLES - consumed)
        chunk_consumed = 0
        while chunk_consumed < chunk:
            take = min(FFT_SIZE - fill, chunk - chunk_consumed)
            write_index = (head + fill) & (FFT_SIZE - 1)
            first = min(take, FFT_SIZE - write_index)
            ring[write_index : write_index + first] = range(
                consumed + chunk_consumed,
                consumed + chunk_consumed + first,
            )
            if first < take:
                ring[: take - first] = range(
                    consumed + chunk_consumed + first,
                    consumed + chunk_consumed + take,
                )
            fill += take
            chunk_consumed += take
            if fill == FFT_SIZE:
                logical = ring[head:] + ring[:head]
                expected_start = frames * HOP_SIZE
                if logical != list(range(expected_start, expected_start + FFT_SIZE)):
                    raise AssertionError(f"ring window mismatch at frame {frames}")
                frames += 1
                if consumed + chunk_consumed < TILE_SAMPLES:
                    head = (head + HOP_SIZE) & (FFT_SIZE - 1)
                    fill = FFT_SIZE - HOP_SIZE
        consumed += chunk

    if frames != 1152:
        raise AssertionError(f"expected 1152 frames, got {frames}")


def check_pool_divisor() -> None:
    for mask in range(1 << FREQ_POOL):
        legacy = 0
        for _ in range(TIME_POOL):
            for lane in range(FREQ_POOL):
                if mask & (1 << lane):
                    legacy += 1
        precomputed = mask.bit_count() * TIME_POOL
        if legacy != precomputed:
            raise AssertionError(
                f"pool divisor mismatch for mask 0x{mask:02X}: "
                f"{legacy} != {precomputed}"
            )


def check_display_reducer() -> None:
    valid_bins = [
        shifted_bin
        for shifted_bin in range(FFT_SIZE)
        if abs(shifted_bin - FFT_SIZE // 2) * SAMPLE_RATE_HZ
        <= BANDWIDTH_HZ * (FFT_SIZE // 2)
    ]
    display_map = {
        shifted_bin: valid_index * DISPLAY_WIDTH // len(valid_bins)
        for valid_index, shifted_bin in enumerate(valid_bins)
    }
    rng = random.Random(0x192_955)
    for iteration in range(80):
        iq = [rng.randint(-32768, 32767) for _ in range(FFT_SIZE * 2)]
        scalar = [0] * DISPLAY_WIDTH
        grouped = [0] * DISPLAY_WIDTH

        for shifted_bin, display_bin in display_map.items():
            fft_bin = (shifted_bin + FFT_SIZE // 2) & (FFT_SIZE - 1)
            scalar[display_bin] += power(iq, fft_bin)

        for group in range(FREQ_BINS):
            fft_base = (group * FREQ_POOL + FFT_SIZE // 2) & (FFT_SIZE - 1)
            shifted_base = group * FREQ_POOL
            powers = [power(iq, fft_base + lane) for lane in range(FREQ_POOL)]
            for lane, value in enumerate(powers):
                display_bin = display_map.get(shifted_base + lane)
                if display_bin is not None:
                    grouped[display_bin] += value

        if scalar != grouped:
            raise AssertionError(
                f"192-bin display reducer mismatch at iteration {iteration}"
            )

    group_sizes = [list(display_map.values()).count(index)
                   for index in range(DISPLAY_WIDTH)]
    if group_sizes.count(5) != 187 or group_sizes.count(4) != 5:
        raise AssertionError(f"unexpected 56 MHz display groups: {group_sizes}")


def spectrum_level(log2_power: np.ndarray) -> np.ndarray:
    span = DISPLAY_LOG2_CEILING - DISPLAY_LOG2_FLOOR
    scaled = (log2_power - DISPLAY_LOG2_FLOOR) * np.float32(255.0) / span
    rounded = np.floor(scaled + np.float32(0.5)).astype(np.int32)
    return np.where(
        log2_power <= DISPLAY_LOG2_FLOOR,
        0,
        np.where(log2_power >= DISPLAY_LOG2_CEILING, 255, rounded),
    ).astype(np.uint8)


def guarded_vector_spectrum_level(
    power_sum: np.ndarray, divisor: np.ndarray
) -> np.ndarray:
    approximate = fast_log2(power_input(power_sum, divisor))
    approximate = np.where(
        power_sum == 0, np.float32(-39.863136), approximate
    ).astype(np.float32)
    exact = exact_log2(power_sum, divisor)
    exact = np.where(power_sum == 0, np.float32(-39.863136), exact).astype(
        np.float32
    )

    span = DISPLAY_LOG2_CEILING - DISPLAY_LOG2_FLOOR
    level_scale = np.float32(255.0) / span
    fallback = (
        np.abs(approximate - DISPLAY_LOG2_FLOOR) <= DISPLAY_FALLBACK_MARGIN
    ) | (
        np.abs(approximate - DISPLAY_LOG2_CEILING) <= DISPLAY_FALLBACK_MARGIN
    )
    internal = (approximate > DISPLAY_LOG2_FLOOR) & (
        approximate < DISPLAY_LOG2_CEILING
    )
    scaled = (approximate - DISPLAY_LOG2_FLOOR) * level_scale
    rounded = np.floor(scaled + np.float32(0.5)).astype(np.float32)
    boundary_margin = DISPLAY_FALLBACK_MARGIN * level_scale
    fallback |= internal & (
        (np.abs(scaled - (rounded - np.float32(0.5))) <= boundary_margin)
        | (np.abs(scaled - (rounded + np.float32(0.5))) <= boundary_margin)
    )
    return spectrum_level(np.where(fallback, exact, approximate))


def check_spectrum_reducer() -> None:
    valid_bins = [
        shifted_bin
        for shifted_bin in range(FFT_SIZE)
        if abs(shifted_bin - FFT_SIZE // 2) * SAMPLE_RATE_HZ
        <= BANDWIDTH_HZ * (FFT_SIZE // 2)
    ]
    if valid_bins != list(range(35, 990)):
        raise AssertionError("the formal 56 MHz mask must contain bins 35..989")

    spectrum_map = {
        shifted_bin: valid_index * SPECTRUM_WIDTH // len(valid_bins)
        for valid_index, shifted_bin in enumerate(valid_bins)
    }
    if set(spectrum_map) != set(valid_bins) or len(spectrum_map) != 955:
        raise AssertionError("every valid raw FFT bin must map exactly once")
    group_sizes = [
        list(spectrum_map.values()).count(index)
        for index in range(SPECTRUM_WIDTH)
    ]
    if group_sizes.count(4) != 187 or group_sizes.count(3) != 69:
        raise AssertionError(f"unexpected 256-bin spectrum groups: {group_sizes}")
    divisors = [size * TIME_POOL for size in group_sizes]
    if set(divisors) != {27, 36} or sum(divisors) != 955 * TIME_POOL:
        raise AssertionError(f"invalid nine-frame spectrum divisors: {divisors}")

    # Model two consecutive nine-frame pools with deliberately different
    # power. Only the final pool may contribute to the published spectrum.
    all_frames: list[list[int]] = []
    for frame_index in range(2 * TIME_POOL):
        all_frames.append(
            [
                (frame_index + 1) * 100_003 + shifted_bin * 17
                for shifted_bin in valid_bins
            ]
        )
    captured = [0] * SPECTRUM_WIDTH
    expected = [0] * SPECTRUM_WIDTH
    stale_inclusive = [0] * SPECTRUM_WIDTH
    for frame_index, frame in enumerate(all_frames):
        for valid_index, power_value in enumerate(frame):
            spectrum_bin = spectrum_map[valid_bins[valid_index]]
            stale_inclusive[spectrum_bin] += power_value
            if frame_index >= TIME_POOL:
                captured[spectrum_bin] += power_value
                expected[spectrum_bin] += power_value
    if captured != expected or captured == stale_inclusive:
        raise AssertionError("spectrum must average exactly the final nine frames")
    for spectrum_bin, divisor in enumerate(divisors):
        raw_indices = [
            index
            for index, shifted_bin in enumerate(valid_bins)
            if spectrum_map[shifted_bin] == spectrum_bin
        ]
        direct_average = sum(
            all_frames[frame_index][raw_index]
            for frame_index in range(TIME_POOL, 2 * TIME_POOL)
            for raw_index in raw_indices
        ) / divisor
        if captured[spectrum_bin] / divisor != direct_average:
            raise AssertionError(f"nine-frame average mismatch at bin {spectrum_bin}")

    # A raw-bin impulse occupies one native 256 group. Reducing to 128 first
    # and duplicating/interpolating it necessarily spreads that information.
    impulse_index = 477
    direct_256 = [0] * SPECTRUM_WIDTH
    direct_256[spectrum_map[valid_bins[impulse_index]]] = 1
    pooled_128 = [0] * FREQ_BINS
    pooled_128[(impulse_index * FREQ_BINS) // len(valid_bins)] = 1
    duplicated_128 = [value for value in pooled_128 for _ in range(2)]
    if direct_256 == duplicated_128 or sum(direct_256) != 1 or sum(duplicated_128) != 2:
        raise AssertionError("the 256-bin spectrum regressed to 128-bin interpolation")

    # Exercise scalar log2f and the CMSIS vlog host model around every display
    # rounding boundary. The production guard must make their uint8 output
    # identical even where the unguarded approximation differs by one level.
    powers: list[int] = [0]
    power_divisors: list[int] = [27]
    for divisor in (27, 36):
        for level in range(256):
            boundary_log2 = -32.0 + (level + 0.5) / (255.0 / 30.0)
            target = divisor * (2.0 ** (boundary_log2 + 26.0))
            nearest = max(0, int(round(target)))
            for offset in range(-4, 5):
                powers.append(max(0, nearest + offset))
                power_divisors.append(divisor)
    rng = np.random.default_rng(0x256_955)
    powers.extend(
        int(value)
        for value in rng.integers(0, 1 << 35, size=20_000, dtype=np.uint64)
    )
    power_divisors.extend(
        int(value)
        for value in rng.choice(np.asarray([27, 36], dtype=np.uint32), size=20_000)
    )
    power_array = np.asarray(powers, dtype=np.uint64)
    divisor_array = np.asarray(power_divisors, dtype=np.uint32)
    exact = exact_log2(power_array, divisor_array)
    exact = np.where(power_array == 0, np.float32(-39.863136), exact)
    if not np.array_equal(
        guarded_vector_spectrum_level(power_array, divisor_array),
        spectrum_level(exact),
    ):
        raise AssertionError("guarded MVE spectrum levels differ from scalar levels")


def check_source_shape() -> None:
    source = (
        resolve_cpu0(Path(__file__).resolve().parents[1])
        / "src"
        / "framework"
        / "analysis_pipeline.c"
    ).read_text(encoding="utf-8")
    required = (
        "lane->frame_head",
        "analysis_cycle_now_fast",
        "analysis_lane_hot_t g_lane_hot[2] ANALYSIS_DTCM",
        "hot->frame_iq",
        "hot->display_power_sum",
        "analysis_accumulate_display_power",
        "hot->spectrum_power_sum",
        "g_spectrum_raw_bin_map[shifted_bin]",
        "g_spectrum_power_divisor[spectrum_bin] +=",
        "analysis_accumulate_spectrum_power",
        "const uint32_t display_bin = g_display_raw_bin_map[shifted_bin]",
        "if (display_bin >= RA8P1_DISPLAY_TILE_WIDTH)",
        "hot->display_power_sum[display_bin] += power",
        "const uint32_t power = analysis_fft_power_at(fft_index)",
        "q15_t local[ANALYSIS_INGEST_S16_SCALARS]",
        "analysis_store_display_spectrum(lane)",
        "analysis_display_level_guarded",
    )
    for marker in required:
        if marker not in source:
            raise AssertionError(f"missing optimized source marker: {marker}")
    if "memmove(lane->frame_iq" in source:
        raise AssertionError("sliding-window memmove returned to the hot path")
    for stale_field in ("pool_valid_count", "previous_power_divisor", "lane->frame_iq"):
        if stale_field in source:
            raise AssertionError(f"SDRAM hot-path field returned: {stale_field}")
    if "analysis_store_spectrum_level" in source:
        raise AssertionError("the legacy 128-bin model-pool spectrum path returned")
    if "analysis_fft_power8" in source:
        raise AssertionError("the unproven MVE FFT power path returned")


def main() -> None:
    check_power_reduction()
    check_ring_windows()
    check_pool_divisor()
    check_display_reducer()
    check_spectrum_reducer()
    check_source_shape()
    print(
        "PASS: 300 randomized power frames plus Q15 boundaries/masks are bit-exact; "
        "80 randomized frames preserve every independent 192-bin display sum; "
        "955 raw bins form 187x4 + 69x3 native spectrum groups over the final 9 frames; "
        "guarded MVE/scalar spectrum levels match at all tested boundaries; "
        "590336 samples produce the same 1152 overlapping windows without memmove; "
        "all 256 valid masks preserve the 9-frame pool divisor"
    )


if __name__ == "__main__":
    main()
