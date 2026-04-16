/*
 * cv_sweep.c — clean-room cyclic voltammetry controller for the AD5940
 *
 * This file implements the CvXxx API declared in cv_sweep.h. It replaces
 * the AD5940Main.c + Ramp.c combination from Analog Devices' LP path
 * example. Every line of orchestration here is authored from the AD5940
 * datasheet and the ad5940lib HAL header; no code is copied from the
 * ADI example application layer.
 *
 * Design overview
 * ---------------
 * A cyclic voltammetry sweep drives the cell bias (Vbias - Vzero) through
 * a triangular waveform and captures one ADC sample at each voltage step
 * through the low-power transimpedance amplifier LPTIA0. The AD5940
 * sequencer and wakeup timer run the sample loop autonomously; the MCU
 * only services FIFO threshold interrupts to drain results.
 *
 * Three sequencer slots are used:
 *   SEQID_3 : one-shot AFE initialization (references, LP loop, DSP)
 *   SEQID_2 : ADC start-convert + FIFO push, fixed
 *   SEQID_0 : DAC update for even steps (first half of ping-pong window)
 *   SEQID_1 : DAC update for odd steps  (second half of ping-pong window)
 *
 * The wakeup timer alternates SEQ0 -> SEQ2 -> SEQ1 -> SEQ2 per sample
 * period. After SEQ0/SEQ1 update the LPDAC, a user-programmed settling
 * delay elapses, then SEQ2 triggers one ADC conversion and pushes the
 * result into the AFE data FIFO. When the FIFO fills above the configured
 * threshold, the MCU services the interrupt, drains the FIFO, and
 * accumulates the samples into the result buffer.
 *
 * For step counts larger than the combined SEQ0 + SEQ1 capacity (a few
 * hundred steps on a 6 KB sequencer SRAM), the two sequences are
 * re-populated from a CUSTOMINT0 interrupt that fires inside each DAC
 * sequence at the midpoint of its slot, letting the host pre-stage the
 * next chunk of DAC update commands without stalling the ramp.
 *
 * Voltage is reconstructed from the step index at output time rather
 * than stored with each sample; doing so halves the RAM cost of the
 * result buffer, which matters on the 16 KB ADuCM355.
 *
 * Current is recovered from the ADC code via
 *     I_lptia = (code - baseline) * V_ref_per_lsb / R_tia_measured
 * where R_tia_measured comes from the on-chip RTIA self-calibration
 * routine called during cv_init. The LPTIA inverts sign relative to
 * conventional electrochemistry; cv_emit_csv flips it back if the
 * caller requested invert_current in the CvConfig.
 */

#include "cv_sweep.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ad5940.h"


/* ---------- Internal constants ---------- */

/*
 * SEQ RAM layout (byte addresses, one command = 4 bytes).
 *
 *   SEQ3_BASE = 0x000   init sequence,            up to 256 cmds = 1.0 KB
 *   SEQ2_BASE = 0x100   ADC start sequence,       64 cmds      = 0.25 KB
 *   SEQ0_BASE = 0x140   DAC even half ping-pong,  252 cmds     = 0.98 KB
 *   SEQ1_BASE = 0x270   DAC odd half ping-pong,   252 cmds     = 0.98 KB
 *                       (remainder = 1.79 KB slack)
 */
#define SEQ3_BASE           0x0000u
#define SEQ3_MAXLEN         256u
#define SEQ2_BASE           0x0100u
#define SEQ2_MAXLEN          64u
#define SEQ0_BASE           0x0140u
/* Maximum commands per DAC ping-pong slot.
 *
 * Constrained to 8 bits because the runtime SEQ_WR(SEQxINFO, ...)
 * inside each DAC step rewrites the OTHER slot's LEN field, and
 * SEQ_WR's data payload is 24 bits (ad5940.h:3856). The SEQxINFO
 * LEN field is in register bits [26:16] (ad5940.h:2283), so LEN
 * bits [10:8] live in register bits [26:24] which lie above
 * SEQ_WR's reach and are zeroed on every write. Any value > 255
 * is silently truncated to its low 8 bits — largest usable slot
 * length is 252 = 63 steps * 4 cmds. */
#define SEQ0_MAXLEN         252u
#define SEQ1_BASE           0x0270u
#define SEQ1_MAXLEN         252u

#define DAC_CMDS_PER_STEP     4u   /* WR_LPDAC, WAIT, WR_SEQxINFO, SLP/INT0 */
#define PP_HALF_STEPS       (SEQ0_MAXLEN / DAC_CMDS_PER_STEP)

#define LPDAC_VBIAS_LSB_MV  (2200.0f / 4095.0f)      /* 12-bit Vbias DAC */
#define LPDAC_VZERO_LSB_MV  (2200.0f / 4095.0f * 64) /* 6-bit Vzero DAC */
#define LPDAC_OFFSET_MV      200.0f                  /* LPDAC output offset */

#define RTIA_CAL_ADC_PGA    ADCPGA_1P5
#define RTIA_CAL_SINC3_OSR  ADCSINC3OSR_4


/* ---------- Module state ---------- */

static CvConfig   s_cfg;
static bool       s_inited;
static float      s_rtia_ohm;
static float      s_rtia_phase_deg;
static float      s_lfosc_freq;

static uint32_t   s_app_fifo[CV_APP_FIFO];
static uint32_t   s_seqgen_buffer[512];

/*
 * Ping-pong state. s_dac_next_step tracks which step index will be
 * written into the next pair of DAC slots when the refill ISR fires.
 * Even ping-pong rounds fill SEQ0; odd rounds fill SEQ1.
 */
static volatile uint32_t s_dac_next_step;
static volatile uint32_t s_samples_received;
static volatile bool     s_sweep_complete;
static volatile bool     s_sweep_error;

