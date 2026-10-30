/*
 * @file xsh_audio.c
 *
 * Shell command: exercise the audio path (ES8311 codec + I2S0).  Codec
 * operations go through the AUDIO device (volume/mute/powerdown); PCM
 * playback and capture go through the I2S device (beep / tone / record).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <kernel.h>
#include <memory.h>
#include <device.h>
#include <audio.h>
#include <i2s.h>

#define AUDIO_RATE 16000U

static int usage(char *name)
{
    printf("Usage: %s <command> [args]\n\n", name);
    printf("Commands:\n");
    printf("\tvol <0-100>        codec volume\n");
    printf("\tmute <on|off>      codec mute\n");
    printf("\tbeep [hz] [ms]     tone via I2S beep (default 440 500)\n");
    printf("\ttone [hz] [ms]     square tone via I2S playback\n");
    printf("\trec [ms]           record from the mic and report peak\n");
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (audio ...).
 */
shellcmd xsh_audio(int nargs, char *args[])
{
    int hz, ms;

    if (nargs < 2 || strcmp(args[1], "--help") == 0)
    {
        return usage(args[0]);
    }

    if (strcmp(args[1], "vol") == 0 && nargs == 3)
    {
        int v = atoi(args[2]);
        printf("%s: volumen %d -> %s\n", args[0], v,
               control(AUDIO0, AUDIO_CTRL_VOLUME, v, 0) == OK ? "OK" : "ERR");
        return OK;
    }

    if (strcmp(args[1], "mute") == 0 && nargs == 3)
    {
        int on = (strcmp(args[2], "on") == 0);
        printf("%s: mute %s -> %s\n", args[0], args[2],
               control(AUDIO0, AUDIO_CTRL_MUTE, on, 0) == OK ? "OK" : "ERR");
        return OK;
    }

    if (strcmp(args[1], "beep") == 0 && nargs <= 4)
    {
        hz = (nargs >= 3) ? atoi(args[2]) : 440;
        ms = (nargs >= 4) ? atoi(args[3]) : 500;
        printf("%s: beep %d Hz %d ms -> %s\n", args[0], hz, ms,
               control(I2S0, I2S_CTRL_BEEP, hz, ms) == OK ? "OK" : "ERR");
        return OK;
    }

    if (strcmp(args[1], "tone") == 0 && nargs <= 4)
    {
        size_t frames;
        int16_t *buf;
        uint32_t step;
        uint32_t phase = 0;
        size_t i;

        hz = (nargs >= 3) ? atoi(args[2]) : 440;
        ms = (nargs >= 4) ? atoi(args[3]) : 1000;
        frames = (size_t)((long) AUDIO_RATE * ms / 1000);
        buf = (int16_t *) psram_malloc(frames * 2 * sizeof(int16_t));
        if (buf == NULL)
        {
            fprintf(stderr, "%s: sin memoria (PSRAM)\n", args[0]);
            return SYSERR;
        }
        /* Square wave: flip the sample every half period. */
        step = (uint32_t)(((uint64_t) hz << 32) / AUDIO_RATE);
        for (i = 0; i < frames; i++)
        {
            int16_t s = (phase < 0x80000000UL) ? 6000 : -6000;
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
            phase += step;
        }
        printf("%s: tono %d Hz %d ms (%u frames)...\n", args[0], hz, ms,
               (unsigned) frames);
        printf("%s: play -> %s\n", args[0],
               control(I2S0, I2S_CTRL_PLAY, (long) buf, (long) frames) == OK
                   ? "OK" : "ERR");
        psram_free(buf);
        return OK;
    }

    if (strcmp(args[1], "rec") == 0 && nargs <= 3)
    {
        size_t samples;
        int16_t *buf;
        int peak = 0;
        size_t i;

        ms = (nargs >= 3) ? atoi(args[2]) : 1000;
        samples = (size_t)((long) AUDIO_RATE * ms / 1000);
        buf = (int16_t *) psram_malloc(samples * 2 * sizeof(int16_t));
        if (buf == NULL)
        {
            fprintf(stderr, "%s: sin memoria (PSRAM)\n", args[0]);
            return SYSERR;
        }
        printf("%s: grabando %d ms del micro...\n", args[0], ms);
        if (control(I2S0, I2S_CTRL_RECORD, (long) buf, (long) samples) != OK)
        {
            fprintf(stderr, "%s: record falló\n", args[0]);
            psram_free(buf);
            return SYSERR;
        }
        for (i = 0; i < samples * 2; i++)
        {
            int v = buf[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        printf("%s: pico = %d (0..32767) en %u muestras\n", args[0], peak,
               (unsigned) samples);
        psram_free(buf);
        return OK;
    }

    return usage(args[0]);
}
