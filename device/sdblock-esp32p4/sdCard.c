/**
 * @file sdCard.c
 *
 * Card side of the ESP32-P4 microSD stack: the CMD0 ... ACMD6 identification
 * sequence, the CID/CSD registers, the capacity decode and the card state
 * the SD0 device reports.
 *
 * Every hardware step goes through the SDMMC0 host device (<sdmmc.h>), so
 * this file never touches a register: commands are SDMMC_CTRL_CMD, the long
 * responses come back through SDMMC_CTRL_RESP, and the clock, the bus width
 * and the addressing mode are host settings the card chooses here.
 *
 * Two locks, in this order (card, then bus):
 *
 *   bus  (sdmmcBusLock, system/sdmmcbus.c) -- one transaction on the SDHOST
 *         block, which the ESP32-C6 on slot 1 will share with this card.
 *   card (sdCardLock, here)                -- the card itself: its cursor,
 *         its addressing mode, its RCA and the multi-command sequences
 *         (ACMD41/CMD13 wait loops) that only make sense if no other user
 *         of the card slips a command between their steps.  Several
 *         consumers exist -- the FAT device (shell, boot, any process that
 *         opens a file) and raw block users such as the `sd` command -- and
 *         the bus lock alone would let them interleave inside one card
 *         sequence.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <stdint.h>

#include <device.h>
#include <kernel.h>
#include <monitor.h>
#include <sdblock.h>
#include <sdmmc.h>

#include "sdCard.h"

/* ------------------------------------------------------------------ */
/* Card lock                                                          */
/* ------------------------------------------------------------------ */

static monitor sdcardmon = SYSERR;

void sdCardMonInit(void)
{
    if (SYSERR == sdcardmon)
    {
        sdcardmon = moncreate();
    }
}

void sdCardLock(void)
{
    if (SYSERR != sdcardmon)
    {
        lock(sdcardmon);
    }
}

void sdCardUnlock(void)
{
    if (SYSERR != sdcardmon)
    {
        unlock(sdcardmon);
    }
}

/* ------------------------------------------------------------------ */
/* Card state                                                         */
/* ------------------------------------------------------------------ */
bool sd_present = FALSE;
bool sd_hc = FALSE;                 /* SDHC/SDXC (block addressed)     */
uint32_t sd_rca;
uint32_t sd_blocks;
uint8_t sd_width = 1;
uint32_t sd_ocr;
uint32_t sd_cid[4];
uint32_t sd_csd[4];
uint32_t sd_lba;                    /* cursor of the SD0 device        */
const char *sd_fail = "start";      /* stage a failed probe stopped at */

/* ------------------------------------------------------------------ */
/* Commands, through the host device                                  */
/* ------------------------------------------------------------------ */

/* Record the stage a failed probe stopped at, so the boot message says
 * which part of the sequence the card stopped answering. */
#define SD_ID_FAIL(stage)   do { sd_fail = (stage); return FALSE; } while (0)

/** One command on the card.  \p resp is 0, 1 (short) or 2 (long). */
static bool sdCmd(uint idx, uint32_t arg, uint resp, bool crc, uint32_t *r0)
{
    struct sdcmd cmd;

    cmd.idx = (uint8_t)idx;
    cmd.resp = (uint8_t)resp;
    cmd.crc = crc ? 1 : 0;
    cmd.arg = arg;
    cmd.r0 = 0;

    if (SYSERR == control(SDMMC0, SDMMC_CTRL_CMD, (long)&cmd, 0))
    {
        return FALSE;
    }
    if (NULL != r0)
    {
        *r0 = cmd.r0;
    }
    return TRUE;
}

/** Command with a short response, checking the R1 error bits. */
static bool sdCmdR1(uint idx, uint32_t arg, bool ignore_errors, uint32_t *r0)
{
    uint32_t r;

    if (!sdCmd(idx, arg, 1, TRUE, &r))
    {
        return FALSE;
    }
    if ((r & SD_R1_ERR_MASK) != 0 && !ignore_errors)
    {
        return FALSE;
    }
    if (NULL != r0)
    {
        *r0 = r;
    }
    return TRUE;
}