static CvResult *s_active_result;




/* ---------- Forward declarations ---------- */

static CvError cfg_platform(void);
static CvError cfg_lp_loop(void);
static CvError cfg_dsp_adc(void);
static CvError cal_rtia(void);
static CvError gen_init_seq(void);
static CvError gen_adc_seq(void);
static CvError gen_dac_seq_slot(uint32_t slot_id, uint32_t first_step,
                                uint32_t step_count, bool last_chunk);
static CvError start_wupt(void);
static CvError stop_wupt(void);
static void    service_fifo(CvResult *out);
static uint16_t vbias_code_for_step(uint32_t step);
static void    log_info(const char *msg);


/* ---------- Voltage reconstruction ---------- */

/*
 * At step index i, the bias voltage is:
 *   half = step_number / 2
 *   if i <  half:   v = v_start + (v_peak - v_start) * (i / half)      (rising)
 *   if i >= half:   v = v_peak  - (v_peak - v_start) * ((i - half) / half) (falling)
 *
 * This is inlined in cv_emit_csv and in vbias_code_for_step so we do not
 * store the 4-byte float per sample.
 */
static float voltage_for_step(uint32_t step)
{
    const uint32_t half = s_cfg.step_number / 2u;
    if (half == 0u) return s_cfg.v_start_mv;

    if (step < half) {
        float t = (float)step / (float)half;
        return s_cfg.v_start_mv + (s_cfg.v_peak_mv - s_cfg.v_start_mv) * t;
    } else {
        float t = (float)(step - half) / (float)half;
        return s_cfg.v_peak_mv - (s_cfg.v_peak_mv - s_cfg.v_start_mv) * t;
    }
}

/*
 * vbias_code_for_step — 12-bit LPDAC code for the step index.
 *
 * LPDAC0 generates Vbias = LPDAC_OFFSET + code * LPDAC_VBIAS_LSB.
 * We want Vbias = Vzero + v_cell, so
 *   code = (Vzero + v_cell - LPDAC_OFFSET) / LPDAC_VBIAS_LSB
 * clamped to [0, 4095].
 */
static uint16_t vbias_code_for_step(uint32_t step)
{
    float v_cell = voltage_for_step(step);
    float vbias_mv = s_cfg.vzero_mv + v_cell;
    float code_f = (vbias_mv - LPDAC_OFFSET_MV) / LPDAC_VBIAS_LSB_MV;
    if (code_f < 0.0f) code_f = 0.0f;
    if (code_f > 4095.0f) code_f = 4095.0f;
    return (uint16_t)(code_f + 0.5f);
}

static uint8_t vzero_code(void)
{
    float code_f = (s_cfg.vzero_mv - LPDAC_OFFSET_MV) / LPDAC_VZERO_LSB_MV;
    if (code_f < 0.0f) code_f = 0.0f;
    if (code_f > 63.0f) code_f = 63.0f;
    return (uint8_t)(code_f + 0.5f);
}


/* ---------- Parameter validation ---------- */

static CvError validate_cfg(const CvConfig *cfg)
{
    if (cfg == NULL) return CV_ERR_PARAM;
    if (cfg->step_number < 2u || cfg->step_number > CV_MAX_STEPS) return CV_ERR_STEP_RANGE;
    if ((cfg->step_number & 1u) != 0u) return CV_ERR_PARAM; /* must be even */
    if (cfg->duration_ms == 0u) return CV_ERR_PARAM;
    if (cfg->sample_delay_ms <= 0.0f) return CV_ERR_PARAM;
    if (cfg->num_avg_sweeps == 0u) return CV_ERR_PARAM;
    if (cfg->vzero_mv < LPDAC_OFFSET_MV) return CV_ERR_PARAM;
    if (cfg->vzero_mv > 2200.0f) return CV_ERR_PARAM;
    if (cfg->adc_ref_volt < 1500.0f || cfg->adc_ref_volt > 2000.0f) return CV_ERR_PARAM;
    if (cfg->rcal_ohm <= 0.0f) return CV_ERR_PARAM;

    /* Check both sweep endpoints land in the 12-bit DAC range. */
    float v0_mv = cfg->vzero_mv + cfg->v_start_mv;
    float v1_mv = cfg->vzero_mv + cfg->v_peak_mv;
    if (v0_mv < LPDAC_OFFSET_MV || v0_mv > 2200.0f) return CV_ERR_PARAM;
    if (v1_mv < LPDAC_OFFSET_MV || v1_mv > 2200.0f) return CV_ERR_PARAM;

    return CV_OK;
}


/* ---------- cv_init ---------- */

CvError cv_init(const CvConfig *cfg)
{
    CvError e;

    e = validate_cfg(cfg);
    if (e != CV_OK) return e;

    s_cfg = *cfg;
    s_inited = false;
    s_rtia_ohm = 0.0f;
    s_rtia_phase_deg = 0.0f;

    /* Initialize the ad5940lib sequence generator workspace. */
    AD5940_SEQGenInit(s_seqgen_buffer,
                      sizeof(s_seqgen_buffer) / sizeof(s_seqgen_buffer[0]));

    /* Hard reset the AD5940 via its reset pin, then re-init driver state. */
    AD5940_HWReset();
    AD5940_Initialize();

    e = cfg_platform();
    if (e != CV_OK) { log_info("[ERR] cfg_platform"); return e; }

    e = cfg_lp_loop();
    if (e != CV_OK) { log_info("[ERR] cfg_lp_loop"); return e; }

    e = cfg_dsp_adc();
    if (e != CV_OK) { log_info("[ERR] cfg_dsp_adc"); return e; }

    e = cal_rtia();
    if (e != CV_OK) { log_info("[ERR] cal_rtia"); return e; }

    e = gen_init_seq();
    if (e != CV_OK) { log_info("[ERR] gen_init_seq"); return e; }

    e = gen_adc_seq();
    if (e != CV_OK) { log_info("[ERR] gen_adc_seq"); return e; }

    s_inited = true;
    {
        int _r = (int)(s_rtia_ohm       * 100.0f + 0.5f);
        int _p = (int)(s_rtia_phase_deg * 100.0f +
                       (s_rtia_phase_deg >= 0.0f ? 0.5f : -0.5f));
        int _pf = _p % 100; if (_pf < 0) _pf = -_pf;
        printf("[INFO] cv_init OK, RTIA=%d.%02d ohm phase=%d.%02d deg\n",
               _r / 100, _r % 100, _p / 100, _pf);
    }
    return CV_OK;
}


