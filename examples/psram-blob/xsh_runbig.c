/**
 * @file xsh_runbig.c
 *
 * CARGADOR DE REFERENCIA, NO COMPILADO: vease README.md de este directorio
 * para como engancharlo en la shell.
 *
 * Segundo paso del ejercicio de PSRAM: un blob QUE NO ES UNA HOJA.
 *
 * 'runblob' usa un blob de 4 bytes (una hoja 'add'+'ret').  Aqui el blob es
 * 'sqsum()' de sqsum.c: 120 bytes, dos bucles, marco de pila con
 * array local, dos llamadas internas PC-relativas y una escritura a traves de
 * un puntero que pasa el llamante.  Sirve para ver lo que el blob pequeno tapa:
 *
 *   - el blob ya no cabe en una linea de cache (64 B): ocupa 2, asi que la
 *     copia y la verificacion tienen que ser palabra a palabra, no de una
 *     palabra.  Un blob multi-linea puede medio caer en la celda buena y medio
 *     en la linea rancia de L2; la falla entonces no es "no ejecuta", es
 *     "ejecuta mitad y mitad", que es peor porque no es determinista.
 *   - la pila del blob es la pila del shell, en L2MEM y CON cache: el codigo
 *     que se ejecuta sin cache (por 0x88) accede a datos que si van por cache,
 *     y ahi no hay problema -- son caminos distintos y los datos van por L1-D.
 *   - el blob no puede tener datos propios.  Enlazado en 0, cualquier
 *     referencia a su .rodata calcularia la direccion cerca de 0x0000xxxx y
 *     petaria con mcause=5; por eso todos los punteros los inyecta el
 *     llamante como argumentos.  Asi se ve por que un cargador runtime necesita
 *     O BIEN codigo sin datos, O BIEN relocalizar (vease el GOT que nunca llega
 *     a correr 'elf' en este arbol).
 *
 * La coherencia esta ya medida en 'runblob' (vease su encabezado): el fetch de
 * instrucciones sube icache -> L2 -> PSRAM, y copiar por 0x88 no invalida la
 * linea L2 rancia.  Aqui se ejecuta por la ventana directa (0x88), que es la
 * ruta probada y la que no necesita operaciones de cache.
 *
 * Modos:
 *   runbig         copiar por 0x88, verificar palabra a palabra, saltar por 0x88
 *   runbig por48   la misma copia, pero saltando por 0x48 -> se espera la
 *                  excepcion mcause=2; es opt-in y para ver el contraste
 *
 * Regenerar el blob: 'make' en este directorio (sqsum_blob.h se queda aqui).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>                       /* psram_malloc() / psram_freemem()  */

#include "sqsum_blob.h"                   /* generado por 'make' en ese dir.   */

/** Desplazamiento de la ventana directa de la PSRAM (PSRAM_CPU_UNCACHED en
 *  device/psram/psramInit.c:36). */
#define PSRAM_DIRECT      0x40000000U

/** Palabra i del blob, recomuesta desde los bytes del array. */
#define BLOB_WORD(i)      ((uint32_t)sqsum_blob[(i)]      | \
                           ((uint32_t)sqsum_blob[(i) + 1] << 8)  | \
                           ((uint32_t)sqsum_blob[(i) + 2] << 16) | \
                           ((uint32_t)sqsum_blob[(i) + 3] << 24))

