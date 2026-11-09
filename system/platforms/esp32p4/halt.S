/**
 * @file halt.S
 *
 * halt() del ESP32-P4: en vez de quedarse mudo, REINICIA el SoC.  El P4 no
 * tiene el "RTC_CNTL SW_SYS_RST" de los chips viejos: el reset por software
 * del HP core 0 vive en LP_AON_CLKRST, registro HPCPU_RESET_CTRL0 (offset
 * 0x14), bit 13 HPCORE0_SW_RESET, escribir-1-para-disparar (la misma receta
 * de shell/xsh_reset.c).  El SoC rearranca desde la ROM, o sea que el SBL
 * corre otra vez y la placa vuelve al arranque normal.
 */
/* Embedded Xinu, Copyright (C) 2018.  All rights reserved. */

	.globl halt

/**
 * @fn void halt(void)
 *
 * Halt the system by rebooting the SoC.
 */
halt:
	.func halt

	li	t0, 0x50111014		/* LP_AON_CLKRST_BASE + 0x14             */
	li	t1, 0x2000		/* bit 13: HPCORE0_SW_RESET              */
	sw	t1, 0(t0)

_loop:
	/* Si el reset no disparo (nunca visto), esperar interrupciones. */
	wfi
	j _loop
	.endfunc
