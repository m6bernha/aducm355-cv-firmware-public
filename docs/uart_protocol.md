# UART protocol

## Physical layer

- UART0 on P0.10 (TX) and P0.11 (RX)
- 230400 baud, 8 data bits, no parity, 1 stop bit
- No flow control
- TX polling mode (see `retarget_uart.c`)

## Framing

The firmware produces an ASCII-only stream. Three kinds of lines:

1. Status lines beginning with `[BOOT]`, `[INFO]`, `[ERR]`, or
   `[FATAL]`, terminated with `\n`
2. The CSV header line `Voltage_mV, Current_uA`
3. Data lines of the form `<float>, <float>\n` representing one
   averaged sample

A complete sweep transcript looks like:

```
[BOOT] aducm355-cv-firmware
[BOOT] MCU resource init OK
[INFO] LFOSC measured: 32752.4 Hz
[INFO] cv_init OK, RTIA=121.40 ohm phase=0.12 deg
[INFO] sweep 1/5 complete
[INFO] sweep 2/5 complete
[INFO] sweep 3/5 complete
[INFO] sweep 4/5 complete
[INFO] sweep 5/5 complete
[INFO] Sweep complete: 2000 samples, 5 sweeps averaged
[INFO] RTIA cal: 121.40 ohm, phase 0.12 deg
Voltage_mV, Current_uA
-300.0, 0.1234
-299.5, 0.1198
...
+700.0, 4.5612
...
-300.0, 0.1243
[INFO] END_DATA
[INFO] idle
```

The `END_DATA` marker is the canonical "stream complete" signal. A
downstream parser that waits for `END_DATA` is guaranteed to see the
full sweep.

## Why ASCII CSV

- Robust to serial terminal misconfiguration. If the baud rate is
  wrong the user sees garbage instead of silent corruption.
- Human-readable during bring-up. No host-side decoder needed.
- Easy to diff against reference data from other potentiostats that
  also produce CSV.

Cost:

- ~20 characters per sample versus ~4 bytes for a binary frame
- At 2000 samples and 230400 baud, the dump takes ~1.8 seconds. This
  is acceptable because the dump happens once, after the sweep
  completes, not during acquisition.

## Binary protocol

Not implemented. An earlier project sprint sketched a length-prefixed
binary protocol for host-firmware coordination; see the historical
sprint notes in the original bench repository. That work is out of
scope for this public repository, which targets the demonstration
firmware only.

## Parsing

`tools/parse_uart_csv.py` is the reference parser. It is intentionally
permissive: it tolerates extra `[INFO]` lines, ignores lines it does
not recognize, and returns a list of `CvSweep` records for every
`Voltage_mV, Current_uA` header it encounters.
