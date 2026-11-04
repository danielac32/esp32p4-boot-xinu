/**
 * @file xsh_telnetd.c
 *
 * Alias for the telnetserver command.
 */
/* Embedded Xinu, ported to ESP32-S3. */

#include <shell.h>
#include <string.h>
#include <stdio.h>

shellcmd xsh_telnetd(int nargs, char *args[])
{
    return xsh_telnetserver(nargs, args);
}
