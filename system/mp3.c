/**
 * @file mp3.c
 *
 * Decodificador MP3 en streaming (minimp3, dominio publico) con salida fija
 * al formato del cable de audio: estereo de 16 bits a 48 kHz.
 *
 * Decisiones y por que:
 *
 *   - minimp3 vive en el KERNEL, no en la app.  El texto de una app ELF se
 *     ejecuta por la ventana directa (0x88......), sin cache de instrucciones;
 *     ahi un decodificador no llega a tiempo real.  En el kernel va por la
 *     cache de flash y sobra CPU.
 *
 *   - La salida se remuestrea a 48 kHz.  El device y el codec estan fijados a
 *     48 kHz (medido en placa), y cambiar eso obligaria a tocar los divisores
 *     fraccionarios del I2S y la secuencia del ES8311 solo para el 44,1.  Con
 *     interpolacion lineal entera la diferencia es inaudible en este altavoz.
 *
 *   - Todo el estado es de un solo reproductor: no es reentrante.  La placa no
 *     tiene dos salidas de audio y el diseno del reproductor es de un tema a la
 *     vez; documentarlo es mas honesto que fingir una API concurrente.
 *
 * AVISO DE COMA FLOTANTE: minimp3 calcula en float.  Es la unica parte del
 * kernel que lo hace.  El cambio de contexto de Xinu no salva los registros
 * f0-f31, asi que esto solo es seguro mientras UN hilo use el decodificador:
 * el de la app que reproduce.  El resto del kernel (LVGL, red, FAT) es entero
 * y no toca la FPU.  Queda escrito aqui porque es la excepcion a la regla del
 * arbol, y no debe crecer.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <mp3.h>
#include <fslock.h>

#include <fatfs/fat_filelib.h>      /* fl_fopen/fl_fread/fl_fseek            */

/* La implementacion de minimp3, una sola vez en todo el kernel. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

/* Ventana de lectura del fichero.  Cada frame MP3 son como mucho 1441 bytes,
 * asi que 8 KB dan para cinco frames y dejan margen para compactar sin leer
 * demasiado a menudo. */
#define MP3_BUF         (8 * 1024)

/* Frecuencia del cable (device de audio y codec). */
#define MP3_SALIDA_HZ   48000UL

/* Estado del unico reproductor. */
static mp3dec_t *dec;               /* ~24 KB, en PSRAM                     */
static short    *pcm;               /* un frame decodificado, tal cual      */
static uint8_t  *inbuf;             /* ventana de lectura                   */
static void     *fich;              /* FILE* de la FAT                      */
static unsigned  datos_base;        /* offset del primer byte de audio      */
static unsigned  datos_bytes;       /* tamano del flujo (sin ID3)           */
static int       in_len;
static int       in_pos;
static int       eof;
static int       hz;
static int       canales;
static int       kbps;
static unsigned  ms_total;
static unsigned  tramas_out;        /* contadas a la salida (48 kHz)        */
static int       pcm_frames;
static int       pcm_pos;
static unsigned  frac;              /* parte fraccionaria del remuestreo    */
static unsigned  paso;              /* avance por muestra, 16.16            */
static int       ult_l, ult_r;      /* ultima muestra del frame en curso    */
static mp3dec_frame_info_t info;

/** Lee mas bytes del fichero, compactando antes lo que no se ha consumido. */
static int rellenar(void)
{
    int n;

    if (in_pos > 0)
    {
        if (in_len > in_pos)
        {
            memmove(inbuf, inbuf + in_pos, (size_t)(in_len - in_pos));
        }
        in_len -= in_pos;
        in_pos  = 0;
    }
    if (in_len >= MP3_BUF)
    {
        return 1;
    }
    n = fl_fread(inbuf + in_len, 1, MP3_BUF - in_len, fich);
    if (n <= 0)
    {
        eof = 1;
        return 0;
    }
    in_len += n;
    return 1;
}

