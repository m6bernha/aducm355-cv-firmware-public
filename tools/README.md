# tools/

Host-side validation tooling for the firmware.

## What's here

- `parse_uart_csv.py` — parse a firmware UART log into CvSweep records
- `compare_sweeps.py` — compare a firmware run against a golden reference
- `test_parser.py` — unit tests for the parser

## Typical workflow

1. Connect the EVAL-ADuCM355QSPZ board, open a serial terminal at
   230400 baud, and capture the firmware output to a file:

   ```
   python -m serial.tools.miniterm COM5 230400 > capture.log
   ```

2. Reset the board. The firmware runs five averaged sweeps and emits
   the averaged CSV before idling.

3. Sanity-check the parse:

   ```
   python parse_uart_csv.py capture.log
   ```

4. Compare against the golden reference for the same configuration:

   ```
   python compare_sweeps.py golden_5mM_glucose_50mVps.csv capture.log
   ```

   Exit code 0 means all four criteria passed. Exit code 2 means at
   least one failed and the report shows which.

## Validation thresholds

Defined in `compare_sweeps.compare`:

| Metric | Threshold |
|---|---|
| Sample count match | exact |
| Mean relative error (over sweep) | < 5 % |
| Pearson correlation | > 0.95 |
| Peak current error | < 10 % |
| Peak position error | < 2 % of sweep length |

These are appropriate for a prototype CV firmware against desktop
potentiostat references. Tighter thresholds are not warranted given
the single-point RTIA cal and the 16-bit ADC quantization.

## Dependencies

Pure Python 3.8+. No numpy, no scipy, no pandas. Intentionally
minimal so the tests run anywhere.
