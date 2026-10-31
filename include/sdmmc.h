/*
 * sdmmc.h — candado del CONTROLADOR SDMMC compartido (microSD + ESP32-C6).
 *
 * La microSD (slot 0, device SD0 en <sd.h>) y el C6 (slot 1,
 * drivers/sdio/esp_hosted_sdio.c) usan el MISMO controlador SDMMC: sus
 * transacciones hay que serializarlas.
 *
 * El candado es un MONITOR del kernel y lo POSEE el kernel en un sitio
 * neutral: system/sdmmcbus.c, creado en el arranque (sysinit) antes de
 * inicializar los devices.  Las librerías crudas de drivers/ no crean
 * semáforos/monitores: sólo llaman a sdmmcBusLock()/sdmmcBusUnlock()
 * alrededor de cada transacción.  El monitor es reentrante (dueño +
 * profundidad), así que una rutina que ya lo tiene puede volver a pedirlo.
 *
 * Antes de sdmmcBusInit() (arranque, monohilo) el candado es no-op: no hay
 * nada que serializar.
 */
#ifndef _SDMMC_H_
#define _SDMMC_H_

#include <clock.h>
#include <conf.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Shared bus lock                                                     */
/* ------------------------------------------------------------------ */

/* Crea el monitor del controlador (idempotente). */
void sdmmcBusInit(void);

/* Toma/suelta el candado del controlador (no-op si aún no existe). */
void sdmmcBusLock(void);
void sdmmcBusUnlock(void);

/* ------------------------------------------------------------------ */
/* Host device (SDMMC0)                                                */
/* ------------------------------------------------------------------ */
/* The SDHOST block is itself a device: the mechanical layer that owns the
 * pads, the clock tree, the command engine and the IDMAC.  The card layer
 * (device SD0, <sdblock.h>) drives it only through this interface, so the
 * card code never touches a register.
 *
 * The host keeps a cursor of its own: read() and write() move blocks at
 * that LBA and advance it.  The card keeps its own cursor on top and pushes
 * it down (SDMMC_CTRL_SEEK) before every transfer.
 */

/* One sector: the block a command moves, in either direction. */
#define SD_BLOCK_SIZE   512

struct sdcmd
{
    uint32_t arg;                   /**< command argument                   */
    uint32_t r0;                    /**< the short response, filled in      */
    uint8_t  idx;                   /**< command index, 0..63               */
    uint8_t  resp;                  /**< 0 none, 1 short (48 bit), 2 long   */
    uint8_t  crc;                   /**< ask for response CRC checking      */
    uint8_t  pad;
};

/** What the host is doing, for SD_CTRL_INFO and diagnostics. */
struct sdmmcinfo
{
    uint32_t host_khz;              /**< block clock fed to both slots      */
    uint32_t card_khz;              /**< this slot's card clock             */
    uint8_t  width;                 /**< data lines the host drives (1 or 4)*/
    uint8_t  ready;                 /**< 1 once the controller came up      */
    uint8_t  reserved[2];
};

/* Symbolic clock rates, so the card layer does not carry divider numbers. */
#define SDMMC_RATE_PROBE    0       /**< 400 kHz, identification            */
#define SDMMC_RATE_WORK     1       /**< 20 MHz, transfer state             */

/* sdmmcControl() functions. */
#define SDMMC_CTRL_SEEK     0x60    /**< arg1 = LBA of the next transfer    */
#define SDMMC_CTRL_RATE     0x61    /**< arg1 = SDMMC_RATE_*                */
#define SDMMC_CTRL_WIDTH    0x62    /**< arg1 = 1 or 4 data lines           */
#define SDMMC_CTRL_CMD      0x63    /**< arg1 = struct sdcmd *              */
#define SDMMC_CTRL_RESP     0x64    /**< arg1 = uint32_t[4], last long resp */
#define SDMMC_CTRL_ADDRESSING 0x65  /**< arg1: 0 byte addressed, 1 blocks   */
#define SDMMC_CTRL_INFO     0x66    /**< arg1 = struct sdmmcinfo *          */

/* ------------------------------------------------------------------ */
/* SD protocol                                                        */
/* ------------------------------------------------------------------ */
#define SD_CMD0_GO_IDLE            0
#define SD_CMD2_ALL_SEND_CID       2
#define SD_CMD3_SEND_RCA           3
#define SD_CMD7_SELECT             7
#define SD_CMD8_SEND_IF_COND       8
#define SD_CMD9_SEND_CSD           9
#define SD_CMD13_SEND_STATUS       13
#define SD_CMD16_SET_BLOCKLEN      16
#define SD_CMD17_READ_SINGLE       17
#define SD_CMD24_WRITE_SINGLE      24
#define SD_CMD55_APP_CMD           55
#define SD_ACMD6_SET_BUS_WIDTH     6
#define SD_ACMD41_SEND_OP_COND     41

/* R1 error bits and card states. */
#define SD_R1_ERR_MASK             0xFEF80000UL
#define SD_R1_READY_FOR_DATA       (1UL << 8)
#define SD_R1_STATE(r)             (((r) >> 9) & 0xFU)
#define SD_R1_STATE_TRAN           4U

/* The device operations of SDMMC0. */
devcall sdmmcInit(device *);
devcall sdmmcOpen(device *);
devcall sdmmcClose(device *);
devcall sdmmcRead(device *, void *, uint);
devcall sdmmcWrite(device *, const void *, uint);
devcall sdmmcSeek(device *, long);
devcall sdmmcControl(device *, int, long, long);

/* ------------------------------------------------------------------ */
/* Transaction errors                                                  */
/* ------------------------------------------------------------------ */

/**
 * Why the last transaction failed: a tag in the high nibble plus the
 * RINTSTS or IDSTS bits the controller held at that moment.  The `sd` shell
 * command reports it when a transfer fails.
 */
extern uint32_t sd_hwerr;
#define SD_HWERR_INHIBIT   0x10000000UL   /* CMD_START never cleared       */
#define SD_HWERR_ERR       0x20000000UL   /* a command or data error       */
#define SD_HWERR_TIMEOUT   0x30000000UL   /* no CMD_DONE, card silent      */
#define SD_HWERR_DMA       0x40000000UL   /* the DMA engine gave up        */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t sdNow(void)
{
    return (uint32_t)clkcount();
}

/** True once \p ms milliseconds have passed since \p start. */
static inline bool sdTimedOut(uint32_t start, uint ms)
{
    uint32_t cycles = ms * (platform.clkfreq / 1000UL);

    return (uint32_t)(sdNow() - start) > cycles;
}

#endif                          /* _SDMMC_H_ */
