/**
 * @file psramcopy.h
 *
 * Mover bulto por el acceso directo de PSRAM (0x88......) de palabra en
 * palabra, no byte a byte.
 *
 * Las dos vias por las que sale y entra PCM pasan por esa ventana porque la
 * memoria no puede ir por la via con cache (es la trampa que ya esta escrita en
 * ld.script y en la cabecera de audioIn.c: una barrida grande por 0x48......
 * deja lineas de L2MEM leidas a cero).  El precio del acceso directo es que no
 * hay burst: cada acceso es una transaccion completa por el AHB, y la memcpy y
 * la memset del arbol van byte a byte.  Medido en placa con esto puesto, esa
 * forma de copiar deja el consumidor de captura en unos 152.000 bytes por
 * segundo frente a los 192.000 que produce el I2S, y el anillo se desborda a
 * mitad de la toma: se pierden dos de cada diez trozos.
 *
 * I2S_FRAME_BITS es 32, asi que todo lo que se mueve por aqui son tramas de 4
 * bytes y las dos puntas caen alineadas casi siempre; los bordes siguen a byte
 * por si acaso, porque un acceso desalineado no lo resuelve el core.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef XINU_AUDIO_PSRAMCOPY_H
#define XINU_AUDIO_PSRAMCOPY_H

#include <stdint.h>
#include <stddef.h>

/** Copiar @p n bytes de @p src a @p dst yendo de 4 en 4 en cuanto las dos
 *  puntas lo permiten. */
static inline void psram_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* Mientras alguna de las dos no este en borde de palabra no se puede leer
     * de 4 en 4: se copian esos bytes sueltos primero.  Si nunca se alinean
     * --puntos con distinto resto-- esto acaba copiando todo a byte, que es lo
     * que se merece quien no respeta el formato del cable. */
    while (0U != n && 0U != ((((uintptr_t)d | (uintptr_t)s) & 3U)))
    {
        *d++ = *s++;
        n--;
    }

    for (; n >= 4U; n -= 4U, d += 4U, s += 4U)
    {
        *(uint32_t *)d = *(const uint32_t *)s;
    }

    while (0U != n)
    {
        *d++ = *s++;
        n--;
    }
}

/** Poner a cero @p n bytes de @p dst, de palabra en palabra igual que
 *  @ref psram_copy. */
static inline void psram_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (0U != n && 0U != (((uintptr_t)d & 3U)))
    {
        *d++ = 0U;
        n--;
    }

    for (; n >= 4U; n -= 4U, d += 4U)
    {
        *(uint32_t *)d = 0U;
    }

    while (0U != n)
    {
        *d++ = 0U;
        n--;
    }
}

#endif /* XINU_AUDIO_PSRAMCOPY_H */
