# Polarity inversion: bench debug note

## The observation

Early CV sweeps from the bench firmware came out with the wrong sign.
Sweeps against the project's real analyte that should have produced a
clear anodic feature on the forward scan were instead producing the
mirror image at the same potential. Current magnitude tracked the
reference potentiostat, but the sign was flipped.

## Candidate causes (before bench work)

1. Wrong ADC mux: reading LPTIA0_N as the positive input and
   LPTIA0_P as the negative would invert the code sign
2. Wrong LPDAC switch routing: Vbias and Vzero physically swapped on
   the LPTIA input would reverse the effective cell bias
3. LPTIA sign convention for this pin-mode wiring
4. A sign flip in the post-processing math

## Bench test: passive dummy cell

Built a three-resistor star dummy cell: three 1 kohm resistors joined
at a common node, with the three free ends plugged directly into the
working-electrode, reference-electrode, and counter-electrode jacks on
the EVAL-ADuCM355QSPZ. This is a pure linear passive load that removes
every electrochemistry variable from the test. For a CV ramp across
this network the expected current is `I = V / 1 kohm`, a straight
line through the origin with an unambiguous sign convention.

Ran the same CV waveform against the dummy cell. The result was
linear with the slope predicted by a 1 kohm effective resistance, but
inverted relative to the conventional anodic-positive convention used
by the reference potentiostat data for the same sweep parameters.

## Conclusion

The hardware chain as wired, end to end, produces an ADC code that is
sign-inverted relative to the reference convention. This firmware does
not attempt to attribute the inversion to a specific stage (mux,
switch routing, LPTIA topology, or ADC code interpretation). The
observation is empirical and the fix is a single multiply.

## The fix

A single sign flip in the current emit path, controlled by
`CvConfig.invert_current`. When true, `cv_emit_csv` multiplies the
averaged current by -1 before writing the CSV row so downstream
analysis sees anodic-positive values that match the reference
potentiostat convention.

```c
const float sign = s_cfg.invert_current ? -1.0f : 1.0f;
printf("%.1f, %.4f\n", v, r->current_ua[i] * n_inv * sign);
```

The default `main.c` sets `invert_current = true` because the bench
topology is fixed by the wiring on the EVAL-ADuCM355QSPZ. The flag
exists so the same firmware runs unchanged against a future board or
pinout where the hardware convention already matches the reference.

`cv_emit_csv` is the right place for the sign flip, not the ADC code
conversion inside the FIFO drain. Keeping the raw samples in the
physical sign that the hardware produces makes later debugging easier
because a glitched reading in the log matches the hardware convention.

## Lesson

A passive resistor dummy cell plugged directly into the electrode
jacks is the fastest way to isolate sign and gain questions on a
potentiostat front end. It removes the analyte, the electrodes, and
every electrochemistry variable from the test and leaves only the
AFE signal chain and the post-processing math.
