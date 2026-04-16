# Engineering notes

Living document. Open questions, known limitations, future directions,
and review observations for the aducm355-cv-firmware project.

## Open questions

### PGA gain in current reconstruction

The ADC is configured with PGA gain = 1.5 (ADCPGA_1P5). The current
reconstruction in `service_fifo` (`cv_sweep.c` line ~717) computes:

    lsb_mv_per_code = adc_ref_volt / 32768.0

Standard AD5940 conversion: `V_input = ADC_code * V_ref / (32768 * PGA)`.
The code omits the PGA divisor. If PGA is not already absorbed into the
RTIA cal result (`AD5940_LPRtiaCal` may return an effective RTIA that
includes PGA), every current reading is 1.5x the true value.

The dummy-cell validation (`docs/rtia_calibration.md`) showed correct
Ohm's law slope, which suggests the PGA factor IS absorbed somewhere.
Needs definitive confirmation by running a known-current source through
the LPTIA and comparing firmware output to a calibrated multimeter.
This is a blocking question for field calibration accuracy.

### SINC3 output bit format

`service_fifo` extracts lower 16 bits of FIFO words and sign-extends
(`cv_sweep.c` line ~726). Verify against the AD5940 datasheet that
`FIFOSRC_SINC3` packs [15:0] = signed SINC3 result with no status
bits in the upper 16. The high-speed path uses different packing.

## Known limitations (production blockers)

### Single-shot operation

The firmware runs one averaged sweep then idles. A field device needs:
triggered acquisition (command from host MCU), continuous monitoring
mode, configurable sweep parameters at runtime. This is the largest
gap between the current prototype and a deployable product.

### No watchdog

If the AFE hangs (documented in `docs/sequencer_debug.md`), the device
bricks until power-cycled. The ADuCM355 has a hardware watchdog timer.
Production firmware must enable it with a timeout longer than a single
sweep cycle (~210 s) and implement a recovery path that resets the AFE
and restarts the sweep.

### RTIA thermal drift

Single boot-time calibration. No periodic recalibration, no temperature
compensation. The EVAL-ADuCM355QSPZ has an onboard ADT7420 temperature
sensor (I2C). For a storm drain with 0-40 C daily swing, RTIA drift
could introduce 5-10% systematic error on every reading. Production
firmware should either recalibrate periodically or apply a temperature
correction lookup table.

### No CRC on CSV output

A single bit-flip in a UART byte turns `0.1234` into `0.9234`. For
field data integrity, add a trailing CRC line after `END_DATA`. The
Nordic telemetry layer may add its own integrity check, but defense in
depth is warranted.

## Performance observations

### FIFO threshold

Current threshold = 4. This generates ~500 MCU interrupts per sweep
(2000 samples / 4). For a battery-powered field device, raising the
threshold to 32 would reduce interrupts to ~63 with no risk of FIFO
overflow (FIFO capacity = 512, sweep rate is slow). Not a correctness
issue but a power budget concern.

### Vzero quantization

The 6-bit LPDAC Vzero has 34.38 mV step size. With Vzero configured
at 1100 mV, the actual output is 1093.9 mV (6.1 mV error, 0.6% of
sweep range). Acceptable for prototype. A future custom PCB with a
dedicated DAC for Vzero would eliminate this quantization.

### Scan rate accuracy

Sweep timing derives from LFOSC frequency (~31.7 kHz) measured once at
boot. LFOSC drifts with temperature. A 1% LFOSC drift causes a 1% scan
rate drift. For CV this is generally acceptable (peak positions are
voltage-dependent, not time-dependent). Chronoamperometry would be more
sensitive.

## Validation status

### Golden reference

No golden reference CSV from this firmware exists yet. The Mar 14 Sprint
lab data (`FYDP/Lab Work/Mar 14 Sprint/`) contains 18 captures from the
PRIVATE firmware (different UART format, voltage range 300 to -300 vs
this firmware's -300 to +700). These are not drop-in golden files for
`tools/compare_sweeps.py`.

To create a proper golden reference: run this firmware with a 1 kohm
resistor star dummy cell (3x 1 kohm joined at common node, free ends
to CE/RE/SE jacks), capture the full UART output, and save as
`tools/golden_1kohm_star.csv`. The expected output is a linear I-V
with slope predicted by Ohm's law and the measured RTIA.

### Validation thresholds

`compare_sweeps.py` uses: mean relative error < 5%, Pearson correlation
> 0.95, peak current error < 10%, peak position error < 2% of sweep
length. These are prototype thresholds. Production QC will need tighter
bounds once the measurement uncertainty budget is characterized.

## Future architecture (v2 custom PCB)

The current firmware targets the EVAL-ADuCM355QSPZ evaluation board.
GlycoTech's production hardware will use a custom PCB with a different
MCU, ESP wireless module, dedicated DAC, ADC, and control electronics.
This firmware (v1) serves as:

1. Proof of concept for the electrochemical measurement chain
2. Reference implementation for AFE sequencer bring-up
3. Validation of the clean-room approach to ADI HAL usage

The v2 firmware will need:

- Runtime command protocol (binary, length-prefixed, CRC-protected)
- Triggered and continuous acquisition modes
- Over-the-air parameter configuration
- Periodic RTIA recalibration with temperature compensation
- Watchdog and fault recovery
- Power management for battery operation
- Data logging to local flash between telemetry uplinks

## Debug methodology reference

`sequencer_debug.md` documents the systematic approach used during
bring-up. Key tools and techniques for future debugging:

- J-Link halt + live AFE register dump (WUPTCON, SEQxINFO, AFECON,
  INTCFLAG, FIFOCNTSTA, LPDACDAT0)
- Host-side sequence buffer decode via UART printf before SRAM write
- S3+S1 boot-mode recovery when AFE hibernate wedges SWD
  (UG-1308 page 22)
- Single-variable validation: one fix per flash cycle, classify
  outcome before proceeding
- FIFO re-init after any operation that drives the ADC internally
  (RTIA cal, LFOSC measure)
