/**
 * @file     xsh_sd.c
 *
 * Shell command (sd): show the microSD state via the raw SD driver (SLOT0 of
 * the SDMMC controller, shared with the ESP32-C6).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <drivers/sd/sd.h>

/**
 * @ingroup shell
 *
 * Shell command (sd).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_sd(int nargs, char *args[])
{
    uint32 blocks;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tShows the microSD state and capacity.\n");
        printf("Options:\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs > 1)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    if (!sd_is_up())
    {
        printf("microSD : not initialized\n");
        return OK;
    }

    blocks = sd_capacity_blocks();
    printf("microSD : up\n");
    printf("blocks  : %lu (512 B each)\n", (unsigned long)blocks);
    printf("size    : %lu MiB\n", (unsigned long)(blocks / 2048UL));

    return OK;
}
