/**
 * @file disk.h
 *
 * Low level disk interface for FAT File IO Library (C)ChaN, 2014.  The three
 * hooks below are what fatfs/disk.c provides to the library: one capacity
 * query and one sector run in each direction.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _DISKIO_DEFINED
#define _DISKIO_DEFINED

#include <kernel.h>

int sd_init(void);
int sd_writesector(uint32 sector, uint8 *buffer, uint32 sector_count);
int sd_readsector(uint32 sector, uint8 *buffer, uint32 sector_count);

#endif
