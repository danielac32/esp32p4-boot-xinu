/**
 * @file fat.h
 *
 * FAT32 device: volume mounted on the microSD (SDMMC slot 0).  The device
 * owns the monitor that serializes the shared SDMMC controller (microSD +
 * ESP32-C6) and mounts the FAT library (fatfs/).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _FAT_H_
#define _FAT_H_

#include <device.h>
#include <fatfs.h>

/* control(FAT0, CMD, arg1, arg2) */
#define FAT_CTRL_MOUNTED  0x01 /**< arg1 ignored; returns 1 if mounted     */
#define FAT_CTRL_BLOCKS   0x02 /**< arg1 ignored; returns capacity in 512B */

/* Device functions */
devcall fatInit(device *);
devcall fatControl(device *, int, long, long);

#endif                          /* _FAT_H_ */
