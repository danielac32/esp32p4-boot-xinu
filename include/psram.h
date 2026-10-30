/**
 * @file psram.h
 *
 * PSRAM device interface.  The PSRAM controller is brought up and mapped
 * by the SBL; the kernel-side @c psramInit device builds the free list
 * managed by system/psrammem.c (psram_malloc/psram_free).
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#ifndef _PSRAM_H_
#define _PSRAM_H_

#include <device.h>
 

/* Driver functions */
devcall psramInit(device *);

#endif                          /* _PSRAM_H_ */
