/**
 * @file     xsh_reset.c
 *
 * Shell command (reset): trigger a full SoC reset on the ESP32-P4.
 *
 * The P4 has no "RTC_CNTL SW_SYS_RST" like older parts: the HP core 0
 * software reset lives in LP_AON_CLKRST, register HPCPU_RESET_CTRL0
 * (offset 0x14), bit 13 HPCORE0_SW_RESET (write-1-to-trigger).  The reset
 * cause reported afterwards is 6'h3, "digital system software reset".
 * The SoC then reboots from the ROM, i.e. the SBL runs again.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <kernel.h>
#include <interrupt.h>
#include <esp32p4.h>

/* LP_AON_CLKRST register block: the first six registers are LP_CLK_CONF,
 * LP_CLK_PO_EN, LP_CLK_EN, LP_RST_EN, RESET_CAUSE and HPCPU_RESET_CTRL0. */
#define LP_AONCLKRST_HPCPU_RESET_CTRL0  (LP_AON_CLKRST_BASE + 0x14UL)
#define LP_AONCLKRST_HPCORE0_SW_RESET   (1UL << 13)

/**
 * @ingroup shell
 *
 * Shell command (reset).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_reset(int nargs, char *args[])
{
    irqmask im;
    volatile unsigned long spin;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tPerforms a full reset (reboot) of the ESP32-P4.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        fprintf(stderr, "Try '%s --help' for more information\n", args[0]);
        return SYSERR;
    }

    printf("reset: rebooting the SoC...\n");

    im = disable();
    *(volatile uint32_t *)LP_AONCLKRST_HPCPU_RESET_CTRL0 =
        LP_AONCLKRST_HPCORE0_SW_RESET;

    /* The write resets the chip almost immediately; if we ever get past this
     * spin the reset did not fire, so report it instead of hanging. */
    for (spin = 0; spin < 4000000UL; spin++)
    {
    }
    restore(im);

    fprintf(stderr, "%s: software reset did not fire\n", args[0]);
    return SYSERR;
}
