/**
 * @file fat.h
 *
 * FAT volume on the microSD card (SDMMC slot 0).  The device mounts the FAT
 * library (fatfs/) at boot and owns the library's reentrant monitor; the
 * sectors themselves travel through the SD0 card device, so this layer never
 * touches SDMMC registers.  Users move files with the library (fl_*) and ask
 * this device for state and for the destructive volume operations (format),
 * which need the library monitor held across more than one library call.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _FAT_H_
#define _FAT_H_

#include <device.h>
#include <fatfs.h>

/** Volume name written into the boot sector when the caller gives none. */
#define FAT_DEFAULT_LABEL   "XINUSD"

/* control(FAT0, CMD, arg1, arg2) */
#define FAT_CTRL_MOUNTED  0x01 /**< arg1 ignored; returns 1 if mounted     */
#define FAT_CTRL_BLOCKS   0x02 /**< arg1 ignored; returns capacity in 512B */
#define FAT_CTRL_FORMAT   0x03 /**< arg1 is a const char * label; erases
                                * the volume and mounts the fresh one      */

/* Device functions */
devcall fatInit(device *);
devcall fatControl(device *, int, long, long);

/** TRUE once fatInit() mounted a volume (safe to call fl_*). */
bool fatMounted(void);

/** Build a FAT32 volume over the whole card, destroying every file on it,
 *  and mount the result.  @p label is the volume name, up to 11 characters;
 *  NULL or "" uses ::FAT_DEFAULT_LABEL.  Returns OK or SYSERR. */
devcall fatFormat(const char *label);

/** Hold the library monitor across a sequence of fl_* calls.  The library
 *  itself only locks around a single call, so a multi-step operation - a
 *  format followed by its remount, for example - needs these. */
void fatLibLock(void);
void fatLibUnlock(void);

#endif                          /* _FAT_H_ */
