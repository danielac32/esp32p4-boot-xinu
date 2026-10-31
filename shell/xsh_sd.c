/**
 * @file xsh_sd.c
 *
 * Shell command (sd): show what the microSD card answered during
 * identification and dump the sectors it hands back, resolving the device by
 * path so "/dev/sd0" is the only name the user has to know.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <sdblock.h>
#include <shell.h>

/* Out of the shell's small stack: one sector, the register being shown and
 * the copy a write is verified against. */
static uint8_t  sector[SD_BLOCK_SIZE];
static uint8_t  check[SD_BLOCK_SIZE];
static uint32_t reg[4];

/** A sector pattern that identifies the block it belongs to. */
static void fillPattern(uint32_t lba)
{
    uint i;

    for (i = 0; i < SD_BLOCK_SIZE; i++)
    {
        sector[i] = (uint8_t)(lba + i * 7U + 0x5AU);
    }
}

/**
 * Show a 136-bit response the way the card numbers it: word 0 holds bits
 * 127:96, so the CID fields read from the top down.
 */
static void showCid(void)
{
    char pnm[6];
    uint32_t psn;
    int i;

    pnm[0] = (char)(reg[0] & 0xFFU);
    for (i = 1; i <= 4; i++)
    {
        pnm[i] = (char)((reg[1] >> (8 * (4 - i))) & 0xFFU);
    }
    pnm[5] = '\0';

    /* CID bits: PNM 103:64, PRV 63:56, PSN 55:24, MDT 19:8.  The name ends
     * one byte before word 1 does, so the revision is the top byte of word 2
     * and the serial is what is left of word 2 plus the top byte of word 3. */
    psn = ((reg[2] & 0x00FFFFFFUL) << 8) | (reg[3] >> 24);

    printf("  cid   : %08X %08X %08X %08X\n", reg[0], reg[1], reg[2], reg[3]);
    printf("  card  : \"%s\" rev %u.%u  manuf 0x%02X '%c%c'  sn %lu  mfg %u/%02u\n",
           pnm,
           (unsigned)((reg[2] >> 28) & 0xFU), (unsigned)((reg[2] >> 24) & 0xFU),
           (unsigned)((reg[0] >> 24) & 0xFFU),
           (char)((reg[0] >> 16) & 0xFFU), (char)((reg[0] >> 8) & 0xFFU),
           (unsigned long)psn,
           1997U + (unsigned)((reg[3] >> 12) & 0xFU),
           (unsigned)((reg[3] >> 8) & 0xFU));
}

/** One sector, sixteen bytes to a line: offset, hex, then printable text. */
static void dumpSector(uint32_t lba)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    char line[80];
    uint32_t row;

    for (row = 0; row < SD_BLOCK_SIZE; row += 16)
    {
        char *p = line;
        uint16_t col;

        p += sprintf(p, "%08lX  ", (unsigned long)(lba * SD_BLOCK_SIZE + row));
        for (col = 0; col < 16; col++)
        {
            uint8_t b = sector[row + col];

            if (8 == col)
            {
                *p++ = ' ';          /* split the line into two halves */
            }
            *p++ = hexdigits[b >> 4];
            *p++ = hexdigits[b & 0xFU];
            *p++ = ' ';
        }
        *p++ = ' ';
        *p++ = ' ';
        for (col = 0; col < 16; col++)
        {
            uint8_t b = sector[row + col];

            *p++ = ((b >= 0x20 && b < 0x7F) ? (char)b : '.');
        }
        *p = '\0';
        printf("  %s\n", line);
    }
}

/**
 * @ingroup shell
 *
 * Shell command (sd).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return OK for success, SYSERR for a syntax or a device error
 */
