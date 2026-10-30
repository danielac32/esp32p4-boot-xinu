/**
 * @file     xsh_i2c.c
 *
 * Shell command (i2c): probe/read/write devices on the shared I2C0 bus
 * through the I2C0 device (control()).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <i2c.h>

static bool i2c_parse(const char *s, unsigned long *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        return sscanf(s + 2, "%lx", out) == 1;
    }
    return sscanf(s, "%lu", out) == 1;
}

static int i2c_usage(char *name);

/**
 * @ingroup shell
 *
 * Shell command (i2c <cmd> ...).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_i2c(int nargs, char *args[])
{
    unsigned long addr, reg, len;
    struct i2c_req req;

    if (nargs < 2 || strcmp(args[1], "--help") == 0)
    {
        return i2c_usage(args[0]);
    }

    if (0 == strcmp(args[1], "scan") && nargs == 2)
    {
        unsigned long a;
        int found = 0;
        printf("scanning I2C0 (0x03..0x77)...\n");
        for (a = 0x03; a <= 0x77; a++)
        {
            if (control(I2C0, I2C_CTRL_PROBE, (long)a, 0) == OK)
            {
                printf("  found 0x%02lx\n", a);
                found++;
            }
        }
        printf("%d device(s) found\n", found);
        return OK;
    }

    if (0 == strcmp(args[1], "rd") && (nargs == 4 || nargs == 5))
    {
        uint8_t buf[64];
        unsigned long i;
        len = 1;
        if (!i2c_parse(args[2], &addr) || !i2c_parse(args[3], &reg) ||
            (nargs == 5 && !i2c_parse(args[4], &len)) ||
            len == 0 || len > sizeof(buf))
        {
            fprintf(stderr, "%s: invalid arguments\n", args[0]);
            return SYSERR;
        }
        req.reg = (uint16_t)reg;
        req.buf = buf;
        req.len = (size_t)len;
        if (control(I2C0, I2C_CTRL_READ_REGS, (long)addr, (long)&req) != OK)
        {
            fprintf(stderr, "%s: read failed\n", args[0]);
            return SYSERR;
        }
        printf("0x%02lx[0x%02lx] =", addr, reg);
        for (i = 0; i < len; i++)
        {
            printf(" %02x", buf[i]);
        }
        printf("\n");
        return OK;
    }

    if (0 == strcmp(args[1], "rd16") && (nargs == 4 || nargs == 5))
    {
        uint8_t buf[64];
        unsigned long i;
        len = 1;
        if (!i2c_parse(args[2], &addr) || !i2c_parse(args[3], &reg) ||
            (nargs == 5 && !i2c_parse(args[4], &len)) ||
            len == 0 || len > sizeof(buf))
        {
            fprintf(stderr, "%s: invalid arguments\n", args[0]);
            return SYSERR;
        }
        req.reg = (uint16_t)reg;
        req.buf = buf;
        req.len = (size_t)len;
        if (control(I2C0, I2C_CTRL_READ_REGS16, (long)addr, (long)&req) != OK)
        {
            fprintf(stderr, "%s: read failed\n", args[0]);
            return SYSERR;
        }
        printf("0x%02lx[0x%04lx] =", addr, reg);
        for (i = 0; i < len; i++)
        {
            printf(" %02x", buf[i]);
        }
        printf("\n");
        return OK;
    }

    if (0 == strcmp(args[1], "wr") && nargs == 5)
    {
        unsigned long val;
        uint8_t v;
        if (!i2c_parse(args[2], &addr) || !i2c_parse(args[3], &reg) ||
            !i2c_parse(args[4], &val))
        {
            fprintf(stderr, "%s: invalid arguments\n", args[0]);
            return SYSERR;
        }
        v = (uint8_t)val;
        req.reg = (uint16_t)reg;
        req.buf = &v;
        req.len = 1;
        if (control(I2C0, I2C_CTRL_WRITE_REG8, (long)addr, (long)&req) != OK)
        {
            fprintf(stderr, "%s: write failed\n", args[0]);
            return SYSERR;
        }
        printf("0x%02lx[0x%02lx] <- 0x%02lx\n", addr, reg, val);
        return OK;
    }

    if (0 == strcmp(args[1], "wr16") && nargs == 5)
    {
        unsigned long val;
        uint8_t v;
        if (!i2c_parse(args[2], &addr) || !i2c_parse(args[3], &reg) ||
            !i2c_parse(args[4], &val))
        {
            fprintf(stderr, "%s: invalid arguments\n", args[0]);
            return SYSERR;
        }
        v = (uint8_t)val;
        req.reg = (uint16_t)reg;
        req.buf = &v;
        req.len = 1;
        if (control(I2C0, I2C_CTRL_WRITE_REG16, (long)addr, (long)&req) != OK)
        {
            fprintf(stderr, "%s: write failed\n", args[0]);
            return SYSERR;
        }
        printf("0x%02lx[0x%04lx] <- 0x%02lx\n", addr, reg, val);
        return OK;
    }

    return i2c_usage(args[0]);
}

static int i2c_usage(char *name)
{
    printf("Usage: %s <command> [args]\n\n", name);
    printf("Commands:\n");
    printf("\tscan\n");
    printf("\trd <addr> <reg> [len]\n");
    printf("\trd16 <addr> <reg> [len]\n");
    printf("\twr <addr> <reg> <val>\n");
    printf("\twr16 <addr> <reg> <val>\n");
    printf("\t(addresses/registers/values accept decimal or 0x hex)\n");
    return SYSERR;
}