/** Decodifica el siguiente frame en pcm[].  Devuelve muestras por canal. */
static int decodificar(void)
{
    int samples;

    for (;;)
    {
        if ((in_len - in_pos) < (int)MINIMP3_MAX_SAMPLES_PER_FRAME && !eof)
        {
            rellenar();
        }
        if ((in_len - in_pos) <= 0)
        {
            return 0;
        }

        samples = mp3dec_decode_frame(dec, inbuf + in_pos, in_len - in_pos,
                                      pcm, &info);
        if (samples > 0)
        {
            in_pos += info.frame_bytes;
            if (0 == hz)
            {
                /* Primer frame bueno: de aqui salen formato y duracion. */
                hz      = info.hz;
                canales = info.channels;
                kbps    = info.bitrate_kbps;
                paso    = (unsigned)(((unsigned long long)hz << 16) /
                                     MP3_SALIDA_HZ);
                if (kbps > 0)
                {
                    ms_total = (unsigned)(((unsigned long long)datos_bytes * 8ULL)
                                          / (unsigned long long)kbps);
                }
            }
            ult_l = (1 == info.channels) ? pcm[samples - 1]
                                         : pcm[2 * (samples - 1)];
            ult_r = (1 == info.channels) ? pcm[samples - 1]
                                         : pcm[2 * (samples - 1) + 1];
            return samples;
        }

        /* Sin sincronia en este punto: un byte y volver a probar.  Es lo que
         * hace el propio ejemplo de minimp3 para reengancharse tras basura. */
        in_pos++;
    }
}

/** Muestra (l,r) del frame en curso, con los bordes repetidos. */
static void muestra(int i, int *l, int *r)
{
    if (i < 0)
    {
        *l = ult_l;
        *r = ult_r;
    }
    else if (i >= pcm_frames)
    {
        *l = ult_l;
        *r = ult_r;
    }
    else if (1 == canales)
    {
        *l = pcm[i];
        *r = pcm[i];
    }
    else
    {
        *l = pcm[2 * i];
        *r = pcm[2 * i + 1];
    }
}

static void mp3_cerrar_nolock(void);
static int  mp3_rebobinar_nolock(void);

int mp3_abrir(const char *ruta)
{
    uint8_t cab[10];
    unsigned tam;
    int n;

    fs_lock();
    mp3_cerrar_nolock();

    if (NULL == ruta)
    {
        fs_unlock();
        return SYSERR;
    }
    if (NULL == dec)
    {
        dec   = (mp3dec_t *)psram_malloc(sizeof(mp3dec_t));
        pcm   = (short *)psram_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME *
                                      (int)sizeof(short));
        inbuf = (uint8_t *)psram_malloc(MP3_BUF);
        if (NULL == dec || NULL == pcm || NULL == inbuf)
        {
            mp3_cerrar_nolock();
            fs_unlock();
            return SYSERR;
        }
    }

    fich = fl_fopen(ruta, "r");
    if (NULL == fich)
    {
        fs_unlock();
        return SYSERR;
    }

    fl_fseek(fich, 0, 2);
    tam = (unsigned)fl_ftell(fich);
    fl_fseek(fich, 0, 0);

    datos_base = 0;
    if (tam > 10 && 10 == fl_fread(cab, 1, 10, fich))
    {
        if ('I' == cab[0] && 'D' == cab[1] && '3' == cab[2])
        {
            /* Cabecera ID3v2: tamano sincero (7 bits por byte) + 10. */
            datos_base = 10UL + (((unsigned)(cab[6] & 0x7F) << 21) |
                                 ((unsigned)(cab[7] & 0x7F) << 14) |
                                 ((unsigned)(cab[8] & 0x7F) << 7)  |
                                 ((unsigned)(cab[9] & 0x7F)));
        }
    }
    if (datos_base >= tam)
    {
        datos_base = 0;
    }
    datos_bytes = tam - datos_base;

    fl_fseek(fich, (long)datos_base, 0);
    in_len   = 0;
    in_pos   = 0;
    eof      = 0;
    hz       = 0;
    canales  = 0;
    kbps     = 0;
    ms_total = 0;
    tramas_out = 0;
    pcm_frames = 0;
    pcm_pos    = 0;
    frac       = 0;
    paso       = 65536U;
    ult_l = ult_r = 0;
    mp3dec_init(dec);

    /* Un frame de prueba: si no sale, el fichero no es MP3 (o no se puede
     * decodificar) y es mejor decirlo al abrir que a mitad de la lista. */
    n = decodificar();
    if (n <= 0)
    {
        mp3_cerrar_nolock();
        fs_unlock();
        return SYSERR;
    }
    n = mp3_rebobinar_nolock();
    fs_unlock();
    return n;
}