/* ---------- cfg_platform ---------- */

static CvError cfg_platform(void)
{
    CLKCfg_Type  clk;
    FIFOCfg_Type fifo;
    SEQCfg_Type  seq;
    LFOSCMeasure_Type lfosc;

    memset(&clk,  0, sizeof(clk));
    memset(&fifo, 0, sizeof(fifo));
    memset(&seq,  0, sizeof(seq));
    memset(&lfosc, 0, sizeof(lfosc));

    /* 16 MHz HFOSC for both system and ADC clock paths. HFXTAL off. */
    clk.HFOSCEn       = bTRUE;
    clk.HFXTALEn      = bFALSE;
    clk.LFOSCEn       = bTRUE;
    clk.HfOSC32MHzMode = bFALSE;
    clk.SysClkSrc     = SYSCLKSRC_HFOSC;
    clk.SysClkDiv     = SYSCLKDIV_1;
    clk.ADCCLkSrc     = ADCCLKSRC_HFOSC;
    clk.ADCClkDiv     = ADCCLKDIV_1;
    AD5940_CLKCfg(&clk);

    /* 2 KB FIFO, 4 KB sequencer SRAM, SINC3 source. Threshold is small
     * so we drain samples soon after each conversion. */
    fifo.FIFOEn     = bTRUE;
    fifo.FIFOMode   = FIFOMODE_FIFO;
    fifo.FIFOSize   = FIFOSIZE_2KB;
    fifo.FIFOSrc    = FIFOSRC_SINC3;
    fifo.FIFOThresh = 4u;
    AD5940_FIFOCfg(&fifo);

    seq.SeqMemSize   = SEQMEMSIZE_4KB;
    seq.SeqBreakEn   = bFALSE;
    seq.SeqIgnoreEn  = bTRUE;
    seq.SeqCntCRCClr = bTRUE;
    seq.SeqEnable    = bFALSE;
    seq.SeqWrTimer   = 0u;
    AD5940_SEQCfg(&seq);

    /* INTC1 = diagnostics: all sources. INTC0 = service sources we
     * actually handle in the main loop (FIFO threshold, end-of-seq,
     * custom interrupt for ping-pong refill). */
    AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_ALLINT, bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
    AD5940_INTCCfg(AFEINTC_0,
                   AFEINTSRC_DATAFIFOTHRESH | AFEINTSRC_ENDSEQ | AFEINTSRC_CUSTOMINT0,
                   bTRUE);
    AD5940_INTCClrFlag(AFEINTSRC_ALLINT);

    /* Measure LFOSC against the 16 MHz system clock. Wakeup timer
     * timing derives from this frequency, so an inaccurate value here
     * skews the sweep duration. */
    lfosc.CalDuration  = 1000.0f;
    lfosc.CalSeqAddr   = 0u;
    lfosc.SystemClkFreq = 16000000.0f;
    if (AD5940_LFOSCMeasure(&lfosc, &s_lfosc_freq) != AD5940ERR_OK) {
        return CV_ERR_HAL;
    }
    {
        int _lf = (int)(s_lfosc_freq * 10.0f + 0.5f);
        printf("[INFO] LFOSC measured: %d.%01d Hz\n", _lf / 10, _lf % 10);
    }

    AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);
    return CV_OK;
}


/* ---------- cfg_lp_loop ---------- */

static CvError cfg_lp_loop(void)
{
    AFERefCfg_Type  ref;
    LPLoopCfg_Type  lp;

    memset(&ref, 0, sizeof(ref));
    memset(&lp,  0, sizeof(lp));

    /* Turn on the HP bandgap + 1.1 V and 1.8 V buffers (needed for the
     * ADC reference chain). Leave the optional LP reference off; we
     * run the LP loop from the HP reference. */
    ref.HpBandgapEn    = bTRUE;
    ref.Hp1V1BuffEn    = bTRUE;
    ref.Hp1V8BuffEn    = bTRUE;
    ref.Disc1V1Cap     = bFALSE;
    ref.Disc1V8Cap     = bFALSE;
    ref.Hp1V8ThemBuff  = bFALSE;
    ref.Hp1V8Ilimit    = bFALSE;
    ref.Lp1V1BuffEn    = bFALSE;
    ref.Lp1V8BuffEn    = bFALSE;
    ref.LpBandgapEn    = bTRUE;
    ref.LpRefBufEn     = bTRUE;
    ref.LpRefBoostEn   = bFALSE;
    AD5940_REFCfgS(&ref);

    /* LPTIA0 / LPDAC0 / LPAmp0. This is a potentiostat topology:
     *   LPDAC0 drives Vbias/Vzero
     *   LPAmp0 is the counter-electrode driver
     *   LPTIA0 senses working-electrode current through RTIA
     */
    lp.LpAmpCfg.LpAmpSel    = LPAMP0;
    lp.LpAmpCfg.LpAmpPwrMod = LPAMPPWR_BOOST3;
    lp.LpAmpCfg.LpPaPwrEn   = bTRUE;
    lp.LpAmpCfg.LpTiaPwrEn  = bTRUE;
    lp.LpAmpCfg.LpTiaRf     = LPTIARF_20K;
    lp.LpAmpCfg.LpTiaRload  = LPTIARLOAD_SHORT;
    lp.LpAmpCfg.LpTiaRtia   = s_cfg.lptia_rtia_sel;
    lp.LpAmpCfg.LpTiaSW     = LPTIASW(2) | LPTIASW(4) | LPTIASW(5);

    lp.LpDacCfg.LpdacSel     = LPDAC0;
    lp.LpDacCfg.DacData6Bit  = vzero_code();
    lp.LpDacCfg.DacData12Bit = vbias_code_for_step(0u);
    lp.LpDacCfg.DataRst      = bFALSE;
    lp.LpDacCfg.LpDacSW      = LPDACSW_VBIAS2LPPA | LPDACSW_VZERO2LPTIA;
    lp.LpDacCfg.LpDacRef     = LPDACREF_2P5;
    lp.LpDacCfg.LpDacSrc     = LPDACSRC_MMR;
    lp.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_12BIT;
    lp.LpDacCfg.LpDacVzeroMux = LPDACVZERO_6BIT;
    lp.LpDacCfg.PowerEn      = bTRUE;
    AD5940_LPLoopCfgS(&lp);

    return CV_OK;
}


