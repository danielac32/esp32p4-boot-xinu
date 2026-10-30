/**
 * @file xdone.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stdio.h>

extern void halt(void);

/**
 * @ingroup misc
 *
 * Print system completion message as last thread exits.
 */
void xdone(void)
{
    kprintf("\r\n\r\nAll user processes have completed.\r\n\r\n");
    disable();
    halt();
}