shellcmd xsh_runbig(int nargs, char *args[])
{
    uint8_t   *cached;                    /* ventana con cache (0x48......)    */
    uint8_t   *direct;                    /* ventana directa  (0x88......)     */
    volatile uint32_t *w;
    int (*fn)(const int *, int, int, int *);
    const int  v1[4] = { 1, 2, 3, 4 };
    const int  v2[2] = { 7, 2 };
    int        mx;
    int        r;
    uint32_t   esperado, leido;
    uint32_t   i, malas, linea0, lineas;
    bool       por48 = (nargs > 1 && strcmp(args[1], "por48") == 0);

    /* Paso 0: tamano real del blob y cuantas lineas de cache abarca.  Con 120
     * bytes ya son 2 lineas de 64, asi que no vale comprobar "la primera
     * palabra" como en 'runblob'. */
    kprintf("blob sqsum: len=%u  palabras=%u\r\n",
            sqsum_blob_len, sqsum_blob_len / 4U);

    cached = (uint8_t *)psram_malloc(sqsum_blob_len);
    if (NULL == cached) {
        kprintf("runbig: psram_malloc fallo\r\n");
        return SYSERR;
    }
    direct = cached + PSRAM_DIRECT;

    /* Lineas que toca este bloque: se calcula con la direccion de la celda, no
     * con el tamano a secas -- un bloque mal alineado puede ocupar una linea
     * mas de lo que sugiere 120/64. */
    linea0 = ((uintptr_t)direct & 63UL);
    lineas = (linea0 + sqsum_blob_len + 63UL) / 64UL;
    kprintf("bloq con cache =0x%08lx  directo =0x%08lx  offset_en_linea=%lu  lineas=%lu\r\n",
            (unsigned long)(uintptr_t)cached,
            (unsigned long)(uintptr_t)direct,
            (unsigned long)linea0,
            (unsigned long)lineas);

    /* Paso 1: envenenar TODA la zona por la ventana directa, no solo la primera
     * palabra: en un blob de 120 bytes hay 30 palabras que pueden estar a
     * medias. */
    memset(direct, 0xA5, sqsum_blob_len);

    /* Paso 2: copiar por la ventana directa. */
    memcpy(direct, sqsum_blob, sqsum_blob_len);

    /* Paso 3: verificar palabra a palabra POR LA VENTANA DIRECTA (la que ve la
     * celda).  Se cuentan las que no coinciden en vez de parar en la primera,
     * porque el patron de fallos importa: si fallan las de despues de la linea
     * 64, el problema es de cobertura, no de copia. */
    malas = 0;
    for (i = 0; i + 3U < sqsum_blob_len; i += 4U) {
        w = (volatile uint32_t *)(direct + i);
        esperado = BLOB_WORD(i);
        leido = *w;
        if (esperado != leido) {
            if (malas < 4U) {
                kprintf("  palabra %lu: esperado=0x%08x leido=0x%08x\r\n",
                        (unsigned long)(i / 4U), (unsigned)esperado,
                        (unsigned)leido);
            }
            malas++;
        }
    }
    kprintf("verificacion de celda: %lu/%lu palabras MALAS -> %s\r\n",
            (unsigned long)malas, (unsigned long)(sqsum_blob_len / 4U),
            (malas == 0U) ? "OK" : "FALLO");
    if (malas != 0U) {
        psram_freemem(cached, sqsum_blob_len);
        return SYSERR;
    }

    /* Paso 4: saltar.  Por defecto por la ventana DIRECTA: es la ruta que no
     * necesita operaciones de cache y la que ya esta probada con 'runblob
     * directo'.  'runbig por48' salta por la con cache, donde la linea rancia
     * de L2 puede volver a servir basura en cuanto el bloque no sea nuevo del
     * todo. */
    fn = (int (*)(const int *, int, int, int *))(uintptr_t)
             (por48 ? cached : direct);
    kprintf("llamando a 0x%08lx por ventana %s...\r\n",
            (unsigned long)(uintptr_t)fn,
            por48 ? "CON CACHE (riesgo de linea rancia)" : "DIRECTA");

    /* Paso 5: dos llamadas con argumentos reales.  El vector y el puntero de
     * salida viven en la pila del shell (L2MEM, con cache): el blob los usa
     * desde PSRAM sin cache, y ahi no hay conflicto de caminos.
     *
     * Se guarda el resultado en una variable ANTES del kprintf: el orden de
     * evaluacion de los argumentos de una funcion no esta definido en C, asi que
     * escribir '%d %d', (*fn)(...), mx podria leer mx todavia con el -1. */
    mx = -1;
    r = (*fn)(v1, 4, 5, &mx);
    kprintf("sqsum({1,2,3,4},4,5)=%d  max=%d   (esperado 41 y 4)\r\n", r, mx);
    mx = -1;
    r = (*fn)(v2, 2, 1, &mx);
    kprintf("sqsum({7,2},2,1)=%d  max=%d   (esperado 63 y 7)\r\n", r, mx);

    psram_freemem(cached, sqsum_blob_len);

    return OK;
}
