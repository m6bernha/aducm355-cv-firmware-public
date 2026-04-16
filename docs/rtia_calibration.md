# RTIA self-calibration

## What it is

The AD5940's low-power transimpedance amplifier (LPTIA0) converts cell
current to a voltage for the ADC. The conversion ratio is set by a
software-selectable feedback resistor called RTIA. On the
EVAL-ADuCM355QSPZ the nominal 200 ohm setting is laser-trimmed to be
close to 200 ohm, but absolute accuracy is not guaranteed. Real parts
drift with temperature and process.

`ad5940lib` provides an on-chip self-cal routine,
`AD5940_LPRtiaCal`, that measures the actual RTIA value against the
board's 200 ohm reference resistor (RCAL). The routine drives a known
low-frequency sine across RCAL, measures the resulting voltage at the
LPTIA output, and returns the RTIA magnitude and phase as a polar
impedance.

## What this firmware does

`cv_sweep.c :: cal_rtia()` is a thin orchestration wrapper around
`AD5940_LPRtiaCal`. It populates the `LPRTIACal_Type` structure with
the clock frequencies, the RTIA selector being measured, and the
reference resistor value from the `CvConfig`, then calls the HAL. The
result is stored in module-static variables `s_rtia_ohm` and
`s_rtia_phase_deg`, which `cv_emit_csv` uses to convert raw ADC codes
to current and which are reported in every UART transcript.

```c
I_cell = (ADC_code - baseline) * (V_ref / 2^16) / R_tia_measured
```

The self-cal is invoked exactly once, inside `cv_init`, during boot.
The measured value is used for the entire session. There is no
periodic recalibration, no thermal compensation, and no persistent
storage.

## What it surfaced on the bench

During bring-up on the `gcc_min_uart` bench firmware, the reported
value came back at approximately 121 ohm against the nominal 200 ohm
selection, a 39 percent gain deviation. Three possibilities needed to
be ruled out:

1. A bug in the cal invocation (wrong clock setting, wrong DFT source)
2. An incorrect RCAL resistor on the eval board
3. A genuine trim tolerance

Cross-checked the 121 ohm value with the same three-resistor star
dummy cell used for the polarity bench test: three 1 kohm resistors
joined at a common node, plugged directly into the working, reference,
and counter electrode jacks on the EVAL-ADuCM355QSPZ. Sweeping a CV
ramp across this known 1 kohm passive load with the HAL-reported
121 ohm applied as the current scale factor produced the linear I-V
predicted by Ohm's law. If the 121 ohm number had been wrong by a
meaningful fraction the reconstructed current would have been off by
the same fraction and the slope would have missed the expected
1 mA per volt. The on-chip cal was correct. The data pipeline was
updated to use the measured value as the scale factor for current
reconstruction, and subsequent CV sweeps against the project analyte
matched the desktop potentiostat reference to within the validation
thresholds in `tools/compare_sweeps.py`.

## Authorship note

The self-calibration algorithm lives inside `ad5940lib` and is
Analog Devices' intellectual property. This firmware does not
reimplement it. The authored work here is:

- Selecting the cal routine parameters (clocks, OSR, DFT source)
- Integrating the call into the boot flow
- Validating the reported value against an external current source
- Propagating the measured RTIA into the post-processing pipeline
- Surfacing the result on every UART transcript for traceability

A future iteration could replace the HAL call with a home-grown
single-point cal routine: drive a known voltage across RCAL with
LPDAC0, sample the LPTIA output via the ADC, and compute
`R_tia = V_rcal * R_cal / V_tia`. That would give the repository a
fully-authored calibration path and eliminate the last HAL dependency
in the cal chain. It is not included in this revision because the
HAL routine is already validated and no bench issue forced the
rewrite.
