/*
 * retarget_uart.c — newlib _write syscall routed to UART0
 *
 * Enables printf/puts/fputs to work on bare-metal by forwarding stdout
 * (fd = 1) and stderr (fd = 2) to UART0 using polling transmit.
 *
 * Hook-in: build.bat passes --specs=nano.specs --specs=nosys.specs.
 * nosys.specs provides a weak _write stub. Our strong _write symbol
 * in this translation unit overrides the stub because our .o appears
 * before the nosys library on the link command line.
 *
 * UART0 must already be configured (UartInit called from main) before
 * any printf output will work.
 */

#include <stdint.h>
#include <unistd.h>       /* STDOUT_FILENO (1), STDERR_FILENO (2) */
#include "ADuCM355.h"

int _write(int fd, const char *buf, int len)
{
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) return -1;

    for (int i = 0; i < len; i++) {
        /* Poll until the TX holding register is empty. */
        while ((pADI_UART0->COMLSR & BITM_UART_COMLSR_THRE) == 0) { }
        pADI_UART0->COMTX = (uint8_t)buf[i];
    }
    return len;
}
