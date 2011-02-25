#ifndef __ASM_GENERIC_SERIAL_H
#define __ASM_GENERIC_SERIAL_H

/*
 * This should not be an architecture specific #define, oh well.
 *
 * Traditionally, it just describes i8250 and related serial ports
 * that have this clock rate.
 */

#if defined(CONFIG_BRCMSTB)

/* Base frequency for STB 16550 UART */
#define BASE_BAUD (81000000 / 16)

#else

#define BASE_BAUD (1843200 / 16)

#endif

#endif /* __ASM_GENERIC_SERIAL_H */
