# Sequencer debug trail

Clean-room `cv_sweep.c` built and `cv_init` completed successfully
(RTIA self-cal returned ~121 ohm, LFOSC measured ~31.7 kHz), but
`cv_run_sweep` produced `samples=0` indefinitely.  The sweep's
main poll loop never received a DATAFIFOTHRESH, ENDSEQ, or
CUSTOMINT0 event on AFEINTC_0.

Six bugs were found and fixed.  All fixes derived from the
`ad5940.h` register definitions and the AD5940 / ADuCM355
datasheets, consistent with the repo's clean-room authorship
constraint.

## Bugs, in discovery order

### 1. SEQxINFO bit-field swap

Each DAC step's sequencer command re-wrote the other slot's
SEQxINFO with LEN and ADDR in the wrong positions.
`info_word = (other_addr << 16) | SEQ0_MAXLEN` put the SRAM
address into the LEN field and vice-versa.

- Source: `ad5940.h:2283-2286` — `BITP_AFE_SEQ0INFO_LEN = 16`,
  `BITP_AFE_SEQ0INFO_ADDR = 0`.
- Fix: rewrite using the BITP macros so LEN and ADDR land in the
  correct bit positions.

### 2. LPDACDAT0 missing Vzero pack

The runtime `SEQ_WR(REG_AFE_LPDACDAT0, vbias_code)` wrote only the
12-bit Vbias field, leaving the 6-bit Vzero field zeroed on every
DAC update.  This collapsed the cell common-mode from its
configured 1100 mV to the 200 mV LPDAC offset rail.

- Source: `ad5940.h:1882-1885` — `BITP_AFE_LPDACDAT0_DACIN6 = 12`,
  `BITP_AFE_LPDACDAT0_DACIN12 = 0`.  `SEQ_WR` at `ad5940.h:3856`
  writes 24 bits covering both fields.
- Fix: pack `(vzero_code << BITP_DACIN6) | (vbias << BITP_DACIN12)`
  on every DAC update.

### 3. SEQ2 terminated with SEQ_STOP

The ADC sequence (SEQ2) ended with `SEQ_STOP()`, which disables
the sequencer master-enable.  Since WUPT dispatches SEQ2 twice per
4-slot round (as SEQB and SEQD), the sequencer was disabled after
the very first ADC slot, halting the entire sweep.

- Source: `ad5940.h:3862` — "Disable sequencer, this will generate
  End of Sequence interrupt."  `ad5940.h:3864` — `SEQ_SLP()`
  "Trigger sleep … AFE will go to sleep/hibernate mode" without
  disabling the sequencer.
- Fix: replace `SEQ_STOP()` with `SEQ_SLP()` in `gen_adc_seq`.
  SEQ3 (one-shot init) correctly retains `SEQ_STOP`.

### 4. Missing init-priming sandwich

`cv_run_sweep` called `AD5940_SEQMmrTrig(SEQID_3)` before
`AD5940_SEQCtrlS(bTRUE)`.  Triggering against a disabled
sequencer is a silent no-op.  There was also no busy-wait for
ENDSEQ, no SEQCNT reset, and no disable/re-enable cycle between
the init sequence and the WUPT epoch.

- Source: `ad5940.h:4820` `AD5940_SEQCtrlS` — sequencer
  master-enable.  `ad5940.h:1129` `REG_AFE_SEQCNT` reset value 0.
- Fix: enable first, trigger, busy-wait on AFEINTC_1 ENDSEQ,
  clear flag, disable, zero SEQCNT, re-enable, then start WUPT.

### 5. SEQ0_MAXLEN exceeded SEQ_WR 24-bit payload

`SEQ0_MAXLEN = 304` required LEN bit 8 (register bit 24) to be
set in SEQxINFO.  The `SEQ_WR` instruction encoding carries only
24 bits of data payload (`ad5940.h:3856`), so register bits
[31:24] are zeroed on every sequencer write.  LEN was silently
truncated from 304 to 48, collapsing each ping-pong slot from 76
DAC steps to 12.

- Source: `ad5940.h:3856` — `(data) & 0xFFFFFF`.
  `ad5940.h:2283` — LEN occupies register bits [26:16].
- Fix: reduce `SEQ0_MAXLEN` to 252 (63 steps x 4 cmds), the
  largest value whose LEN fits in 8 bits.

### 6. FIFO fault from RTIA cal residue + oversized conversion window

`cal_rtia()` drives the ADC internally and may overflow the data
FIFO.  In `FIFOMODE_FIFO` (`ad5940.h:3880`), overflow puts the
FIFO into a fault state that silently rejects all subsequent data.
The sweep started against a faulted FIFO.

Additionally, the SEQ2 conversion window was hardcoded at 1600
system clocks (~100 us), producing ~200 SINC3 samples per ADC
slot instead of the intended 1.  Even with a clean FIFO this
would cause immediate re-overflow.

- Source: `ad5940.h:3880` — FIFO fault-on-overflow.
  `ad5940.h:4883` `AD5940_ClksCalculate` — computes minimum
  system clocks for a given SINC3 sample count.
- Fix: re-initialise the FIFO (disable, reconfigure, re-enable)
  and clear all INTC flags before arming the sweep.  Replace the
  hardcoded wait with `AD5940_ClksCalculate` for `DataCount = 1`.

## Methodology

Each fix was validated independently before the next was applied:

1. Flash via J-Link, capture UART at 230400 baud for 220 s.
2. Check for `[INFO] sweep N/5 complete` milestones in the UART
   stream.  If absent, halt the MCU via SWD and read live AFE
   registers (SEQxINFO, AFECON, INTCFLAG0/1, FIFOCNTSTA,
   LPDACDAT0) to identify the stall point.
3. Decode the host-side sequencer generator buffer over UART to
   verify the exact opcode stream written to AFE SRAM.
4. Cross-check the call sequence against `ad5940.h` HAL
   prototypes and register bit definitions; confirm each fix is
   derivable from the public HAL header without reference to ADI
   example application code.

Final validation: five averaged sweeps (2000 steps each), boot
through `[INFO] END_DATA` and `[INFO] idle`, with no cell
connected (open-circuit electrode jacks, floating LPTIA input).
