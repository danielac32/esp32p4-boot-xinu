/**
 * @file stdbool.h
 *
 * Minimal C99 <stdbool.h> for the kernel.  Xinu's <stddef.h> already
 * defines @c bool (as @c char) plus TRUE/FALSE; here we only provide the
 * lowercase spellings used by code coming from the ESP32 SDK.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _STDBOOL_H_
#define _STDBOOL_H_

#include <stddef.h>             /* provides bool, TRUE, FALSE */

#ifndef true
#define true    TRUE
#endif

#ifndef false
#define false   FALSE
#endif

#define __bool_true_false_are_defined 1

#endif                          /* _STDBOOL_H_ */