/** CMD55 (<rca>) followed by an application command. */
static bool sdAppCmd(uint idx, uint32_t arg, uint resp, bool crc,
                     uint32_t *r0)
{
    if (!sdCmd(SD_CMD55_APP_CMD, sd_rca << 16, 1, TRUE, NULL))
    {
        return FALSE;
    }
    return sdCmd(idx, arg, resp, crc, r0);
}

/** Copy the controller's 136-bit response out, as the card numbers it. */
static bool sdLongResponse(uint32_t *dst)
{
    return OK == control(SDMMC0, SDMMC_CTRL_RESP, (long)dst, 0);
}

/** Decode the captured CSD to work out how many 512 byte blocks the card holds. */
static bool sdDecodeCsd(void)
{
    uint32_t c3 = sd_csd[0];      /* CSD[127:96] */
    uint32_t c2 = sd_csd[1];      /* CSD[95:64]  */
    uint32_t c1 = sd_csd[2];      /* CSD[63:32]  */

    if (((c3 >> 30) & 0x3U) == 1U)
    {
        /* CSD version 2.0: (C_SIZE + 1) * 512 KiB, in 512 byte blocks. */
        uint32_t csize = ((c2 & 0x3FU) << 16) | ((c1 >> 16) & 0xFFFFU);

        sd_blocks = (csize + 1U) * 1024U;
    }
    else
    {
        /* CSD version 1.0: (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN
         * bytes, converted to 512 byte blocks:
         *   (C_SIZE+1) << (C_SIZE_MULT + READ_BL_LEN - 7) */
        uint32_t csize = ((c2 & 0x3FFU) << 2) | ((c1 >> 30) & 0x3U);
        uint32_t mult = (c1 >> 15) & 0x7U;
        uint32_t blen = (c2 >> 16) & 0xFU;
        uint32_t shift = mult + blen;

        if (shift < 7)
        {
            return FALSE;
        }
        sd_blocks = (csize + 1U) << (shift - 7U);
    }
    return TRUE;
}