/* ---------- cfg_dsp_adc ---------- */

static CvError cfg_dsp_adc(void)
{
    DSPCfg_Type dsp;
    memset(&dsp, 0, sizeof(dsp));

    /* ADC samples the LPTIA0 differential pair. */
    dsp.ADCBaseCfg.ADCMuxN = ADCMUXN_LPTIA0_N;
    dsp.ADCBaseCfg.ADCMuxP = ADCMUXP_LPTIA0_P;
    dsp.ADCBaseCfg.ADCPga  = s_cfg.adc_pga_gain;

    /* 800 kHz raw ADC rate, SINC3 -> SINC2 chain. We read from the
     * SINC3 output so Sinc2NotchEnable does not affect data path, but
     * we leave it configured to keep the HAL struct in a known state. */
    dsp.ADCFilterCfg.ADCSinc3Osr   = s_cfg.adc_sinc3_osr;
    dsp.ADCFilterCfg.ADCRate       = ADCRATE_800KHZ;
    dsp.ADCFilterCfg.BpSinc3       = bFALSE;
    dsp.ADCFilterCfg.Sinc2NotchEnable = bTRUE;
    dsp.ADCFilterCfg.BpNotch       = bTRUE;
    dsp.ADCFilterCfg.ADCSinc2Osr   = ADCSINC2OSR_1067;
    dsp.ADCFilterCfg.ADCAvgNum     = ADCAVGNUM_2;
    AD5940_DSPCfgS(&dsp);

    return CV_OK;
}


/* ---------- RTIA self-calibration ---------- */

/*
 * Uses the ad5940lib HAL primitive AD5940_LPRtiaCal to measure the
 * actual LPTIA feedback resistor against the on-board RCAL reference.
 * The HAL drives a known voltage across RCAL, measures the resulting
 * current through RTIA, and returns the complex impedance. We store
 * magnitude and phase for later use by cv_emit_csv.
 *
 * This function is orchestration only. The calibration algorithm
 * itself lives inside ad5940lib and is not part of this project's
 * authored code.
 */
static CvError cal_rtia(void)
{
    LPRTIACal_Type cal;
    fImpPol_Type   result_pol;

    memset(&cal, 0, sizeof(cal));
    memset(&result_pol, 0, sizeof(result_pol));

    cal.LpAmpSel     = LPAMP0;
    cal.LpTiaRtia    = s_cfg.lptia_rtia_sel;
    cal.LpAmpPwrMod  = LPAMPPWR_BOOST3;
    cal.bPolarResult = bTRUE;
    cal.AdcClkFreq   = 16000000.0f;
    cal.SysClkFreq   = 16000000.0f;
    cal.ADCSinc3Osr  = RTIA_CAL_SINC3_OSR;
    cal.ADCSinc2Osr  = ADCSINC2OSR_1067;
    cal.DftCfg.DftNum = DFTNUM_16384;
    cal.DftCfg.DftSrc = DFTSRC_SINC3;
    cal.DftCfg.HanWinEn = bTRUE;
    cal.fFreq        = s_lfosc_freq / 4.0f; /* Low-frequency probe */
    cal.fRcal        = s_cfg.rcal_ohm;

    if (AD5940_LPRtiaCal(&cal, &result_pol) != AD5940ERR_OK) {
        return CV_ERR_HAL;
    }

    s_rtia_ohm       = result_pol.Magnitude;
    s_rtia_phase_deg = result_pol.Phase * (180.0f / 3.14159265f);
    return CV_OK;
}

float cv_get_rtia_ohm(void)
{
    return s_rtia_ohm;
}


/* ---------- Sequence generation ---------- */

static CvError gen_init_seq(void)
{
    const uint32_t *seq_cmd;
    uint32_t seq_len;
    SEQInfo_Type info;

    /* ad5940lib records every register-touching HAL call made between
     * SEQGenCtrl(bTRUE) and SEQGenCtrl(bFALSE) into the sequence buffer
     * instead of writing to hardware. We re-emit cfg_lp_loop and
     * cfg_dsp_adc into the generator so that at runtime the sequencer
     * can re-apply them on each sweep start without MCU intervention. */
    AD5940_SEQGenCtrl(bTRUE);
    (void)cfg_lp_loop();
    (void)cfg_dsp_adc();
    AD5940_AFECtrlS(AFECTRL_ADCPWR | AFECTRL_ADCCNV, bFALSE);
    AD5940_SEQGenInsert(SEQ_STOP());
    AD5940_SEQGenCtrl(bFALSE);

    if (AD5940_SEQGenFetchSeq(&seq_cmd, &seq_len) != AD5940ERR_OK) {
        return CV_ERR_HAL;
    }
    if (seq_len > SEQ3_MAXLEN) return CV_ERR_SEQ_OVERFLOW;

    memset(&info, 0, sizeof(info));
    info.SeqId      = SEQID_3;
    info.SeqRamAddr = SEQ3_BASE;
    info.pSeqCmd    = seq_cmd;
    info.SeqLen     = seq_len;
    info.WriteSRAM  = bTRUE;
    AD5940_SEQInfoCfg(&info);
    return CV_OK;
}

