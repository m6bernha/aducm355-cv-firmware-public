# Architecture

## Signal chain

```
    cell (3-electrode)
        WE  ────────────┐
        RE  ─┐          │
        CE  ─┼──┐       │
             │  │       │
             ▼  ▼       ▼
          ┌──────────┐ ┌──────┐
          │  LPAmp0  │ │LPTIA0│
          │  (CE)    │ │ RTIA │
          └────┬─────┘ └──┬───┘
               │          │ V_tia
               │          ▼
         Vbias │      ┌───────┐
           ┌───┴─────►│  ADC  │── code ──►  SINC3 ──► FIFO ──► MCU
           │          └───────┘
       ┌───┴────┐
       │ LPDAC0 │  (12-bit Vbias, 6-bit Vzero)
       │        │
       └────────┘
           ▲
           │ DAC update
           │
      sequencer
           ▲
           │ wakeup timer
       LFOSC (~32 kHz)
```

The AD5940's low-power path uses LPDAC0 to generate the cell bias, a
low-power op-amp pair (LPAmp0 as the counter-electrode driver, LPTIA0
as the working-electrode sense amplifier) to close the potentiostat
loop, and the 16-bit ADC to digitize the LPTIA output. The AFE
sequencer and wakeup timer run the DAC update and ADC sample loop
autonomously so the host MCU only services FIFO-threshold interrupts.

## Module boundaries

```
┌────────────────────────────────────────────┐
│                main.c                       │
│  - Clock/UART bring-up                      │
│  - Static CvConfig (sweep parameters)       │
│  - cv_init -> cv_run_averaged -> cv_emit_csv│
└──────────────────┬─────────────────────────┘
                   │
                   ▼
┌────────────────────────────────────────────┐
│               cv_sweep.[ch]                 │
│  - CvConfig validation                      │
│  - Platform (clock, FIFO, sequencer, INTC)  │
│  - LP loop (DAC, TIA, reference)            │
│  - DSP (ADC mux, filter, PGA)               │
│  - RTIA self-cal (via HAL primitive)        │
│  - Init + ADC + DAC sequence generation     │
│  - Wakeup timer start/stop                  │
│  - Sweep run loop + FIFO drain              │
│  - In-place multi-sweep averaging           │
│  - CSV emit with voltage reconstruction     │
└──────────────────┬─────────────────────────┘
                   │
                   ▼
┌────────────────────────────────────────────┐
│            ad5940lib (external)             │
│  AD5940_CLKCfg, AD5940_FIFOCfg,              │
│  AD5940_SEQCfg, AD5940_LPLoopCfgS,           │
│  AD5940_DSPCfgS, AD5940_LPRtiaCal,           │
│  AD5940_SEQGenCtrl/Insert/FetchSeq,          │
│  AD5940_WUPTCfg, AD5940_FIFORd, ...          │
└────────────────────────────────────────────┘
```

`cv_sweep.c` contains no direct SPI register writes. Every AFE
interaction goes through `ad5940lib`. The library is not vendored;
users clone it separately and the `build.bat` links against their
local copy (see the top-level `README.md`).

## Sequencer slot layout

```
SEQ RAM byte offset
0x000  ┌──────────────────────────┐
       │ SEQID_3  init sequence   │  runs once at sweep start
0x100  ├──────────────────────────┤
       │ SEQID_2  ADC start       │  runs once per sample period
0x140  ├──────────────────────────┤
       │ SEQID_0  DAC even        │  ping-pong half
0x270  ├──────────────────────────┤
       │ SEQID_1  DAC odd         │  ping-pong half
0x3A0  ├──────────────────────────┤
       │     (free, 1.37 KB)      │
0x800  └──────────────────────────┘
```

Each DAC update is four sequencer commands:

1. `SEQ_WR(LPDACDAT0, code)` — push the 12-bit Vbias code
2. `SEQ_WAIT(10)`            — allow the LPDAC output to settle
3. `SEQ_WR(SEQxINFO, ...)`   — queue the other ping-pong slot
4. `SEQ_SLP()` or `SEQ_INT0()` or `SEQ_STOP()`

Each ping-pong half holds 304 / 4 = 76 DAC updates, so a two-slot
window covers 152 steps. Sweeps larger than that fire `CUSTOMINT0` at
the midpoint of each slot, and the MCU refills the just-consumed slot
with the next 76 steps' worth of updates while the other slot is still
running. This is how the firmware supports the 2000-step default
sweep within 6 KB of sequencer SRAM.

## Timing budget

For the default configuration (2000 steps, 40 s, 10 ms sample delay):

- Step period: 40000 / 2000 = 20 ms
- ADC wakeup: 10 ms (sample_delay)
- DAC wakeup: 20 - 10 = 10 ms
- Wakeup timer ticks from LFOSC (~32.7 kHz) so the 10 ms slice is
  about 327 ticks, well inside the 16-bit WUPT range
- With 5x averaging the full acquisition is 5 * 40 s = 200 s

## RAM budget

```
CvResult.current_ua [2048]       8192 B
cv_sweep s_app_fifo [1024]       4096 B
cv_sweep s_seqgen_buffer [512]   2048 B
CvConfig + scalars               ~128 B
Stack, BSS, retarget buffer      ~1400 B
                                 -------
                                 ~15.9 KB  / 16 KB SRAM
```

The in-place `+=` accumulation for averaging is what keeps this under
the 16 KB limit. A naive design that stored each sweep's samples
separately before averaging would need `N * 8 KB` and would overflow
at `N = 2`.

## Where the sweep parameters came from

The default `CvConfig` in `main.c`:

| Parameter | Value | Why |
|---|---|---|
| `v_start_mv` | -300 | Lower bound of the target analyte's expected redox window in buffered reference runs |
| `v_peak_mv` | +700 | Upper bound of the same window, with headroom above the expected oxidation peak |
| `vzero_mv` | 1100 | Center the LPDAC range; ~900 mV headroom in each direction before the 200..2200 mV rail |
| `step_number` | 2000 | 1 mV/step at 1000 mV sweep span on the rising leg (2000 total both directions) |
| `duration_ms` | 40000 | 50 mV/s scan rate, matches the desktop potentiostat reference |
| `sample_delay_ms` | 10 | Matches the reference; conservative for LPDAC settling at this step size |
| `lptia_rtia_sel` | `LPTIARTIA_200R` | 200 ohm selection fits ~9 mA full scale on the 16-bit ADC with 1.8 V reference |
| `num_avg_sweeps` | 5 | sqrt(5) ~ 2.2x noise reduction; trades 200 s per acquisition for lower shot-noise floor |
| `invert_current` | true | Compensates for the LPTIA sign flip so emitted current is anodic-positive |

## Limitations

- Single-point RTIA cal. No thermal drift compensation. Calibration
  runs once at boot and the result is used for the rest of the
  session.
- No watchdog timer. The main loop has a hang-counter that dumps INTC
  flags but will not force a reset.
- No non-volatile storage of cal values. Every boot re-runs
  `AD5940_LPRtiaCal`.
- The timing assumes the LFOSC frequency measured at boot stays stable
  for the full sweep. On long sweeps (minutes) LFOSC drift can shift
  the scan rate by a few percent.
- Error recovery is coarse: any `CvError` in `cv_run_averaged` aborts
  the full averaging run and the firmware parks in a `while(1)` idle
  loop waiting for a manual reset.
