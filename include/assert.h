/**
 * @file assert.h
 *
 * Minimal assert.h for the freestanding Xinu build (the kernel compiles with
 * -nostdinc, so the toolchain's assert.h is not available).  The vendored FAT
 * library includes it; its only uses are inside a disabled test bench.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _ASSERT_H_
#define _ASSERT_H_

#ifdef NDEBUG
#  define assert(expr)  ((void)0)
#else
#  define assert(expr)  ((void)(expr))
#endif

#endif                          /* _ASSERT_H_ */