shellcmd xsh_sd(int nargs, char *args[])
{
    struct sdinfo info;
    unsigned ocr;
    int dev;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s\n\n", args[0]);
        printf("Description:\n");
        printf("\tShows the microSD identity and dumps its sectors.\n");
        printf("Options:\n");
        printf("\tstat\tidentity: capacity, bus, OCR, CID and CSD\n");
        printf("\trd <lba> [blocks]\thexdump of 1 to 16 sectors (default 1)\n");
        printf("\twr <lba>\twrite one sector, read it back and compare\n");
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    dev = getdev("/dev/sd0");
    if (SYSERR == dev)
    {
        fprintf(stderr, "sd: /dev/sd0 is not mounted\n");
        return SYSERR;
    }

    if (1 == nargs || 0 == strcmp(args[1], "stat"))
    {
        if (SYSERR == control(dev, SD_CTRL_INFO, (long)&info, 0))
        {
            fprintf(stderr, "sd: card not available\n");
            return SYSERR;
        }
        printf("microSD : slot 0, %lu blocks (%lu MiB)\n",
               (unsigned long)info.blocks,
               (unsigned long)(info.blocks / 2048UL));
        printf("type    : %s, %u-bit bus, %lu kHz\n",
               info.highcap ? "SDHC/SDXC (block addressed)"
                            : "SDSC (byte addressed)",
               info.bus_width, (unsigned long)info.card_clk_khz);
        printf("rca     : 0x%04X\n", info.rca);

        ocr = (unsigned)control(dev, SD_CTRL_OCR, 0, 0);
        printf("ocr     : %08X  %s%s, window %s\n", ocr,
               (ocr & 0x80000000U) ? "ready" : "busy",
               (ocr & 0x40000000U) ? ", sector access" : ", byte access",
               (ocr & 0x00FF8000U) ? "2.7-3.6V" : "1.65-1.95V");

        if (OK == control(dev, SD_CTRL_CID, (long)reg, 0))
        {
            showCid();
        }
        if (OK == control(dev, SD_CTRL_CSD, (long)reg, 0))
        {
            printf("csd     : %08X %08X %08X %08X\n",
                   reg[0], reg[1], reg[2], reg[3]);
        }
        return OK;
    }

    if (0 == strcmp(args[1], "rd"))
    {
        uint32_t lba;
        uint32_t count;
        uint32_t i;

        if (nargs < 3)
        {
            fprintf(stderr, "sd: usage: sd rd <lba> [blocks]\n");
            return SYSERR;
        }
        lba = (uint32_t)atoi(args[2]);
        count = (nargs > 3) ? (uint32_t)atoi(args[3]) : 1U;
        if (0 == count || count > 16)
        {
            fprintf(stderr, "sd: blocks must be 1 to 16\n");
            return SYSERR;
        }

        if (SYSERR == control(dev, SD_CTRL_SEEK, (long)lba, 0))
        {
            fprintf(stderr, "sd: seek to %lu failed\n", (unsigned long)lba);
            return SYSERR;
        }
        for (i = 0; i < count; i++)
        {
            if (SYSERR == read(dev, sector, SD_BLOCK_SIZE))
            {
                fprintf(stderr, "sd: read of block %lu failed (hwerr 0x%08X)\n",
                        (unsigned long)(lba + i), (unsigned)sd_hwerr);
                return SYSERR;
            }
            dumpSector(lba + i);
        }
        return OK;
    }

    if (0 == strcmp(args[1], "wr"))
    {
        uint32_t lba;
        uint32_t count;
        uint32_t n;
        uint i;

        if (nargs < 3)
        {
            fprintf(stderr, "sd: usage: sd wr <lba> [blocks]\n");
            return SYSERR;
        }
        lba = (uint32_t)atoi(args[2]);
        count = (nargs > 3) ? (uint32_t)atoi(args[3]) : 1U;
        if (0 == count || count > 16)
        {
            fprintf(stderr, "sd: blocks must be 1 to 16\n");
            return SYSERR;
        }

        for (n = 0; n < count; n++)
        {
            fillPattern(lba + n);
            if (SYSERR == control(dev, SD_CTRL_SEEK, (long)(lba + n), 0)
                || SYSERR == write(dev, sector, SD_BLOCK_SIZE))
            {
                fprintf(stderr, "sd: write of block %lu failed (hwerr 0x%08X)\n",
                        (unsigned long)(lba + n), (unsigned)sd_hwerr);
                return SYSERR;
            }
        }

        /* Read them back: the card is the only witness that the bytes got
         * there, and the CRC on the way back covers the card's own copy. */
        for (n = 0; n < count; n++)
        {
            if (SYSERR == control(dev, SD_CTRL_SEEK, (long)(lba + n), 0)
                || SYSERR == read(dev, check, SD_BLOCK_SIZE))
            {
                fprintf(stderr, "sd: read back of block %lu failed "
                        "(hwerr 0x%08X)\n",
                        (unsigned long)(lba + n), (unsigned)sd_hwerr);
                return SYSERR;
            }
            for (i = 0; i < SD_BLOCK_SIZE; i++)
            {
                if (check[i] != (uint8_t)(lba + n + i * 7U + 0x5AU))
                {
                    fprintf(stderr, "sd: block %lu differs at byte %lu "
                            "(0x%02X, expected 0x%02X)\n",
                            (unsigned long)(lba + n), (unsigned long)i,
                            check[i],
                            (uint8_t)(lba + n + i * 7U + 0x5AU));
                    return SYSERR;
                }
            }
        }
        printf("sd: %lu block(s) from %lu written and verified\n",
               (unsigned long)count, (unsigned long)lba);
        return OK;
    }

    fprintf(stderr, "sd: unknown argument: %s\n", args[1]);
    return SYSERR;
}
