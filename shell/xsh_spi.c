/**
 * @file     xsh_spi.c
 *
 * Shell command (spi): write/read raw bytes on the SPI2 bus through the SPI2
 * device (control()).  The LCD shares this bus; keep transfers short.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <spi.h>

#define SPI_MAX_XFER 64

static bool spi_hexbyte(const char *s, uint8_t *out)
{
    unsigned v;
    if (sscanf(s, "%x", &v) != 1 || v > 0xFF)
    {
        return FALSE;
    }
    *out = (uint8_t)v;
    return TRUE;
}

static int spi_collect(int nargs, char *args[], int first, uint8_t *buf, int cap)
{
    int n = 0;
    int i;
    for (i = first; i < nargs; i++)
    {
        if (n >= cap)
        {
            return -1;
        }
        if (!spi_hexbyte(args[i], &buf[n]))
        {
            return -2;
        }
        n++;
    }
    return n;
}

static int spi_usage(char *name);

/**
 * @ingroup shell
 *
 * Shell command (spi <cmd> ...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_spi(int nargs, char *args[])
{
    uint8_t buf[SPI_MAX_XFER];

    if (nargs < 2 || strcmp(args[1], "--help") == 0)
    {
        return spi_usage(args[0]);
    }

    if (0 == strcmp(args[1], "write") && nargs >= 3)
    {
        int n = spi_collect(nargs, args, 2, buf, SPI_MAX_XFER);
        if (n < 0)
        {
            fprintf(stderr, "%s: bad byte list\n", args[0]);
            return SYSERR;
        }
        if (control(SPI2, SPI_CTRL_WRITE, (long)buf, (long)n) != OK)
        {
            fprintf(stderr, "%s: write failed\n", args[0]);
            return SYSERR;
        }
        printf("%s: wrote %d byte(s)\n", args[0], n);
        return OK;
    }

    if (0 == strcmp(args[1], "read") && nargs == 3)
    {
        unsigned n;
        unsigned i;
        if (sscanf(args[2], "%u", &n) != 1 || n == 0 || n > SPI_MAX_XFER)
        {
            fprintf(stderr, "%s: invalid length\n", args[0]);
            return SYSERR;
        }
        if (control(SPI2, SPI_CTRL_READ, (long)buf, (long)n) != OK)
        {
            fprintf(stderr, "%s: read failed\n", args[0]);
            return SYSERR;
        }
        for (i = 0; i < n; i++)
        {
            printf("%02x%c", buf[i], (i + 1 == n) ? '\n' : ' ');
        }
        return OK;
    }

    if (0 == strcmp(args[1], "xfer") && nargs >= 3)
    {
        struct spi_xfer x;
        uint8_t rx[SPI_MAX_XFER];
        unsigned i;
        int n = spi_collect(nargs, args, 2, buf, SPI_MAX_XFER);
        if (n < 0)
        {
            fprintf(stderr, "%s: bad byte list\n", args[0]);
            return SYSERR;
        }
        x.tx = buf;
        x.rx = rx;
        x.len = (size_t)n;
        if (control(SPI2, SPI_CTRL_TRANSFER, (long)&x, 0) != OK)
        {
            fprintf(stderr, "%s: transfer failed\n", args[0]);
            return SYSERR;
        }
        printf("rx:");
        for (i = 0; i < (unsigned)n; i++)
        {
            printf(" %02x", rx[i]);
        }
        printf("\n");
        return OK;
    }

    return spi_usage(args[0]);
}

static int spi_usage(char *name)
{
    printf("Usage: %s <command> [args]\n\n", name);
    printf("Commands:\n");
    printf("\twrite <hh> [hh...]   send hex bytes\n");
    printf("\tread <n>             clock in n bytes (max 64)\n");
    printf("\txfer <hh> [hh...]    full-duplex, print received\n");
    return SYSERR;
}
