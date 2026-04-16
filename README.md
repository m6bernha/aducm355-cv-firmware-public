# aducm355-cv-firmware

Cyclic voltammetry firmware for the Analog Devices EVAL-ADuCM355QSPZ
evaluation board. Implements a single bidirectional CV sweep on the
low-power path, with interrupt-driven ADC sampling, on-chip RTIA
self-calibration, in-place multi-sweep averaging, and UART CSV output.

Prototype firmware from a university capstone project. Not production.
See the [Limitations](#limitations) section for an honest accounting.

## What it does

On reset the firmware:

1. Brings up the ADuCM355 system clock and UART0 at 230400 baud
2. Initializes the AD5940 analog front end for the low-power path
3. Runs the `ad5940lib` RTIA self-calibration against the on-board
   RCAL resistor and reports the measured value
4. Runs five back-to-back 2000-step CV sweeps from -300 mV to +700 mV
   at a 50 mV/s scan rate, accumulating samples in place
5. Emits the averaged sweep as CSV over UART
6. Parks in an idle loop until the next reset

Default sweep parameters are in `src/main.c :: cfg` and can be
changed and rebuilt without touching `cv_sweep.c`.

## Project context

Built to support a 3-electrode electrochemical sensor for detecting
glycol contamination in aqueous KOH. The target analyte is dyed
aircraft de-icing fluid, an ethylene glycol based formulation that
runs off airport taxiways into surrounding waterways. The firmware
drives a standard CV sweep on the AD5940 low-power AFE path and emits
averaged sweeps over UART for host-side analysis.

## What's interesting in the code

- **`src/cv_sweep.c`** — a clean-room application layer for the
  AD5940 LP path: CV ramp waveform generation, DAC ping-pong buffer
  management for >152-step sweeps, wakeup-timer-driven ADC sampling,
  FIFO drain with in-place `+=` averaging, and buffered CSV emit.
  Calls only `ad5940lib` HAL primitives. No code copied from any
  ADI example application.
- **`src/cv_sweep.c :: cal_rtia`** — orchestration wrapper around
  the HAL's `AD5940_LPRtiaCal`. On the bench the measured value came
  back at ~121 ohm against the nominal 200 ohm selection, a 39
  percent gain deviation. See `docs/rtia_calibration.md` for the
  integration story. The cal algorithm itself lives in `ad5940lib`
  and is not reimplemented here.
- **Sign-correct current emission.** The LPTIA in this topology
  inverts cell current relative to conventional electrochemistry.
  `docs/polarity_debug.md` walks through the bench debug that
  isolated the sign and the single-flag fix.
- **`tools/compare_sweeps.py`** — host-side validation harness.
  Parses a firmware UART log and compares it against a golden
  reference with four criteria (sample count, mean relative error,
  Pearson correlation, peak error). Used to gate changes to
  `cv_sweep.c`.

## Status

The firmware builds cleanly against GNU Arm 13.2 and runs on the
EVAL-ADuCM355QSPZ with no external cell connected (open-circuit
electrode jacks). Validated milestones:

- Boot: UART0 emits the boot banner at 230400 baud on the board's
  onboard FT232RQ (JP45/JP46 installed).
- RTIA self-cal: returns ~121 ohm against the nominal 200 ohm
  selection at room temperature. See `docs/rtia_calibration.md`.
- LFOSC measure: ~31.7 kHz.
- CV sweep: five back-to-back 2000-step sweeps from -300 mV to
  +700 mV at 50 mV/s, averaged in place, emitted as CSV over
  UART. See `docs/uart_protocol.md` for framing.
- Host-side tooling: UART parser, golden-reference comparator,
  and unit tests under `tools/` are complete.
- See `docs/sequencer_debug.md` for the register-level debug
  story behind the clean-room bring-up.

## Hardware

- **Board**: Analog Devices EVAL-ADuCM355QSPZ
- **MCU**: ADuCM355 (ARM Cortex-M3, 26 MHz, 16 KB SRAM, 256 KB flash)
- **AFE**: AD5940 LP path, LPDAC0 + LPAmp0 + LPTIA0, 16-bit ADC
- **Debugger**: Segger J-Link via the on-board CMSIS-DAP bridge
- **Host**: any serial terminal at 230400 8N1

Wiring: the board ships with JP45 and JP46 set to route UART0 to the
USB bridge. The firmware does not configure any other GPIO beyond the
AFE Ext_Int3 pin (P2.1) that `AD5940_MCUResourceInit` wires up.

## Build

Requires:

- `arm-none-eabi-gcc` on `PATH` (tested with GNU Arm Embedded 13.2)
- A clone of `ad5940lib`
- The ADuCM355 SDK headers (from Analog's distribution)

Neither of the last two dependencies is vendored in this repository.
Clone them yourself and point the build at them:

```
set AD5940LIB_DIR=C:\path\to\ad5940lib
set ADUCM355_SDK_DIR=C:\path\to\ADuCM355_SDK
build.bat
```

On success the `build/` directory contains `cv_firmware.elf`,
`cv_firmware.hex`, and `cv_firmware.bin`. The J-Link flash programmer
can consume any of the three.

Note that this repository does not ship a startup file. Use the
ADuCM355 startup file from the ADI SDK (`startup_ADuCM355.c`). The
`linker.ld` here assumes that file's symbol names and memory layout.

## Run

Flash the board, open a serial terminal at 230400 baud, then reset.
The firmware takes about 210 seconds to complete five averaged sweeps
and emit the CSV dump. A complete run looks like:

```
[BOOT] aducm355-cv-firmware
[BOOT] MCU resource init OK
[INFO] LFOSC measured: 32752.4 Hz
[INFO] cv_init OK, RTIA=121.40 ohm phase=0.12 deg
[INFO] sweep 1/5 complete
...
[INFO] Sweep complete: 2000 samples, 5 sweeps averaged
[INFO] RTIA cal: 121.40 ohm, phase 0.12 deg
Voltage_mV, Current_uA
-300.0, 0.1234
...
[INFO] END_DATA
[INFO] idle
```

See `docs/uart_protocol.md` for the full framing spec and
`tools/parse_uart_csv.py` for a reference parser.

## Repository layout

```
aducm355-cv-firmware/
├── src/
│   ├── main.c              # bring-up + CvConfig + run loop
│   ├── cv_sweep.h          # public CV sweep API
│   ├── cv_sweep.c          # clean-room CV sweep controller
│   └── retarget_uart.c     # newlib _write -> UART0
├── linker.ld               # GCC linker script, ADuCM355 flash + SRAM
├── build.bat               # GCC ARM build for Windows
├── docs/
│   ├── architecture.md
│   ├── rtia_calibration.md
│   ├── polarity_debug.md
│   ├── sequencer_debug.md
│   ├── uart_protocol.md
│   └── ENGINEERING_NOTES.md
├── tools/
│   ├── parse_uart_csv.py
│   ├── compare_sweeps.py
│   ├── patch_checksum.py
│   ├── test_parser.py
│   └── README.md
├── README.md
├── LICENSE
└── .gitignore
```

## Licensing note

This repository is released under the [MIT License](LICENSE).

The firmware depends on `ad5940lib` (from Analog Devices) and the
ADuCM355 SDK, both of which carry their own licenses that restrict
use to ADI processors. Those dependencies are not included in this
repository. Users must clone them separately from Analog's
distribution and are responsible for complying with ADI's terms.

None of the ADI source files have been copied into this repository.
`src/cv_sweep.c` is a clean-room implementation that calls the public
`ad5940lib` C API. If Analog publishes updated or relicensed versions
of their AFE HAL in the future, the firmware only needs to track the
new API, not re-derive the core logic.

## Limitations

- Single-point RTIA self-calibration at boot. No thermal drift
  compensation. No periodic recalibration.
- No watchdog. A hang-counter in the main loop dumps INTC flags for
  debug visibility but will not reset the chip.
- No persistent storage of calibration data. Every boot re-runs the
  self-cal.
- Sweep parameters are compile-time constants in `main.c`. There is
  no runtime command interface. A follow-on repository could add a
  length-prefixed binary protocol for host-firmware coordination.
- Validation harness thresholds (5% mean relative error, 0.95
  Pearson correlation) are appropriate for a prototype against a
  desktop potentiostat reference, not for production QC.
- Error recovery is coarse. Any `CvError` inside an averaging run
  aborts the entire acquisition.

## Credit

Built during a University of Waterloo Nanotechnology Engineering
capstone project (2026). The AFE bring-up, sweep controller, debugging, and
validation harness are the author's work. The underlying AFE HAL
(`ad5940lib`) and ADuCM355 startup code are Analog Devices'.

The sweep controller (`src/cv_sweep.c`) is a clean-room
implementation against the `ad5940lib` HAL and the AD5940 / ADuCM355
datasheets. It is not a port of the ADI example-application layer.
See `docs/sequencer_debug.md` for the bring-up diagnostic trail.
