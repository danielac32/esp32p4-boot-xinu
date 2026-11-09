/**
 * @file sdblock.h
 *
 * SD card device (SD0) for the ESP32-P4: a 512 byte block device on slot 0
 * of the SDMMC host controller, which is the board's microSD socket.
 *
 * Two layers make the stack.  This one owns the card: identification
 * (CMD0 ... ACMD6), the CID/CSD and capacity decode, the sector cursor and
 * the R1 error policy.  Below it the SDMMC0 host device (SDMMC_CTRL_*,
 * <sdmmc.h>) owns the hardware: pads, clock tree, command engine and the
 * IDMAC that moves each block.  The card layer never touches a register, and
 * this header is what the FAT library and the `sd` shell command consume.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SDBLOCK_H_
#define _SDBLOCK_H_

#include <stddef.h>
#include <stdint.h>
#include <conf.h>
#include <sdmmc.h>

#define SD_MAX_BLOCKS   0x00FFFFFFUL

/* ------------------------------------------------------------------ */
/* Device interface                                                    */
/* ------------------------------------------------------------------ */

/** Information about the card, for SD_CTRL_INFO. */
struct sdinfo
{
    uint32_t blocks;        /**< capacity in 512 byte blocks      */
    uint32_t card_clk_khz;  /**< bus clock in kHz while working   */
    uint16_t rca;           /**< relative card address            */
    uint8_t  present;       /**< 1 once a card answered           */
    uint8_t  highcap;       /**< 1 for SDHC/SDXC (block addressed)*/
    uint8_t  bus_width;     /**< 1 or 4 data lines                */
    uint8_t  reserved;
};

/* sdblkControl() functions */
#define SD_CTRL_PRESENT   0x30  /**< 1 when a card is usable            */
#define SD_CTRL_SEEK      0x31  /**< arg1 = LBA for the next read       */
#define SD_CTRL_BLOCKS    0x32  /**< capacity in 512 byte blocks        */
#define SD_CTRL_HIGHCAP   0x33  /**< 1 for block addressed cards        */
#define SD_CTRL_INFO      0x34  /**< arg1 = struct sdinfo *             */
#define SD_CTRL_RESET     0x35  /**< drop the card and identify it again*/
#define SD_CTRL_OCR       0x36  /**< operating conditions register (R7) */
#define SD_CTRL_CID       0x37  /**< arg1 = uint32_t[4], CID, MSB first */
#define SD_CTRL_CSD       0x38  /**< arg1 = uint32_t[4], CSD, MSB first */

/** Card state, for `sd stat` and for code that reports on the volume. */
extern bool sd_present;
extern bool sd_hc;                  /* SDHC/SDXC (block addressed)     */
extern uint32_t sd_rca;
extern uint32_t sd_blocks;
extern uint8_t sd_width;
extern uint32_t sd_ocr;
extern uint32_t sd_cid[4];          /* most significant word first     */
extern uint32_t sd_csd[4];

/* Convenience accessors, usable without going through the device table. */
uint32_t    sd_capacity_blocks(void);
int         sd_is_up(void);

devcall sdblkInit(device *);
devcall sdblkOpen(device *);
devcall sdblkClose(device *);
devcall sdblkRead(device *, void *, uint);
devcall sdblkWrite(device *, const void *, uint);
devcall sdblkSeek(device *, long);
devcall sdblkControl(device *, int, long, long);

#endif                          /* _SDBLOCK_H_ */
