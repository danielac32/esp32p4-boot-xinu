/**
 * @file sdCard.h
 *
 * Internals of the SD0 card device shared by its source files.  The public
 * interface (control functions, card state, protocol) is in <sdblock.h>.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SDCARD_H_
#define _SDCARD_H_

#include <sdblock.h>

extern uint32_t sd_lba;             /* sector cursor of the SD0 device */
extern const char *sd_fail;         /* stage a failed probe stopped at */

/**
 * Card lock.  Created by sdblkInit; before that (boot, single task) the
 * lock/unlock calls are no-ops, like the bus lock.  Always take the card
 * lock before the bus lock.
 */
void sdCardMonInit(void);
void sdCardLock(void);
void sdCardUnlock(void);

/**
 * Identify the card again, as one operation on the shared controller.  Used
 * by init and by SD_CTRL_RESET.
 */
bool sdIdentify(void);

#endif                          /* _SDCARD_H_ */
