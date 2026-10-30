/**
 * @file stdint.h
 *
 * Signed and unsigned integer types of exact width.
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#ifndef _STDINT_H_
#define _STDINT_H_

typedef signed char          int8_t;
typedef short                int16_t;
typedef int                  int32_t;
typedef long long            int64_t;

typedef unsigned char        uint8_t;
typedef unsigned short       uint16_t;
typedef unsigned int         uint32_t;
typedef unsigned long long   uint64_t;

#if __LP64__
typedef long int             intptr_t;
typedef unsigned long int    uintptr_t;
#else
typedef int                  intptr_t;
typedef unsigned int         uintptr_t;
#endif

/* Least- and fast-width types (RV32: all map to the exact-width types). */
typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

typedef int8_t   int_fast8_t;
typedef int16_t  int_fast16_t;
typedef int32_t  int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint8_t  uint_fast8_t;
typedef uint16_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;

typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

/* Limits of exact-width integer types. */
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255U
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535U
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL

/* Limits of pointer / size / ptrdiff types (ILP32). */
#define INTPTR_MIN   INT32_MIN
#define INTPTR_MAX   INT32_MAX
#define UINTPTR_MAX  UINT32_MAX
#define SIZE_MAX     UINT32_MAX
#define PTRDIFF_MIN  INT32_MIN
#define PTRDIFF_MAX  INT32_MAX

#endif /* _STDINT_H_ */
