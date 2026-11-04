/**
 * @file     shell.c
 *
 */
/* Embedded Xinu, Copyright (C) 2009.  All rights reserved. */

#include <stddef.h>
#include <ctype.h>
#include <interrupt.h>
#include <shell.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <tty.h>
#include <thread.h>
#include <conf.h>
#include <uart.h>
#include <fat.h>

const struct centry commandtab[] = {
    {"clear", TRUE, xsh_clear},
    {"echo", FALSE, xsh_echo},
    {"help", FALSE, xsh_help},
    {"?", FALSE, xsh_help},
    {"exit", TRUE, xsh_exit},
    {"sleep", TRUE, xsh_sleep},
    {"reset", FALSE, xsh_reset},
    {"date", FALSE, xsh_date},
    {"uptime", FALSE, xsh_uptime},
    {"kill", TRUE, xsh_kill},
    {"ps", FALSE, xsh_ps},
    {"stack", FALSE, xsh_stack},
    {"ktest", FALSE, xsh_ktest},
    {"sem", FALSE, xsh_sem},
    {"mon", FALSE, xsh_mon},
    {"mem", FALSE, xsh_mem},
    {"memdump", FALSE, xsh_memdump},
    {"uartstat", FALSE, xsh_uartstat},
    {"lcd", FALSE, xsh_lcd},
    {"i2c", FALSE, xsh_i2c},
    {"gpio", FALSE, xsh_gpio},
    {"touchpanel", FALSE, xsh_touchpanel},
    {"power", FALSE, xsh_power},
    {"audio", FALSE, xsh_audio},
    {"wifi", FALSE, xsh_wifi},
    {"netup", FALSE, xsh_netup},
    {"netdown", FALSE, xsh_netdown},
    {"ping", FALSE, xsh_ping},
    {"ifconfig", FALSE, xsh_ifconfig},
    {"netstat", FALSE, xsh_netstat},
    {"arp", FALSE, xsh_arp},
    {"route", FALSE, xsh_route},
    {"tcpstat", FALSE, xsh_tcpstat},
    {"udpstat", FALSE, xsh_udpstat},
    {"dns", FALSE, xsh_dns},
    {"telnet", FALSE, xsh_telnet},
    {"telnetserver", FALSE, xsh_telnetserver},
    {"telnetd", FALSE, xsh_telnetd},
    {"httpd", FALSE, xsh_httpd},
    {"netboot", FALSE, xsh_netboot},
    {"sd", FALSE, xsh_sd},
    {"pwd", FALSE, xsh_pwd},
    {"cd", FALSE, xsh_cd},
    {"ls", FALSE, xsh_ls},
    {"cat", FALSE, xsh_cat},
    {"head", FALSE, xsh_head},
    {"tail", FALSE, xsh_tail},
    {"cp", FALSE, xsh_cp},
    {"mv", FALSE, xsh_mv},
    {"rm", FALSE, xsh_rm},
    {"mkdir", FALSE, xsh_mkdir},
    {"rmdir", FALSE, xsh_rmdir},
    {"stat", FALSE, xsh_stat},
    {"df", FALSE, xsh_df},
    {"format", FALSE, xsh_format},
    {"hexdump", FALSE, xsh_hexdump},
    {"touch", FALSE, xsh_touch},
    {"test", FALSE, xsh_test},
    {"elfapp", FALSE, xsh_elfapp},
    {"syscheck", FALSE, xsh_syscheck},
};
ulong ncommand = sizeof(commandtab) / sizeof(struct centry);


extern ulong foreground;

/**
 * @ingroup shell
 *
 * The Xinu shell.  Provides an interface to execute commands.
 * @param descrp descriptor of device on which the shell is open
 * @return OK for successful exit, SYSERR for unrecoverable error
 */