static CvError gen_adc_seq(void)
{
    const uint32_t *seq_cmd;
    uint32_t seq_len;
    SEQInfo_Type info;

    /* ADC control sequence: power on ADC, start convert, wait for
     * exactly 1 SINC3 output, stop convert, power off. This runs
     * once per sample period between the two DAC update slots.
     *
     * The conversion window must admit exactly 1 SINC3 sample —
     * more would flood the FIFO (each SEQ2 invocation should
     * produce 1 data word). AD5940_ClksCalculate (ad5940.h:4883)
     * returns the minimum system-clock count for DataCount SINC3
     * outputs given the configured OSR and clock ratio. */
    uint32_t adc_wait_clks;
    {
        ClksCalInfo_Type ci;
        memset(&ci, 0, sizeof(ci));
        ci.DataType      = DATATYPE_SINC3;
        ci.DataCount     = 1u;
        ci.ADCSinc3Osr   = s_cfg.adc_sinc3_osr;
        ci.ADCSinc2Osr   = ADCSINC2OSR_1067;
        ci.ADCAvgNum     = ADCAVGNUM_2;
        ci.RatioSys2AdcClk = 1.0f;
        AD5940_ClksCalculate(&ci, &adc_wait_clks);
        adc_wait_clks += 4u;
    }

    AD5940_SEQGenCtrl(bTRUE);
    AD5940_AFECtrlS(AFECTRL_ADCPWR, bTRUE);
    AD5940_SEQGenInsert(SEQ_WAIT(16u * 250u));       /* ~250 us settle */
    AD5940_AFECtrlS(AFECTRL_ADCCNV, bTRUE);
    AD5940_SEQGenInsert(SEQ_WAIT(adc_wait_clks));
    AD5940_AFECtrlS(AFECTRL_ADCCNV | AFECTRL_ADCPWR, bFALSE);
    /* SEQ2 repeats every WUPT round (twice: as slot B and slot D).
     * It must terminate with SEQ_SLP so the sequencer stays armed
     * for WUPT's next dispatch. SEQ_STOP disables the sequencer
     * master-enable (ad5940.h:3862) and would halt the whole sweep
     * after the first ADC slot. SEQ3 (init, one-shot) correctly
     * uses SEQ_STOP; SEQ2 (repeating) must use SEQ_SLP
     * (ad5940.h:3864). */
    AD5940_SEQGenInsert(SEQ_SLP());
    AD5940_SEQGenCtrl(bFALSE);

    if (AD5940_SEQGenFetchSeq(&seq_cmd, &seq_len) != AD5940ERR_OK) {
        return CV_ERR_HAL;
    }
    if (seq_len > SEQ2_MAXLEN) return CV_ERR_SEQ_OVERFLOW;

    memset(&info, 0, sizeof(info));
    info.SeqId      = SEQID_2;
    info.SeqRamAddr = SEQ2_BASE;
    info.pSeqCmd    = seq_cmd;
    info.SeqLen     = seq_len;
    info.WriteSRAM  = bTRUE;
    AD5940_SEQInfoCfg(&info);
    return CV_OK;
}

/*
 * gen_dac_seq_slot — populate SEQ0 or SEQ1 with a run of DAC updates.
 *
 * Writes step_count DAC-update entries starting at step index
 * first_step. Each entry is four commands:
 *   SEQ_WR(LPDACDAT0, code)             update Vbias DAC
 *   SEQ_WAIT(10)                        allow DAC internal settle
 *   SEQ_WR(SEQxINFO, next_slot)         queue the other ping-pong half
 *   SEQ_SLP()                           release AFE to sleep state
 *
 * On the last chunk of the sweep we swap SEQ_SLP for SEQ_STOP so the
 * whole sequencer halts naturally after the final sample.
 *
 * Between the two halves of a full sweep we insert SEQ_INT0 in the
 * middle slot so the MCU gets a refill signal before the next
 * ping-pong round wraps.
 */
