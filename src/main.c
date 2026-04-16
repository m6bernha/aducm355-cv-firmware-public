/*
 * main.c — ADuCM355 bring-up for the CV sweep firmware
 *
 * Responsibilities:
 *   1. ClockInit  — select HFOSC, HCLK = PCLK = 26 MHz
 *   2. UartInit   — UART0 on P0.10/P0.11 at 230400 8N1 (printf target)
 *   3. Boot banner
 *   4. AD5940_MCUResourceInit — route the AFE Ext_Int3 IRQ
 *   5. cv_init    — configure the AFE, run RTIA self-cal
 *   6. cv_run_averaged + cv_emit_csv  — acquire and stream results
 *   7. Spin forever
 *
 * This firmware is a bring-up-only variant. There is no interactive
 * command loop; every boot runs exactly one averaged sweep and then
 * idles until the next reset. The intent is a minimal, reviewable
 * demonstration of the AD5940 LP path: any command or state-machine
 * layering belongs in a separate file that calls into cv_sweep.h.
 */

#include <stdio.h>

#include "ADuCM355.h"
#include "ClkLib.h"
#include "DioLib.h"
#include "UrtLib.h"
#include "ad5940.h"

#include "cv_sweep.h"


static void ClockInit(void)
{
    DigClkSel(DIGCLK_SOURCE_HFOSC);
    ClkDivCfg(1, 1);   /* HCLK = PCLK = 26 MHz */
}

static void UartInit(void)
{
    /* P0.10 = UART0-TX, P0.11 = UART0-RX, function select 1 */
    DioCfgPin(pADI_GPIO0, PIN10 | PIN11, 1);
    UrtCfg(pADI_UART0, B230400, (BITM_UART_COMLCR_WLS | 3), 0);
    UrtFifoCfg(pADI_UART0, RX_FIFO_1BYTE, BITM_UART_COMFCR_FIFOEN);
    UrtFifoClr(pADI_UART0, BITM_UART_COMFCR_RFCLR | BITM_UART_COMFCR_TFCLR);
}

int main(void)
{
    ClockInit();
    UartInit();

    printf("\n[BOOT] aducm355-cv-firmware\n");

    /* Sets up the Ext_Int3 pin (P2.1) that the AD5940 drives for AFE
     * interrupt notifications, and the glue that lets cv_sweep.c call
     * AD5940_GetMCUIntFlag. This function is part of the board-level
     * MCU wrapper that ships with ad5940lib. */
    AD5940_MCUResourceInit(0);
    printf("[BOOT] MCU resource init OK\n");

    /* Sweep parameters that matched the bench desktop potentiostat
     * used for reference measurements. See docs/architecture.md for
     * the rationale behind each value. */
    static CvConfig cfg = {
        .v_start_mv       = -300.0f,
        .v_peak_mv        = +700.0f,
        .vzero_mv         = 1100.0f,
        .step_number      = 2000u,
        .duration_ms      = 40000u,   /* 50 mV/s scan rate */
        .sample_delay_ms  = 10.0f,
        .lptia_rtia_sel   = LPTIARTIA_200R,
        .adc_pga_gain     = ADCPGA_1P5,
        .adc_sinc3_osr    = ADCSINC3OSR_2,
        .adc_ref_volt     = 1820.0f,
        .rcal_ohm         = 200.0f,
        .num_avg_sweeps   = 5u,
        .invert_current   = true,     /* LPTIA inverts; emit anodic-positive */
    };

    CvError e = cv_init(&cfg);
    if (e != CV_OK) {
        printf("[FATAL] cv_init: %s\n", cv_strerror(e));
        while (1) {}
    }

    static CvResult result;
    e = cv_run_averaged(&result);
    if (e != CV_OK) {
        printf("[FATAL] cv_run_averaged: %s\n", cv_strerror(e));
        while (1) {}
    }

    cv_emit_csv(&result);

    printf("[INFO] idle\n");
    while (1) {}
    return 0;
}
