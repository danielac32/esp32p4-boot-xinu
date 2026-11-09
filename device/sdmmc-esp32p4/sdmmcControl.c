/**
 * @file sdmmcControl.c
 *
 * SDMMC host device (SDMMC0) for the ESP32-P4: control.
 *
 * These are the mechanical operations the card layer needs: set the card
 * clock, set the bus width, send a raw command and read back its response,
 * declare how the card is addressed, move the host cursor and report what
 * the controller is doing.  Everything here runs under the shared bus lock.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <device.h>
#include <stddef.h>
#include <sdmmc.h>

#include "sdhost.h"

/** Map a symbolic rate to this slot's CLKDIV divider. */
static bool sdmmcSetRate(long rate)
{
    uint8_t div;
    bool ok;

    switch (rate)
    {
    case SDMMC_RATE_PROBE:
        div = SD_CARD_DIV_PROBE;
        break;
    case SDMMC_RATE_WORK:
        div = SD_CARD_DIV_WORK;
        break;
    default:
        return FALSE;
    }

    sdmmcBusLock();
    ok = sdSetClock(div);
    sdmmcBusUnlock();
    return ok;
}

devcall sdmmcControl(device *devptr, int func, long arg1, long arg2)
{
    (void)devptr;
    (void)arg2;

    if (!sd_host_ready)
    {
        return SYSERR;
    }

    switch (func)
    {
    case SDMMC_CTRL_SEEK:
        if (arg1 < 0)
        {
            return SYSERR;
        }
        sd_host_lba = (uint32_t)arg1;
        return OK;

    case SDMMC_CTRL_RATE:
        return sdmmcSetRate(arg1) ? OK : SYSERR;

    case SDMMC_CTRL_WIDTH:
    {
        bool ok;

        sdmmcBusLock();
        ok = sdSetWidth((uint)arg1);
        sdmmcBusUnlock();
        return ok ? OK : SYSERR;
    }

    case SDMMC_CTRL_CMD:
    {
        struct sdcmd *cmd = (struct sdcmd *)arg1;
        uint32_t r = 0;
        bool ok;

        if (NULL == cmd)
        {
            return SYSERR;
        }
        sdmmcBusLock();
        ok = sdSendCmd(cmd->idx, cmd->arg, cmd->resp, cmd->crc != 0, &r);
        sdmmcBusUnlock();
        if (!ok)
        {
            return SYSERR;
        }
        cmd->r0 = r;
        return OK;
    }

    case SDMMC_CTRL_RESP:
    {
        uint32_t *dst = (uint32_t *)arg1;

        if (NULL == dst)
        {
            return SYSERR;
        }
        sdmmcBusLock();
        sdLongResp(dst);
        sdmmcBusUnlock();
        return OK;
    }

    case SDMMC_CTRL_ADDRESSING:
        sd_host_hc = (arg1 != 0);
        return OK;

    case SDMMC_CTRL_INFO:
    {
        struct sdmmcinfo *info = (struct sdmmcinfo *)arg1;

        if (NULL == info)
        {
            return SYSERR;
        }
        info->host_khz = SD_HOST_HZ / 1000UL;
        info->card_khz = sd_clk_khz;
        info->width = sd_host_width;
        info->ready = sd_host_ready ? 1 : 0;
        info->reserved[0] = 0;
        info->reserved[1] = 0;
        return OK;
    }

    default:
        return SYSERR;
    }
}
