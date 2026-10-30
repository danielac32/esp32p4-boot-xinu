/**
 * @file stddef.h
 *
 * Includes standard type definitions and primary constants used across
 * all aspects of Xinu.
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#ifndef _STDDEF_H_
#define _STDDEF_H_

/* macro to get offset to struct members */
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

/* Base type definitions */
typedef unsigned char uchar;    /**< unsigned char type                 */
typedef unsigned short ushort;  /**< unsigned short type                */
typedef unsigned int uint;      /**< unsigned int type                  */
typedef unsigned long ulong;    /**< unsigned long type                 */
typedef char bool;              /**< boolean type                       */
typedef unsigned long size_t;   /**< size type                          */
typedef int ptrdiff_t;          /**< pointer difference type            */
typedef int wchar_t;            /**< wide character type                */

/* Fixed-width integer types (upstream Xinu names, in addition to the
 * stdint.h *_t names).  The vendored FAT library uses these. */
typedef signed char int8;       /**< signed 8-bit type                  */
typedef unsigned char uint8;    /**< unsigned 8-bit type                */
typedef signed short int16;     /**< signed 16-bit type                 */
typedef unsigned short uint16;  /**< unsigned 16-bit type               */
typedef signed int int32;       /**< signed 32-bit type                 */
typedef unsigned int uint32;    /**< unsigned 32-bit type               */
typedef signed long long int64;    /**< signed 64-bit type              */
typedef unsigned long long uint64; /**< unsigned 64-bit type            */

/* Function declaration return types */
typedef int syscall;            /**< system call declaration            */
typedef int devcall;            /**< device call declaration            */
typedef int shellcmd;           /**< shell command declaration          */
typedef int thread;             /**< thread declaration                 */
typedef void interrupt;         /**< interrupt procedure                */
typedef void exchandler;        /**< exception procedure                */
typedef int message;            /**< message passing content            */

typedef int tid_typ;            /**< thread ID type                     */

/* Boolean type and constants */
#define FALSE        0          /**< boolean false                      */
#define TRUE         1          /**< boolean true                       */

/* Universal return constants */
#define OK        1             /**< system call ok                     */
#define NULL      0             /**< null pointer for linked lists      */
#define SYSERR   (-1)           /**< system call failed                 */
#define EOF      (-2)           /**< End-of-file (usually from read)    */
#define TIMEOUT  (-3)           /**< system call timed out              */
#define NOMSG    (-4)           /**< no message to receive              */

#endif                          /* _STDDEF_H_ */
