/**
 * @file     xsh_format.c
 *
 * Shell command (format): rebuild the FAT32 volume on the microSD card,
 * destroying every file on it.  The work itself is the `fat` device's; this
 * command only insists on the confirmation and reports the result.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <fat.h>
#include <fsutil.h>

/** The boot sector holds an 11-character volume name; no longer labels fit. */
#define FORMAT_LABEL_MAX 11

/**
 * @ingroup shell
 *
 * Shell command (format yes [LABEL]).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_format(int nargs, char *args[])
{
    const char *label = FAT_DEFAULT_LABEL;

    if (nargs >= 2 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: %s yes [LABEL]\n\n", args[0]);
        printf("Description:\n");
        printf("\tErase the microSD card and build an empty FAT32 volume on\n");
        printf("\tit.  Every file is lost, including the ones the volume\n");
        printf("\tcannot look up any more (orphaned directory entries).  The\n");
        printf("\trewrite of both FAT copies takes close to a minute.\n");
        printf("Options:\n");
        printf("\tyes\trequired confirmation that the card may be erased\n");
        printf("\tLABEL\tvolume name, up to %d characters (default %s)\n",
               FORMAT_LABEL_MAX, FAT_DEFAULT_LABEL);
        printf("\t--help\tdisplay this help and exit\n");
        return OK;
    }

    if (nargs < 2 || strcmp(args[1], "yes") != 0)
    {
        fprintf(stderr, "%s: this erases every file on the microSD card\n",
                args[0]);
        fprintf(stderr, "%s: run '%s yes [LABEL]' to go ahead\n",
                args[0], args[0]);
        return SYSERR;
    }

    if (nargs > 3)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        return SYSERR;
    }

    if (nargs == 3)
    {
        label = args[2];
        if (strlen(label) > FORMAT_LABEL_MAX)
        {
            fprintf(stderr, "%s: label '%s' is over %d characters\n",
                    args[0], label, FORMAT_LABEL_MAX);
            return SYSERR;
        }
    }

    printf("Formatting the card as FAT32 (label \"%s\") ...\n", label);
    printf("This rewrites both FAT copies, sector by sector: about a minute.\n");

    if (OK != control(FAT0, FAT_CTRL_FORMAT, (long)label, 0))
    {
        fprintf(stderr, "%s: format failed\n", args[0]);
        return SYSERR;
    }

    /* Nothing the shell could be pointing at survives the erase. */
    strcpy(fs_cwd, "/");

    printf("Format complete; the volume is empty and the working "
           "directory is /\n");
    return OK;
}
