/**
 * @file spi.h
 *
 * SPI2 master device.  The raw driver lives in drivers/spi/; this device
 * serializes access (shared by the LCD) with a monitor and exposes
 * transfers through control().
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SPI_H_
#define _SPI_H_

#include <device.h>
#include <drivers/spi/spi.h>

/* control(SPI2, CMD, arg1, arg2) */
#define SPI_CTRL_WRITE     0x01 /**< arg1=(const uint8_t*)data arg2=len        */
#define SPI_CTRL_READ      0x02 /**< arg1=(uint8_t*)data arg2=len              */
#define SPI_CTRL_TRANSFER  0x03 /**< arg1=&struct spi_xfer (full-duplex)       */

/** Full-duplex SPI transfer.  @c tx may be NULL (send 0xFF) and @c rx may
 *  be NULL (discard); they may point to the same buffer. */
struct spi_xfer
{
    const uint8_t *tx;
    uint8_t *rx;
    size_t len;
};

/* Driver functions */
devcall spiInit(device *);
devcall spiControl(device *, int, long, long);

/* Serialize raw access to the shared SPI2 bus.  On-bus devices that talk to
 * the raw driver directly (the LCD) bracket their transactions with these so
 * they share the single bus monitor owned by device/spi. */
void spiBusLock(void);
void spiBusUnlock(void);

#endif                          /* _SPI_H_ */