static int mp3_rebobinar_nolock(void)
{
    if (NULL == fich)
    {
        return SYSERR;
    }
    fl_fseek(fich, (long)datos_base, 0);
    in_len = 0;
    in_pos = 0;
    eof    = 0;
    tramas_out = 0;
    pcm_frames = 0;
    pcm_pos    = 0;
    frac       = 0;
    ult_l = ult_r = 0;
    mp3dec_init(dec);
    return OK;
}

static void mp3_cerrar_nolock(void)
{
    if (NULL != fich)
    {
        fl_fclose(fich);
        fich = NULL;
    }
    if (NULL != dec)
    {
        psram_free(dec);
        dec = NULL;
    }
    if (NULL != pcm)
    {
        psram_free(pcm);
        pcm = NULL;
    }
    if (NULL != inbuf)
    {
        psram_free(inbuf);
        inbuf = NULL;
    }
    hz = canales = kbps = 0;
    ms_total = 0;
    tramas_out = 0;
    pcm_frames = pcm_pos = 0;
    frac = 0;
}

int mp3_rebobinar(void)
{
    int r;
    fs_lock();
    r = mp3_rebobinar_nolock();
    fs_unlock();
    return r;
}

void mp3_cerrar(void)
{
    fs_lock();
    mp3_cerrar_nolock();
    fs_unlock();
}

int mp3_hz(void)      { return hz; }
int mp3_canales(void) { return canales; }
int mp3_kbps(void)    { return kbps; }
unsigned mp3_ms_total(void)  { return ms_total; }
unsigned mp3_ms_actual(void)
{
    return (unsigned)(((unsigned long long)tramas_out * 1000ULL) / MP3_SALIDA_HZ);
}

int mp3_leer(short *dst, int frames)
{
    int salida = 0;

    if (NULL == fich || NULL == dst || frames <= 0)
    {
        return SYSERR;
    }

    while (salida < frames)
    {
        if (pcm_pos >= pcm_frames)
        {
            int n;
            /* Solo se bloquea el FS durante la lectura/decodificacion del
             * fichero; el remuestreo de salida no toca el FS (evita entrecortes
             * cuando otra hebra, p. ej. elf_run, lee la SD). */
            fs_lock();
            n = decodificar();
            fs_unlock();

            if (n <= 0)
            {
                break;
            }
            pcm_frames = n;
            pcm_pos    = 0;
        }
        while (salida < frames && pcm_pos < pcm_frames)
        {
            int  al, ar, bl, br;
            long w = (long)frac;

            muestra(pcm_pos, &al, &ar);
            muestra(pcm_pos + 1, &bl, &br);

            /* Interpolacion lineal 16.16.  A 48 kHz el paso es 65536 y esto
             * es una copia exacta (w siempre 0). */
            dst[2 * salida + 0] = (short)(((long)al * (65536L - w) +
                                           (long)bl * w) >> 16);
            dst[2 * salida + 1] = (short)(((long)ar * (65536L - w) +
                                           (long)br * w) >> 16);
            salida++;
            frac     += paso;
            pcm_pos  += (int)(frac >> 16);
            frac     &= 0xFFFFU;
        }
    }

    tramas_out += (unsigned)salida;
    return salida;
}
