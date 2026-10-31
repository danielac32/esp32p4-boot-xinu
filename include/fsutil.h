/**
 * @file fsutil.h
 *
 * Small helpers shared by the filesystem shell commands (xsh_ls, xsh_cat,
 * xsh_cd, ...).  They sit directly on the FAT library (fatfs/), whose
 * reentrant lock is a kernel monitor installed by the `fat` device at boot.
 *
 * There is no VFS and no file descriptors here: commands open the FAT
 * library directly.  The Xinu shell's '>' / '<' redirection only resolves
 * device names (getdev), not paths, so it does not apply to these commands.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _FSUTIL_H_
#define _FSUTIL_H_

#include <stddef.h>
#include <kernel.h>
#include <fat_filelib.h>

/** Maximum length of a path handled by the shell tools, incl. NUL. */
#define FS_PATH_MAX 128

/** Current working directory of the shell (always absolute, starts '/'). */
extern char fs_cwd[FS_PATH_MAX];

/** TRUE if the FAT device mounted a volume (safe to call fl_*). */
bool fs_mounted(void);

/** TRUE if @p path exists (file or directory). */
bool fs_exists(const char *path);

/**
 * Resolve a user-supplied path against fs_cwd into an absolute, normalized
 * path in @p out (which must hold at least @p outsz bytes).
 * @return TRUE on success, FALSE if it does not fit.
 */
bool fs_resolve(const char *arg, char *out, size_t outsz);

/**
 * Copy the contents of file @p src to file @p dst (dst is created/truncated).
 * @return OK, or SYSERR.
 */
int fs_copy(const char *src, const char *dst);

/** Return the final component of @p path (after the last '/'). */
const char *fs_basename(const char *path);

/**
 * Recursively copy @p src (file or directory) to @p dst.
 * @return OK, or SYSERR.
 */
int fs_copy_tree(const char *src, const char *dst);

/**
 * Recursively remove @p path (file or directory).
 * @return OK, or SYSERR.
 */
int fs_remove_tree(const char *path);

#endif                          /* _FSUTIL_H_ */
