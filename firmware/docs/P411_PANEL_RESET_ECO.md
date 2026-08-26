# P411 / JD9165 RESX reset fix and adapter ECO

## Confirmed electrical path

On the CPKHMI RA8P1 core board, `P411` is labeled `LCD_TE`. On the supplied
FPC18-to-CN1 adapter, that net passes through a 10 kohm series resistor to CN1
pin 2, which is connected to the JD9165 panel `RESX` input. It is therefore a
panel reset control in this product, not a tearing-effect input.

## Why reset-dependent flicker is possible

Before FSP applies the pin table, an MCU reset leaves P411 high impedance. A
10 kohm series resistor does not define the panel-side logic level. If P411 is
missing from the generated table, or another core/profile owns it as
`MIPI_TE`, the JD9165 may not receive a deterministic low reset pulse while its
I/O rail is rising. A warm MCU reset is especially vulnerable because panel
power can remain present and preserve a partial DSI state.

Leakage, rail ramp, cable capacitance, and the exact reset cause then decide
whether the panel happens to start correctly. This explains the observed
pattern in which some reconnects or resets are stable and others enter a
persistent flicker state. It is a credible root cause, but final attribution
still requires observing `RESX` and the panel rail with an oscilloscope.

## Firmware contract

- CPU1 is the only P411 owner.
- FSP generates `PANEL_RESET = BSP_IO_PORT_04_PIN_11` as GPIO output, initial low.
- CPU0 has no P411 symbol or pin-table entry.
- P411 is not assigned to `DSI_TE`.
- Warm start drives backlight low and asserts `PANEL_RESET` low.
- Bring-up holds reset low for 10 ms, releases it, then waits 120 ms before the
  JD9165 command sequence. Backlight remains low until clean display frames.

## Adapter ECO recommendation

Insert a non-inverting buffer between P411 and `RESX`. Power the buffer from
the panel I/O rail and add a weak pull-down on the buffer input so its powered
default output is low. Select a device only after confirming the panel I/O
voltage and the following limits:

- MCU output versus buffer `VIH`/`VIL` over voltage and temperature.
- Buffer output versus JD9165 `VIH`/`VIL`.
- Partial-power-down (`Ioff`) behavior and absence of reverse powering when
  either the MCU or panel rail is off.
- Output drive, cable capacitance, and edge integrity on the actual FPC.

Replace or bypass the existing 10 kohm series element when the buffer is
fitted; use a normal small edge-damping resistor only if measurements require
one. Do not add an equal 10 kohm pull-down at `RESX`: together with the existing
10 kohm series resistor it creates a half-supply divider when P411 is high and
can violate the reset input high threshold.

No editable adapter CAD source was present in the project, so this document is
an ECO specification, not a claim that the physical board has already changed.

## Verification

Probe panel I/O power, MCU P411, panel-side `RESX`, and backlight enable during
cold power-up and warm reset. Require a defined low level during rail ramp, at
least the firmware's 10 ms low pulse, one clean rising edge, 120 ms before the
first DSI command, and no back-power current. Run repeated cold and warm reset
cycles while checking display stage 6, `last_error=0`, `running=1`, zero GLCDC
underflow growth, and no visible flicker.