static CvError gen_dac_seq_slot(uint32_t slot_id, uint32_t first_step,
                                uint32_t step_count, bool last_chunk)
{
    if (step_count == 0u || step_count > SEQ0_MAXLEN / DAC_CMDS_PER_STEP) {
        return CV_ERR_SEQ_OVERFLOW;
    }

    uint32_t seq_addr  = (slot_id == SEQID_0) ? SEQ0_BASE : SEQ1_BASE;
    uint32_t other_addr = (slot_id == SEQID_0) ? SEQ1_BASE : SEQ0_BASE;
    uint32_t other_id  = (slot_id == SEQID_0) ? SEQID_1 : SEQID_0;

    AD5940_SEQGenCtrl(bTRUE);

    for (uint32_t k = 0u; k < step_count; k++) {
        uint16_t vbias = vbias_code_for_step(first_step + k);
        bool is_last = last_chunk && (k == step_count - 1u);

        /* LPDACDAT0 packs 6-bit Vzero in [17:12] and 12-bit Vbias in
         * [11:0]. SEQ_WR writes the full 24-bit data payload so the
         * Vzero half must be re-supplied on every DAC update or it
         * will be cleared to the 200 mV rail. vzero_code() is
         * constant across the sweep. */
        uint32_t dac_data =
            ((uint32_t)vzero_code() << BITP_AFE_LPDACDAT0_DACIN6) |
            ((uint32_t)vbias        << BITP_AFE_LPDACDAT0_DACIN12);
        AD5940_SEQGenInsert(SEQ_WR(REG_AFE_LPDACDAT0, dac_data));
        AD5940_SEQGenInsert(SEQ_WAIT(10u));

        /* Queue the other ping-pong slot for the next wakeup. */
        /* SEQxINFO bit layout per ad5940.h: LEN in [26:16], ADDR in
         * [10:0]. Use the BITP macros so a future ad5940.h relayout
         * does not silently break this. */
        uint32_t info_word =
            ((uint32_t)SEQ0_MAXLEN << BITP_AFE_SEQ0INFO_LEN) |
            ((uint32_t)other_addr  << BITP_AFE_SEQ0INFO_ADDR);
        AD5940_SEQGenInsert(SEQ_WR(
            (other_id == SEQID_0) ? REG_AFE_SEQ0INFO : REG_AFE_SEQ1INFO,
            info_word));

        if (is_last) {
            AD5940_SEQGenInsert(SEQ_STOP());
        } else if (k == step_count / 2u) {
            AD5940_SEQGenInsert(SEQ_INT0());  /* refill signal */
        } else {
            AD5940_SEQGenInsert(SEQ_SLP());
        }
    }

    const uint32_t *seq_cmd;
    uint32_t seq_len;
    AD5940_SEQGenCtrl(bFALSE);
    if (AD5940_SEQGenFetchSeq(&seq_cmd, &seq_len) != AD5940ERR_OK) {
        return CV_ERR_HAL;
    }
    if (seq_len > SEQ0_MAXLEN) return CV_ERR_SEQ_OVERFLOW;

    SEQInfo_Type info;
    memset(&info, 0, sizeof(info));
    info.SeqId      = slot_id;
    info.SeqRamAddr = seq_addr;
    info.pSeqCmd    = seq_cmd;
    info.SeqLen     = seq_len;
    info.WriteSRAM  = bTRUE;
    AD5940_SEQInfoCfg(&info);
    return CV_OK;
}


/* ---------- Wakeup timer ---------- */

static CvError start_wupt(void)
{
    WUPTCfg_Type wupt;
    memset(&wupt, 0, sizeof(wupt));

    /* Four-step round: DAC_even -> ADC -> DAC_odd -> ADC.
     * Sleep time between slots is 4 LFOSC ticks (datasheet minimum).
     * Wakeup time sets the gap between DAC update and ADC sample or
     * between ADC sample and the next DAC update. */
    const float step_period_ms =
        (float)s_cfg.duration_ms / (float)s_cfg.step_number;
    const float adc_wakeup_ms  = s_cfg.sample_delay_ms;
    const float dac_wakeup_ms  = step_period_ms - s_cfg.sample_delay_ms;

    if (dac_wakeup_ms <= 0.0f) return CV_ERR_PARAM;

    wupt.WuptEn       = bTRUE;
    wupt.WuptEndSeq   = WUPTENDSEQ_D;
    wupt.WuptOrder[0] = SEQID_0;
    wupt.WuptOrder[1] = SEQID_2;
    wupt.WuptOrder[2] = SEQID_1;
    wupt.WuptOrder[3] = SEQID_2;

    wupt.SeqxSleepTime[SEQID_2] = 4u;
    wupt.SeqxWakeupTime[SEQID_2] =
        (uint32_t)(s_lfosc_freq * adc_wakeup_ms / 1000.0f) - 4u - 2u;
    wupt.SeqxSleepTime[SEQID_0] = 4u;
    wupt.SeqxWakeupTime[SEQID_0] =
        (uint32_t)(s_lfosc_freq * dac_wakeup_ms / 1000.0f) - 4u - 2u;
    wupt.SeqxSleepTime[SEQID_1] = wupt.SeqxSleepTime[SEQID_0];
    wupt.SeqxWakeupTime[SEQID_1] = wupt.SeqxWakeupTime[SEQID_0];
    AD5940_WUPTCfg(&wupt);

    if (AD5940_WakeUp(10) > 10) return CV_ERR_WAKEUP;
    return CV_OK;
}

static CvError stop_wupt(void)
{
    if (AD5940_WakeUp(10) > 10) return CV_ERR_WAKEUP;
    AD5940_WUPTCtrl(bFALSE);
    AD5940_WUPTCtrl(bFALSE);
    return CV_OK;
}


/* ---------- Sample draining ---------- */

static void service_fifo(CvResult *out)
{
    uint32_t fifo_count = AD5940_FIFOGetCnt();
    if (fifo_count == 0u) return;
    if (fifo_count > CV_APP_FIFO) fifo_count = CV_APP_FIFO;

    AD5940_FIFORd(s_app_fifo, fifo_count);

    const float lsb_mv_per_code = s_cfg.adc_ref_volt / 32768.0f;
    const float rtia_used = (s_rtia_ohm > 0.0f) ? s_rtia_ohm : 1.0f;

    for (uint32_t i = 0u; i < fifo_count; i++) {
        if (s_samples_received >= s_cfg.step_number) break;

        /* AD5940 SINC3 output is a signed 16-bit code centered near
         * mid-scale for zero differential. Extract the 16 LSBs and
         * sign-extend. */
        int32_t code = (int32_t)(s_app_fifo[i] & 0xFFFFu);
        if (code >= 0x8000) code -= 0x10000;

        float v_diff_mv = (float)code * lsb_mv_per_code;
        float i_ua = v_diff_mv * 1000.0f / rtia_used; /* mV/ohm -> uA */

        out->current_ua[s_samples_received] += i_ua;
        s_samples_received++;
    }

    out->sample_count = s_samples_received;
}