/** The identification dance, from the probe clock to a selected card. */
static bool sdIdentifyInner(void)
{
    uint32_t r;
    uint32_t t0;
    bool v2 = FALSE;        /**< card understands CMD8 (version 2.0) */

    sd_present = FALSE;
    sd_hc = FALSE;
    sd_rca = 0;
    sd_blocks = 0;
    sd_width = 1;

    if (SYSERR == control(SDMMC0, SDMMC_CTRL_RATE, SDMMC_RATE_PROBE, 0))
    {
        SD_ID_FAIL("probe clock");
    }
    /* Let the card see at least 74 clocks before the first command. */
    udelay(5000);

    /* CMD0: reset every card on the bus to the idle state. */
    if (!sdCmd(SD_CMD0_GO_IDLE, 0, 0, FALSE, NULL))
    {
        SD_ID_FAIL("CMD0");
    }
    udelay(2000);

    /* CMD8: voltage and pattern check.  It only exists on card version
     * 2, so its failure just means "version 1 card".  Version 2 cards
     * are the ones that understand ACMD6 (four data lines). */
    if (sdCmd(SD_CMD8_SEND_IF_COND, 0x1AA, 1, FALSE, &r)
        && ((r & 0xFFFU) == 0x1AAU))
    {
        v2 = TRUE;
    }

    /* ACMD41 until the card leaves its power-up busy state. */
    t0 = sdNow();
    for (;;)
    {
        uint32_t arg = (v2 ? 0x40FF8000UL : 0x00FF8000UL);

        if (!sdAppCmd(SD_ACMD41_SEND_OP_COND, arg, 1, FALSE, &r))
        {
            /* Naming whether CMD8 was understood too, because "no answer at
             * all" and "version 1 card" look the same from here. */
            SD_ID_FAIL(v2 ? "ACMD41" : "ACMD41 (CMD8 unanswered)");
        }
        if (r & (1UL << 31))
        {
            break;
        }
        if (sdTimedOut(t0, 2000))
        {
            SD_ID_FAIL("ACMD41 busy");
        }
        udelay(10000);
    }
    sd_ocr = r;
    sd_hc = (r & (1UL << 30)) != 0;

    /* The host builds the address of every CMD17/CMD24: block numbers on
     * high capacity cards, byte offsets on the older ones. */
    if (SYSERR == control(SDMMC0, SDMMC_CTRL_ADDRESSING, sd_hc ? 1 : 0, 0))
    {
        SD_ID_FAIL("addressing");
    }

    /* CMD2 asks for the CID, CMD3 hands back the relative address. */
    if (!sdCmd(SD_CMD2_ALL_SEND_CID, 0, 2, FALSE, NULL))
    {
        SD_ID_FAIL("CMD2");
    }
    if (!sdLongResponse(sd_cid))
    {
        SD_ID_FAIL("CMD2 response");
    }

    /* CMD3 hands back the address the card accepted.  Its response puts the
     * RCA in bits 31:16, which is where a plain R1 keeps its error bits, so
     * the response is read as data and not checked against SD_R1_ERR_MASK. */
    if (!sdCmd(SD_CMD3_SEND_RCA, 0, 1, TRUE, &r))
    {
        SD_ID_FAIL("CMD3");
    }
    sd_rca = (r >> 16) & 0xFFFFU;
    if (0 == sd_rca)
    {
        SD_ID_FAIL("CMD3 rca");
    }

    /* While still in the identification state, read the CSD for the
     * capacity.  This has to happen before CMD7 selects the card. */
    if (!sdCmd(SD_CMD9_SEND_CSD, sd_rca << 16, 2, FALSE, NULL))
    {
        SD_ID_FAIL("CMD9");
    }
    if (!sdLongResponse(sd_csd))
    {
        SD_ID_FAIL("CMD9 response");
    }
    if (!sdDecodeCsd())
    {
        SD_ID_FAIL("CSD");
    }

    /* CMD7 selects the card, moving it to the transfer state. */
    if (!sdCmdR1(SD_CMD7_SELECT, sd_rca << 16, FALSE, NULL))
    {
        SD_ID_FAIL("CMD7");
    }

    /* Byte addressed cards take a block length of their own.  That can
     * only be done from the stand-by or transfer state, so it happens
     * here rather than during power up. */
    if (!sd_hc)
    {
        if (!sdCmdR1(SD_CMD16_SET_BLOCKLEN, SD_BLOCK_SIZE, FALSE, NULL))
        {
            SD_ID_FAIL("CMD16");
        }
    }

    /* ACMD6 only means something on version 2 cards. */
    if (v2)
    {
        if (!sdAppCmd(SD_ACMD6_SET_BUS_WIDTH, 2, 1, TRUE, NULL))
        {
            SD_ID_FAIL("ACMD6");
        }
        /* The card is on four data lines now, so DAT3 finally joins the
         * bus and the controller switches to the wide bus. */
        if (SYSERR == control(SDMMC0, SDMMC_CTRL_WIDTH, 4, 0))
        {
            SD_ID_FAIL("bus width");
        }
        sd_width = 4;
    }

    /* Up to the working clock now that the card is out of its slow
     * identification mode. */
    if (SYSERR == control(SDMMC0, SDMMC_CTRL_RATE, SDMMC_RATE_WORK, 0))
    {
        SD_ID_FAIL("work clock");
    }

    control(SDMMC0, SDMMC_CTRL_SEEK, 0, 0);
    sd_lba = 0;
    sd_present = TRUE;
    return TRUE;
}

/**
 * Identify the card as one operation: no other card user may interleave a
 * command inside the sequence, and the C6 must not slip a transaction on
 * the shared controller between its steps either.  Both monitors are
 * reentrant, so the host device taking and releasing the bus lock per
 * command nests inside these.
 */
bool sdIdentify(void)
{
    bool ok;

    sdCardLock();
    sdmmcBusLock();
    ok = sdIdentifyInner();
    sdmmcBusUnlock();
    sdCardUnlock();
    return ok;
}

/* ------------------------------------------------------------------ */

/* Convenience accessors                                              */
/* ------------------------------------------------------------------ */

uint32_t sd_capacity_blocks(void)
{
    return sd_present ? sd_blocks : 0;
}

int sd_is_up(void)
{
    return sd_present ? 1 : 0;
}