thread shell(int indescrp, int outdescrp, int errdescrp)
{
    char buf[SHELL_BUFLEN];     /* line input buffer        */
    short buflen;               /* length of line input     */
    char tokbuf[SHELL_BUFLEN + SHELL_MAXTOK];   /* token value buffer       */
    short ntok;                 /* number of tokens         */
    char *tok[SHELL_MAXTOK];    /* pointers to token values */
    char *outname;              /* name of output file      */
    char *inname;               /* name of input file       */
    bool background;            /* is background proccess?  */
    syscall child;              /* pid of child thread      */
    ushort i, j;                /* temp variables           */
    irqmask im;                 /* interrupt mask state     */
 
    /* Set command devices for input, output, and error */
    stdin = indescrp;
    stdout = outdescrp;
    stderr = errdescrp;
 
    printf(SHELL_BANNER);
    printf(SHELL_START);
 
    /* Continually receive and handle commands */
    while (TRUE)
    {
        /* Display prompt */
        printf(SHELL_PROMPT);
        printf("$ ");

        /* Setup proper tty modes for input and output */
        control(stdin, TTY_CTRL_CLR_IFLAG, TTY_IRAW, NULL);
        control(stdin, TTY_CTRL_SET_IFLAG, TTY_ECHO, NULL);

        /* Read command */
        buflen = read(stdin, buf, SHELL_BUFLEN - 1);

        /* Check for EOF and exit gracefully if seen */
        if (EOF == buflen)
        {
            break;
        }

        /* Parse line input into tokens */
        if (SYSERR == (ntok = lexan(buf, buflen, &tokbuf[0], &tok[0])))
        {
            fprintf(stderr, SHELL_SYNTAXERR);
            continue;
        }

        /* Ensure parse generated tokens */
        if (0 == ntok)
        {
            continue;
        }

        /* Initialize command options */
        inname = NULL;
        outname = NULL;
        background = FALSE;

        /* Mark as background thread, if last token is '&' */
        if ('&' == *tok[ntok - 1])
        {
            ntok--;
            background = TRUE;
        }

        /* Check each token and perform special handling of '>' and '<' */
        for (i = 0; i < ntok; i++)
        {
            /* Background '&' should have already been handled; Syntax error */
            if ('&' == *tok[i])
            {
                ntok = -1;
                break;
            }

            /* Setup for output redirection if token is '>'  */
            if ('>' == *tok[i])
            {
                /* Syntax error */
                if (outname != NULL || i >= ntok - 1)
                {
                    ntok = -1;
                    break;
                }

                outname = tok[i + 1];
                ntok -= 2;

                /* shift tokens (not to be passed to command */
                for (j = i; j < ntok; j++)
                {
                    tok[j] = tok[j + 2];
                }
                continue;
            }

            /* Setup for input redirection if token is '<' */
            if ('<' == *tok[i])
            {
                /* Syntax error */
                if (inname != NULL || i >= ntok - 1)
                {
                    ntok = -1;
                    break;
                }
                inname = tok[i + 1];
                ntok -= 2;

                /* shift tokens (not to be passed to command */
                for (j = i; j < ntok; j++)
                {
                    tok[j] = tok[j + 2];
                }

                continue;
            }
        }

        /* Handle syntax error */
        if (ntok <= 0)
        {
            fprintf(stderr, SHELL_SYNTAXERR);
            continue;
        }

        /* Lookup first token in the command table */
        for (i = 0; i < ncommand; i++)
        {
            if (0 == strcmp(commandtab[i].name, tok[0]))
            {
                break;
            }
        }

        /* Handle command not found */
        if (i >= ncommand)
        {
            fprintf(stderr, "%s: command not found\n", tok[0]);
            continue;
        }

        /* Handle command if it is built-in */
        if (commandtab[i].builtin)
        {
            if (inname != NULL || outname != NULL || background)
            {
                fprintf(stderr, SHELL_SYNTAXERR);
            }
            else
            {
                (*commandtab[i].procedure) (ntok, tok);
            }
            continue;
        }

        /* Spawn child thread for non-built-in commands */
        child =
            create(commandtab[i].procedure,
                   SHELL_CMDSTK, SHELL_CMDPRIO,
                   commandtab[i].name, 2, ntok, tok);

        /* Ensure child command thread was created successfully */
        if (SYSERR == child)
        {
            fprintf(stderr, SHELL_CHILDERR);
            continue;
        }

        /* Set file descriptors for newly created thread */
        if (NULL == inname)
        {
            thrtab[child].fdesc[0] = stdin;
        }
        else
        {
            thrtab[child].fdesc[0] = getdev(inname);
        }
        if (NULL == outname)
        {
            thrtab[child].fdesc[1] = stdout;
        }
        else
        {
            thrtab[child].fdesc[1] = getdev(outname);
        }
        thrtab[child].fdesc[2] = stderr;

        if (background)
        {
            /* Make background thread ready, but don't reschedule */
            im = disable();
            ready(child, RESCHED_NO);
            restore(im);
        }
        else
        {
            /* Clear waiting message; Reschedule; */
            while (recvclr() != NOMSG);
            im = disable();
            ready(child, RESCHED_YES);
            restore(im);

            /* Wait for command thread to finish */
            while (receive() != child);
            sleep(10);
        }
    }

    /* Close shell */
    fprintf(stdout, SHELL_EXIT);
    sleep(10);
    return OK;
}
