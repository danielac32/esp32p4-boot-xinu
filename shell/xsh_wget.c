/**
 * @file     xsh_wget.c
 *
 * Shell command (wget): download an http:// URL to a file on the FAT volume.
 * Uses the minimal raw-API HTTP client (drivers/net/httpclient.c) that runs in
 * the NET task.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <thread.h>
#include <drivers/net/wifi.h>
#include <fsutil.h>

/* Best-effort basename of the URL path, for the default output file. */
static const char *url_basename(const char *url, char *buf, size_t bufsz)
{
    const char *p = strstr(url, "://");
    const char *slash;
    const char *name;
    const char *q;
    size_t n;

    p = (p != NULL) ? p + 3 : url;
    slash = strrchr(p, '/');
    if (slash == NULL)
    {
        strlcpy(buf, "index.html", bufsz);
        return buf;
    }

    name = slash + 1;
    q = strchr(name, '?');
    n = (q != NULL) ? (size_t)(q - name) : strlen(name);

    if (n == 0 || n >= bufsz)
        strlcpy(buf, "index.html", bufsz);
    else
    {
        memcpy(buf, name, n);
        buf[n] = '\0';
    }
    return buf;
}

static void wget_usage(const char *cmd)
{
    printf("Usage: %s [-q] [-O FILE] URL\n\n", cmd);
    printf("Description:\n");
    printf("\tDownloads the http:// URL.\n");
    printf("Options:\n");
    printf("\t-O FILE\twrite to FILE (use '-' for standard output)\n");
    printf("\t-q\tquiet\n");
    printf("\t--help\tdisplay this help and exit\n");
}

/**
 * @ingroup shell
 *
 * Shell command (wget [-q] [-O FILE] URL).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_wget(int nargs, char *args[])
{
    const char *url = NULL;
    const char *ofile = NULL;
    bool quiet = FALSE;
    bool to_stdout = FALSE;
    char outpath[FS_PATH_MAX];
    char base[FS_PATH_MAX];
    int i;
    int status;
    long got;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        wget_usage(args[0]);
        return OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (strcmp(args[i], "-O") == 0)
        {
            if (i + 1 >= nargs)
            {
                fprintf(stderr, "%s: -O needs a file\n", args[0]);
                return SYSERR;
            }
            ofile = args[++i];
        }
        else if (strcmp(args[i], "-q") == 0)
        {
            quiet = TRUE;
        }
        else if (args[i][0] == '-' && args[i][1] != '\0')
        {
            fprintf(stderr, "%s: unknown option '%s'\n", args[0], args[i]);
            wget_usage(args[0]);
            return SYSERR;
        }
        else if (url == NULL)
        {
            url = args[i];
        }
        else
        {
            fprintf(stderr, "%s: only one URL allowed\n", args[0]);
            return SYSERR;
        }
    }

    if (url == NULL)
    {
        fprintf(stderr, "%s: missing URL\n", args[0]);
        wget_usage(args[0]);
        return SYSERR;
    }

    if (!wifi_is_link_up())
    {
        fprintf(stderr, "%s: network is down (no WiFi link yet)\n", args[0]);
        return SYSERR;
    }

    if (ofile != NULL && strcmp(ofile, "-") == 0)
    {
        to_stdout = TRUE;
    }
    else if (ofile != NULL)
    {
        if (!fs_mounted())
        {
            fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
            return SYSERR;
        }
        if (!fs_resolve(ofile, outpath, sizeof(outpath)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            return SYSERR;
        }
    }
    else
    {
        if (!fs_mounted())
        {
            fprintf(stderr, "%s: no FAT volume mounted\n", args[0]);
            return SYSERR;
        }
        url_basename(url, base, sizeof(base));
        if (!fs_resolve(base, outpath, sizeof(outpath)))
        {
            fprintf(stderr, "%s: path too long\n", args[0]);
            return SYSERR;
        }
    }

    if (http_get_start(url, to_stdout ? NULL : outpath) != OK)
    {
        fprintf(stderr, "%s: another transfer is already running\n", args[0]);
        return SYSERR;
    }

    if (!quiet)
    {
        printf("--%s--  %s\n", "get", url);
        printf("Connecting...\n");
    }

    while (!http_get_done())
        sleep(100);          /* cede la CPU: el NET task hace el trabajo */

    status = http_get_status();
    got = http_get_received();

    if (http_get_error()[0] != '\0')
    {
        fprintf(stderr, "%s: %s\n", args[0], http_get_error());
        http_get_release();
        return SYSERR;
    }

    if (to_stdout)
    {
        const char *body = http_get_body(NULL);
        long len;
        long k;

        (void)http_get_body(&len);
        for (k = 0; k < len; k++)
            putchar(body[k]);
    }
    else if (!quiet)
    {
        printf("HTTP %d  %ld bytes -> %s\n", status, got, outpath);
    }

    http_get_release();
    return (status >= 200 && status < 300) ? OK : SYSERR;
}
