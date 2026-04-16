/*
 * cv_sweep.h — cyclic voltammetry sweep controller for the AD5940 LP path
 *
 * Public interface for a single-purpose CV sweep application layer built
 * on top of the ad5940lib HAL. Supports:
 *
 *   - Single bidirectional voltage ramp from V_start to V_peak and back
 *   - LPTIA0 current measurement with software-selected RTIA
 *   - On-chip RTIA self-calibration against the 200 ohm RCAL resistor
 *   - In-place multi-sweep averaging into a single sample buffer
 *   - Buffered UART CSV output emitted after averaging completes
 *
 * Not supported (out of scope for this rewrite):
 *   - Voltage-mode measurement on VSE0/VRE0
 *   - Unidirectional ramps
 *   - External RTIA resistors
 *   - Mid-sweep abort or pause
 *
 * Dependencies: ad5940lib (the AD5940 HAL). Users of this header must
 * provide ad5940.h on the include path. See the top-level README for
 * build instructions.
 */

#ifndef CV_SWEEP_H
#define CV_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#define CV_MAX_STEPS   2048u     /* Matches the 8 KB sample buffer budget */
#define CV_APP_FIFO    1024u     /* uint32 elements, 4 KB */


/* ---------- Error codes ---------- */

typedef enum {
    CV_OK                 = 0,
    CV_ERR_PARAM          = -1,   /* Invalid config parameter */
    CV_ERR_HAL            = -2,   /* Underlying ad5940lib returned an error */
    CV_ERR_WAKEUP         = -3,   /* Failed to wake AFE before sweep start */
    CV_ERR_SEQ_OVERFLOW   = -4,   /* Generated sequencer commands exceed SRAM */
    CV_ERR_STEP_RANGE     = -5,   /* StepNumber out of range [2, CV_MAX_STEPS] */
    CV_ERR_TIMEOUT        = -6,   /* Sweep did not complete in expected window */
    CV_ERR_NOT_INITED     = -7,   /* cv_run_* called before cv_init */
} CvError;


/* ---------- Configuration ---------- */

/*
 * CvConfig — user-facing sweep parameters.
 *
 * All voltages are in millivolts relative to AGND. The AD5940 LPDAC
 * produces Vzero (6-bit, 200 mV + n * 34.375 mV) and Vbias (12-bit,
 * 200 mV + n * 0.537 mV). The effective cell bias is (Vbias - Vzero).
 * Valid Vzero range is 200..2200 mV. For a bidirectional sweep that
 * symmetrically spans V_start..V_peak, Vzero must be chosen so that
 * both endpoints land inside the DAC range.
 */
typedef struct {
    /* Sweep waveform */
    float v_start_mv;      /* Initial cell bias, e.g. -300 */
    float v_peak_mv;       /* Peak cell bias,    e.g. +700 */
    float vzero_mv;        /* Vzero reference,   e.g. 1100 */
    uint32_t step_number;  /* Total ADC samples per sweep, even, <= CV_MAX_STEPS */
    uint32_t duration_ms;  /* Total sweep duration, sets scan rate */
    float sample_delay_ms; /* Settling delay between DAC update and ADC capture */

    /* Front end */
    uint32_t lptia_rtia_sel;   /* LPTIARTIA_* selector from ad5940.h */
    uint32_t adc_pga_gain;     /* ADCPGA_* selector */
    uint32_t adc_sinc3_osr;    /* ADCSINC3OSR_* selector */
    float adc_ref_volt;        /* Measured ADC reference in mV, typ 1820 */
    float rcal_ohm;            /* On-board RCAL in ohm, typ 200 */

    /* Averaging */
    uint32_t num_avg_sweeps;   /* >= 1. Samples are += accumulated */

    /* Polarity correction. The LPTIA inverts the cell current sign
     * relative to conventional electrochemistry. Set true to emit
     * anodic-positive currents in cv_emit_csv. */
    bool invert_current;
} CvConfig;


/* ---------- Result ---------- */

typedef struct {
    float current_ua[CV_MAX_STEPS];  /* Per-step current, accumulated sum */
    uint32_t sample_count;           /* Valid elements in current_ua */
    uint32_t completed_sweeps;       /* How many sweeps actually contributed */
    bool has_rtia;                   /* True if rtia_ohm is populated */
    float rtia_ohm;                  /* Measured RTIA from cv_init */
    float rtia_phase_deg;            /* Measured phase */
} CvResult;


/* ---------- API ---------- */

/*
 * cv_init — one-time AFE bring-up and RTIA self-calibration.
 *
 * Performs the full platform configuration (clock, FIFO, sequencer,
 * interrupt controller, LFOSC frequency measurement), configures the
 * LP loop and DSP path according to cfg, then runs the AD5940 LP RTIA
 * self-calibration against the on-board RCAL resistor. The measured
 * RTIA value is stored and later exposed via cv_get_rtia_ohm and used
 * inside cv_run_* to convert ADC codes to current.
 *
 * Must be called exactly once before any cv_run_* call. Returns CV_OK
 * on success or a negative CvError.
 */
CvError cv_init(const CvConfig *cfg);


/*
 * cv_run_sweep — run a single bidirectional CV sweep.
 *
 * Blocks until the sweep completes (all step_number ADC samples
 * collected) or a CvError condition occurs. Samples are written to
 * out->current_ua with out->sample_count == cfg->step_number on
 * success. Does not accumulate; overwrites the result buffer.
 */
CvError cv_run_sweep(CvResult *out);


/*
 * cv_run_averaged — run cfg->num_avg_sweeps sweeps and accumulate.
 *
 * Zeros out->current_ua, then runs num_avg_sweeps sweeps back-to-back,
 * accumulating each into the result buffer via +=. On return the
 * buffer contains a sum; cv_emit_csv divides by completed_sweeps at
 * output time. Sets out->completed_sweeps to the number of sweeps that
 * actually ran (< num_avg_sweeps on early termination).
 */
CvError cv_run_averaged(CvResult *out);


/*
 * cv_emit_csv — write averaged result over stdout (UART).
 *
 * Emits:
 *   [INFO] Sweep complete: <samples> samples, <n> sweeps averaged
 *   [INFO] RTIA cal: <rtia_ohm> ohm, phase <phase> deg
 *   Voltage_mV, Current_uA
 *   <v>, <i>
 *   ...
 *   [INFO] END_DATA
 *
 * Voltage is reconstructed from step index using the stored CvConfig,
 * not from the samples themselves. Current is divided by
 * completed_sweeps and sign-corrected per cfg->invert_current.
 */
void cv_emit_csv(const CvResult *r);


/*
 * cv_get_rtia_ohm — the RTIA magnitude measured during cv_init.
 *
 * Returns 0 if cv_init has not succeeded yet.
 */
float cv_get_rtia_ohm(void);


/*
 * cv_strerror — human-readable name for a CvError.
 */
const char *cv_strerror(CvError e);

#endif /* CV_SWEEP_H */