/* ---------- cv_run_sweep / cv_run_averaged ---------- */

static CvError run_one_sweep_accumulating(CvResult *out)
{
    CvError e;

    s_samples_received = 0u;
    s_dac_next_step    = 0u;
    s_sweep_complete   = false;
    s_sweep_error      = false;
    s_active_result    = out;

    /* Populate the first ping-pong chunk. The sequencer is re-armed in
     * the refill ISR for subsequent chunks. */
    const uint32_t first_chunk = (s_cfg.step_number < PP_HALF_STEPS)
        ? s_cfg.step_number
        : PP_HALF_STEPS;
    e = gen_dac_seq_slot(SEQID_0, 0u, first_chunk,
                         s_cfg.step_number == first_chunk);
    if (e != CV_OK) return e;
    s_dac_next_step = first_chunk;

    if (s_cfg.step_number > first_chunk) {
        const uint32_t second_chunk =
            (s_cfg.step_number - first_chunk < PP_HALF_STEPS)
                ? (s_cfg.step_number - first_chunk)
                : PP_HALF_STEPS;
        e = gen_dac_seq_slot(SEQID_1, first_chunk, second_chunk,
                             first_chunk + second_chunk == s_cfg.step_number);
        if (e != CV_OK) return e;
        s_dac_next_step = first_chunk + second_chunk;
    }

    /* Re-initialise the data FIFO before arming the sweep.
     *
     * cal_rtia() and the AD5940 library's internal sequencer probes
     * drive the ADC and may fill the FIFO. In FIFOMODE_FIFO
     * (ad5940.h:3880), an overflow puts the FIFO into a fault state
     * that silently rejects all subsequent data. Disable, reconfigure,
     * and re-enable to guarantee a clean, empty FIFO.
     *
     * Clear all INTC flags and the MCU-side EXTINT3 latch so the poll
     * loop starts with no stale edges from the calibration phase. */
    AD5940_FIFOCtrlS(FIFOSRC_SINC3, bFALSE);
    {
        FIFOCfg_Type fifo;
        memset(&fifo, 0, sizeof(fifo));
        fifo.FIFOEn     = bTRUE;
        fifo.FIFOMode   = FIFOMODE_FIFO;
        fifo.FIFOSize   = FIFOSIZE_2KB;
        fifo.FIFOSrc    = FIFOSRC_SINC3;
        fifo.FIFOThresh = 4u;
        AD5940_FIFOCfg(&fifo);
    }
    AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
    AD5940_ClrMCUIntFlag();

    /* Prime the sequencer for WUPT-driven repeat execution.
     *
     * Order is hardware-dictated:
     *  1. Master-enable the sequencer. SEQMmrTrig against a
     *     disabled sequencer is a silent no-op.
     *  2. Fire the one-shot init (SEQ3 terminates in SEQ_STOP,
     *     raising ENDSEQ on AFEINTC_1).
     *  3. Busy-wait on ENDSEQ — we must not start WUPT until the
     *     AFE is in its post-init state.
     *  4. Clear the flag so the main poll loop later sees a clean
     *     ENDSEQ edge from the sweep's own SEQ_STOP (end-of-sweep).
     *  5. Disable, zero SEQCNT, re-enable. The hardware sequencer-
     *     command counter must start from 0 for the WUPT epoch
     *     (REG_AFE_SEQCNT reset value is 0 per ad5940.h; carrying
     *     the init-phase SEQCNT into the sweep causes WUPT dispatch
     *     to mis-schedule).
     *
     * The pre-trigger ENDSEQ clear guards against the flag being
     * latched by cal_rtia's internal sequencer probes (LP RTIA cal
     * runs its own short sequence and may leave ENDSEQ set).
     */
    AD5940_INTCClrFlag(AFEINTSRC_ENDSEQ);
    AD5940_SEQCtrlS(bTRUE);
    AD5940_SEQMmrTrig(SEQID_3);
    while (AD5940_INTCTestFlag(AFEINTC_1, AFEINTSRC_ENDSEQ) == bFALSE) { }
    AD5940_INTCClrFlag(AFEINTSRC_ENDSEQ);
    AD5940_SEQCtrlS(bFALSE);
    AD5940_WriteReg(REG_AFE_SEQCNT, 0);
    AD5940_SEQCtrlS(bTRUE);

    e = start_wupt();
    if (e != CV_OK) return e;

    /* Main polling loop. Uses the MCU-side interrupt flag the
     * ad5940lib glue sets from the Ext_Int handler. A hang watchdog
     * dumps the INTC flags if we go too many iterations without a
     * sample. */
    uint32_t hang_count = 0u;
    while (!s_sweep_complete && !s_sweep_error) {
        if (AD5940_GetMCUIntFlag()) {
            AD5940_ClrMCUIntFlag();
            hang_count = 0u;

            uint32_t flags = AD5940_INTCGetFlag(AFEINTC_0);

            if (flags & AFEINTSRC_DATAFIFOTHRESH) {
                service_fifo(out);
                AD5940_INTCClrFlag(AFEINTSRC_DATAFIFOTHRESH);
            }

            if (flags & AFEINTSRC_CUSTOMINT0) {
                /* Ping-pong refill. Repopulate the slot that just ran. */
                AD5940_INTCClrFlag(AFEINTSRC_CUSTOMINT0);

                if (s_dac_next_step < s_cfg.step_number) {
                    uint32_t remaining = s_cfg.step_number - s_dac_next_step;
                    uint32_t chunk = (remaining < PP_HALF_STEPS)
                                     ? remaining : PP_HALF_STEPS;
                    uint32_t slot = (((s_dac_next_step / PP_HALF_STEPS) & 1u) == 0u)
                                    ? SEQID_0 : SEQID_1;
                    bool last = (s_dac_next_step + chunk == s_cfg.step_number);
                    if (gen_dac_seq_slot(slot, s_dac_next_step, chunk, last) != CV_OK) {
                        s_sweep_error = true;
                    }
                    s_dac_next_step += chunk;
                }
            }

            if (flags & AFEINTSRC_ENDSEQ) {
                AD5940_INTCClrFlag(AFEINTSRC_ENDSEQ);
                /* Drain any residual FIFO samples. */
                service_fifo(out);
            }
        } else {
            hang_count++;
            if (hang_count >= 2000000u) {
                uint32_t f0 = AD5940_INTCGetFlag(AFEINTC_0);
                uint32_t f1 = AD5940_INTCGetFlag(AFEINTC_1);
                printf("[HANG] samples=%lu INTC0=0x%lX INTC1=0x%lX\n",
                       (unsigned long)s_samples_received,
                       (unsigned long)f0,
                       (unsigned long)f1);
                hang_count = 0u;
            }
        }

        if (s_samples_received >= s_cfg.step_number) {
            s_sweep_complete = true;
        }
    }

    (void)stop_wupt();
    AD5940_SEQCtrlS(bFALSE);

    if (s_sweep_error) return CV_ERR_HAL;
    return CV_OK;
}

CvError cv_run_sweep(CvResult *out)
{
    if (!s_inited) return CV_ERR_NOT_INITED;
    if (out == NULL) return CV_ERR_PARAM;

    memset(out->current_ua, 0, sizeof(out->current_ua));
    out->sample_count     = 0u;
    out->completed_sweeps = 0u;
    out->has_rtia         = true;
    out->rtia_ohm         = s_rtia_ohm;
    out->rtia_phase_deg   = s_rtia_phase_deg;

    CvError e = run_one_sweep_accumulating(out);
    if (e != CV_OK) return e;
    out->completed_sweeps = 1u;
    return CV_OK;
}

CvError cv_run_averaged(CvResult *out)
{
    if (!s_inited) return CV_ERR_NOT_INITED;
    if (out == NULL) return CV_ERR_PARAM;
    if (s_cfg.num_avg_sweeps == 0u) return CV_ERR_PARAM;

    memset(out->current_ua, 0, sizeof(out->current_ua));
    out->sample_count     = 0u;
    out->completed_sweeps = 0u;
    out->has_rtia         = true;
    out->rtia_ohm         = s_rtia_ohm;
    out->rtia_phase_deg   = s_rtia_phase_deg;

    for (uint32_t n = 0u; n < s_cfg.num_avg_sweeps; n++) {
        CvError e = run_one_sweep_accumulating(out);
        if (e != CV_OK) {
            printf("[ERR] sweep %lu failed: %s\n",
                   (unsigned long)(n + 1u), cv_strerror(e));
            return e;
        }
        out->completed_sweeps++;
        printf("[INFO] sweep %lu/%lu complete\n",
               (unsigned long)out->completed_sweeps,
               (unsigned long)s_cfg.num_avg_sweeps);
    }
    return CV_OK;
}


/* ---------- CSV output ---------- */

void cv_emit_csv(const CvResult *r)
{
    if (r == NULL || r->sample_count == 0u || r->completed_sweeps == 0u) {
        printf("[ERR] cv_emit_csv: empty result\n");
        return;
    }

    printf("[INFO] Sweep complete: %lu samples, %lu sweeps averaged\n",
           (unsigned long)r->sample_count, (unsigned long)r->completed_sweeps);
    {
        int _r = (int)(r->rtia_ohm       * 100.0f + 0.5f);
        int _p = (int)(r->rtia_phase_deg * 100.0f +
                       (r->rtia_phase_deg >= 0.0f ? 0.5f : -0.5f));
        int _pf = _p % 100; if (_pf < 0) _pf = -_pf;
        printf("[INFO] RTIA cal: %d.%02d ohm, phase %d.%02d deg\n",
               _r / 100, _r % 100, _p / 100, _pf);
    }
    printf("Voltage_mV, Current_uA\n");

    const float n_inv = 1.0f / (float)r->completed_sweeps;
    const float sign = s_cfg.invert_current ? -1.0f : 1.0f;

    for (uint32_t i = 0u; i < r->sample_count; i++) {
        float v = voltage_for_step(i);
        float i_avg = r->current_ua[i] * n_inv * sign;
        {
            long _v = (long)(v     * 10.0f    + (v     >= 0.0f ? 0.5f : -0.5f));
            long _i = (long)(i_avg * 10000.0f + (i_avg >= 0.0f ? 0.5f : -0.5f));
            long va = _v < 0 ? -_v : _v; long ia = _i < 0 ? -_i : _i;
            printf("%s%ld.%01ld, %s%ld.%04ld\n",
                   _v < 0 ? "-" : "", va / 10, va % 10,
                   _i < 0 ? "-" : "", ia / 10000, ia % 10000);
        }
    }

    printf("[INFO] END_DATA\n");
}


/* ---------- Miscellaneous ---------- */

const char *cv_strerror(CvError e)
{
    switch (e) {
    case CV_OK:               return "ok";
    case CV_ERR_PARAM:        return "invalid parameter";
    case CV_ERR_HAL:          return "ad5940lib HAL error";
    case CV_ERR_WAKEUP:       return "AFE wakeup failed";
    case CV_ERR_SEQ_OVERFLOW: return "sequencer command buffer overflow";
    case CV_ERR_STEP_RANGE:   return "step count out of range";
    case CV_ERR_TIMEOUT:      return "sweep timeout";
    case CV_ERR_NOT_INITED:   return "cv_init not called";
    default:                  return "unknown";
    }
}

static void log_info(const char *msg)
{
    printf("%s\n", msg);
}
