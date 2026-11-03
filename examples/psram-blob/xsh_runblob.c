/**
 * @file xsh_runblob.c
 *
 * CARGADOR DE REFERENCIA, NO COMPILADO: examples/ no esta en COMPS
 * (compile/Makefile:242).  Vease README.md de este directorio para como
 * engancharlo en la shell si algun dia se quiere el comando.
 *
 * Procedimiento CORRECTO para ejecutar un blob de codigo copiado en PSRAM a
 * runtime.  El blob es el 'sum(a,b)' de sum.c, volcado por 'make' a
 * sum_blob.h, que es un fichero GENERADO de este mismo directorio.  Cuatro
 * modos, para comparar:
 *
 *   runblob          copiar por 0x88 y saltar por 0x48  -> FALLA (medido): la
 *                  celda es correcta pero el fetch se sirve de la linea L2 rancia
 *   runblob mal      copiar por 0x48 y saltar por 0x48  -> FALLA igual, y el
 *                  readback engaña porque lo sirve L1-D
 *   runblob directo  copiar por 0x88 y saltar por 0x88  -> FUNCIONA (medido):
 *                  'sum(2,3)=5 sum(40,2)=42'.  Sin icache ni L2 en el camino el
 *                  fetch lee la celda tal cual.  Es la via que no necesita
 *                  operaciones de cache, a cambio de ir sin caché de instrucciones
 *   runblob thrash   copiar por 0x88, barrido de 256 KB de LECTURAS por 0x48 para
 *                  desalojar la linea L2 vieja, y saltar por 0x48 -> comprueba el
 *                  modelo de desalojo; leido por 0x48 debe verse ya el blob
 *
 * ---- DONDE ESTA EL ERROR DEL INTENTO INGENUO -------------------------------
 *
 * Lo que NO esta mal: reservar con psram_malloc() y saltar a la direccion con
 * cache que devuelve.  Ejecutar por 0x48...... es legitimo y es por donde hay
 * que ejecutar.  En el P4 no existe ninguna regla de "direccion con cache = no
 * ejecutable": el MMU de memoria externa solo tiene bits de validez y de tipo
 * (flash o PSRAM), sin permisos lectura/escritura/ejecucion -- ver
 * MMU_PSRAM_TLB_MARK_AS_VALID_ENTRY en machine/sbl/mmu.c:28 y el RAM(rwx) del
 * compile/platforms/esp32p4/ld.script:27, que ya declara PSRAM ejecutable.  Y
 * este kernel no configura ningun PMP, asi que tampoco hay filtro ahi.
 *
 * Lo que SI esta mal es asumir que, con los bytes ya en la celda, el fetch los
 * ve.  El camino de instrucciones es icache -> L2 -> PSRAM, y ninguna de las dos
 * ventanas fuerza la coherencia de ese camino (ni escribir por 0x88, ni el
 * readback por 0x48): hace falta una operacion de cache explicita, o ejecutar
 * por la ventana directa.  La PSRAM responde en dos direcciones sobre las MISMAS
 * celdas (TRM 7.3.3.1, ya documentado en device/psram/psramInit.c:28):
 *
 *     0x48......  ventana con cache  -> pasa por L1-D y L2
 *     0x88......  ventana directa    -> el CPU va recto a la celda
 *
 *   - memcpy() por 0x48 deja los bytes SUCIOS en L1-D/L2 (write-back).  La
 *     busqueda de instrucciones va por la L1 de instrucciones, que es un array
 *     APARTE (machine/sbl/esp32p4.h:6233), y detras de ella esta la L2.  El
 *     fetch no va a la celda: sube icache(miss) -> L2(hit) y se sirve de ahi.
 *     Se ejecuta basura: mcause=2 (instruccion ilegal) o un salto a donde no
 *     debe.  Y la comprobacion engañosa es leer de vuelta POR LA MISMA VENTANA
 *     0x48: eso lo sirve L1-D, que si tiene el dato nuevo.
 *
 *   - Escribir por 0x88 (address-through) si llega a la celda, pero NO es
 *     suficiente: salva L1-D y L2 sin invalidarlos, asi que la linea vieja que
 *     L2 ya tenia para ese bloque sigue ahi y el fetch sigue sirviendo basura.
 *     MEDIDO en placa: celda verificada correcta por 0x88 y aun asi
 *     mepc=0x48027008 mcause=2 mtval=0xaaaaaaab (0xaaaa... es contenido viejo
 *     del bloque; el kernel nunca borra la PSRAM al liberar).  En la variante
 *     'mal' el mtval=0x20aa coincide con la mitad baja de la celda vieja
 *     (0xaaaa20aa): mismo mecanismo, dos caminos distintos hacia la linea L2.
 *
 *   - Consecuencia: para ejecutar codigo copiado a runtime hace falta una
 *     OPERACION DE CACHE explicita, no cambiar de ventana.  Receta canonica:
 *     copiar por 0x48 -> writeback+invalidate L1-D y L2 sobre el rango ->
 *     invalidate de la icache de ESTE nucleo -> 'fence.i' -> saltar.  El
 *     bloque esta en include/soc/cache_struct.h: CACHE.sync_map (bits [0]
 *     L1-ICache0, [4] L1-DCache, [5] L2-Cache), CACHE.sync_addr (direccion
 *     virtual), CACHE.sync_size y CACHE.sync_ctrl (invalidate_ena / clean_ena /
 *     writeback_ena / writeback_invalidate_ena, mutuamente excluyentes, con bit
 *     de hecho).  Los envoltorios de include/hal/cache_ll.h no sirven aqui:
 *     delegan en los simbolos de ROM Cache_Invalidate_Addr /
 *     Cache_WriteBack_Addr, y este arbol no trae la tabla de funciones ROM.
 *
 *   - El atajo que si funciona sin tocar la cache es 'runblob directo': saltar
 *     por la ventana 0x88, donde no hay linea de icache ni de L2 que valga para
 *     esa direccion y el codigo se lee recto de la celda.  Castiga el rendimiento
 *     (ni icache ni L2), pero para un blob pequeno es la ruta honesta.
 *
 * Otras dos causas que hay que descartar antes de culpar a la cache, porque
 * producen la MISIMA excepcion mcause=2:
 *
 *   - Pegar un ELF en vez de un binario plano.  Un ELF empieza por e_ident
 *     (0x7F 'E' 'L' 'F') y por la cabecera de programa; saltar al offset 0 de
 *     un ELF ejecuta la cabecera como instrucciones.  Hay que pasar por
 *     objcopy -O binary, y enlazar en 0 para que la entrada caiga en el offset
 *     0 (vease link.ld de este directorio).
 *   - Un blob con relocations sin resolver (-fPIC, llamadas a funciones
 *     externas, tabla de saltos).  Un 'int sum(int a,int b){return a+b;}' de
 *     hoja no genera ninguna: sale 'add a0,a0,a1' + 'ret', 4 bytes, cero
 *     relocations.  Compruebelo con 'riscv32-unknown-elf-objdump -R'.
 *
 * Nota de recarga: tras ejecutar una vez, la icache de este nucleo ya tiene
 * linea de esa direccion -- y la linea es de 64 bytes como minimo
 * (MIN_L1_ICACHE_LINE_SIZE en include/esp32p4/rom/cache.h:31; configurable hasta
 * 128, enum cache_line_size_t en :110), mucho mas que este blob --, asi que
 * OTRO blob en el MISMO bloque exige volver a pasar por CACHE.sync_* (ahora
 * tambien el bit de icache).  Para el caso normal, que es cargar y llamar una
 * vez sobre un bloque recien reservado, basta con la receta de arriba; y si se
 * quiere evitar cualquier operacion de cache, ejecutar por 0x88
 * ('runblob directo').
 *
 * Aviso sobre 'fence.i' (la extension Zifencei si esta en el -march del
 * proyecto): invalida la icache del nucleo, pero no dice nada del contenido de
 * L2, y lo que esta aqui envenenado es precisamente la linea de L2.  No lo
 * tomes como el arreglo completo sin medirlo.
 *
 * Otro detalle del entorno: el printf() de Xinu (lib/libxc/doprnt.c) no
 * implementa %p.  Las direcciones se imprimen con 0x%08lx y un cast, como en
 * todo el arbol.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>                       /* psram_malloc() / psram_freemem()  */

#include "sum_blob.h"                     /* generado por 'make' en este dir.  */

/** Desplazamiento de la ventana directa de la PSRAM (mismo valor que
 *  PSRAM_CPU_UNCACHED en device/psram/psramInit.c:36). */
#define PSRAM_DIRECT      0x40000000U

#define PSRAM_CACHED_BASE 0x48000000U
/* Barrido de 256 KB, el doble de la L2 (128 KB).  ANTES eran 4 MB y eso mato la
 * placa: el lazo largo deja pasar un tick del reloj y el dispatch cayo en
 * irq_missing() de system/platforms/esp32p4/clic.c:61, que es un for(;;) -- la
 * placa se queda parada hasta reset.  La linea que disparo es id 7 = IRQ_TIMER
 * (include/conf.h:75), el machine timer de RISC-V: lo habilita clkinit()
 * (system/clkinit.c:56-57) via enable_irq y lo genera el CLINT comparando mtime
 * contra mtimecmp (system/platforms/esp32p4/riscvclk.c:39-46).  OJO: NO es
 * PMU1_IRQn=7 -- numero de fuente ETS heredado del enum de ESP-IDF en
 * machine/sbl/esp32p4.h:30, copiado tambien en include/esp32p4.h:108; otro
 * espacio de nombres, otra cosa.
 * Lo raro de la medida es interruptVector[7]=0 CON ie=1: en este arbol solo lo
 * escriben clic_init() (-> p4_irq_default), clkinit() (-> clkhandler) y
 * uartHwInit() (el del device); ninguno escribe 0.  Es decir, la tabla del .bss
 * estaba PISADA, y el sospechoso natural son las corridas anteriores de este
 * mismo comando: al saltar por 0x48 con la linea L2 rancia el nucleo ejecuto
 * basura (mtval=0xaaaaaaab), y una instruccion sacada de basura puede guardar en
 * una direccion cualquiera.  Nada que ver con el blob como tal: es una mina del
 * kernel para cualquier lazo largo.  Con 256 KB el barrido son ~4 ms. */
#define EVICT_SPAN        0x00040000U
#define EVICT_STRIDE      64U             /* tamano de linea de L2/ICache */

/** Primera palabra del blob, leida del array para poder contrastarla in situ. */
#define BLOB_WORD(i)      ((uint32_t)sum_blob[(i)]      | \
                           ((uint32_t)sum_blob[(i) + 1] << 8)  | \
                           ((uint32_t)sum_blob[(i) + 2] << 16) | \
                           ((uint32_t)sum_blob[(i) + 3] << 24))

shellcmd xsh_runblob(int nargs, char *args[])
{
    uint8_t  *cached;                     /* ventana con cache (0x48......)     */
    uint8_t  *direct;                     /* ventana directa  (0x88......)     */
    uint32_t *w;
    int (*fn)(int, int);
    uint32_t  esperado, leido;
    volatile uint32_t *scan;
    uint32_t  sink;
    bool      malo       = (nargs > 1 && strcmp(args[1], "mal") == 0);
    bool      porDirecta = (nargs > 1 && strcmp(args[1], "directo") == 0);
    bool      desalojo   = (nargs > 1 && strcmp(args[1], "thrash") == 0);

    (void)nargs;

    /* Paso 0: que se vea lo que hay dentro del blob.  Con -march con 'C' son
     * dos instrucciones comprimidas de 16 bits: add a0,a0,a1 + ret.  Cuatro
     * bytes en total, y la entrada esta en el offset 0 porque se enlazo en 0
     * (link.ld). */
    kprintf("blob: len=%u  palabra[0]=0x%08x  (= 0x952e add a0,a0,a1 / 0x8082 ret)\r\n",
            sum_blob_len, (unsigned)BLOB_WORD(0));

    /* Paso 1: reservar.  Alinear importa: sin la extension 'C' el fetch exige
     * 4 bytes; con 'C' basta 2.  psram_malloc redondea a multiples de bloque
     * minimo, asi que siempre sale al menos a 4. */
    cached = (uint8_t *)psram_malloc(sum_blob_len);
    if (NULL == cached) {
        kprintf("runblob: psram_malloc fallo\r\n");
        return SYSERR;
    }
    direct = cached + PSRAM_DIRECT;

    kprintf("bloq con cache =0x%08lx  directo =0x%08lx  alineacion=%lu\r\n",
            (unsigned long)(uintptr_t)cached,
            (unsigned long)(uintptr_t)direct,
            (unsigned long)((uintptr_t)cached & 3UL));

    if (malo) {
        /* ---- LA FORMA MALA, para ver la diferencia --------------------- */
        /* memcpy por la ventana con cache: los bytes se quedan sucios en
         * L1-D/L2.  Lo que hay en la celda puede ser lo viejo del bloque. */
        memcpy(cached, sum_blob, sum_blob_len);

        /* Se engana a si mismo leyendo por la MISMA ventana con cache: esa
         * lectura la sirve L1-D, que si tiene el dato nuevo, y parece que todo
         * esta bien.  La que no lo tiene es la icache. */
        kprintf("[-mal-] lectura con cache=0x%08x (sale bien, es engano)\r\n",
                *(volatile uint32_t *)cached);

        w = (uint32_t *)direct;
        kprintf("[-mal-] lectura directa =0x%08x (la celda real)\r\n", *w);
        esperado = BLOB_WORD(0);
        kprintf("[-mal-] esperado        =0x%08x\r\n", (unsigned)esperado);

        fn = (int (*)(int, int))(uintptr_t)cached;
        kprintf("[-mal-] llamando... si mtval sale igual a la celda real, es la linea L2/icache\r\n");
        kprintf("[-mal-] sum(2,3)=%d\r\n", fn(2, 3));

        psram_freemem(cached, sum_blob_len);
        return OK;
    }

    /* ---- LA FORMA QUE UNO CREE CORRECTA (y medida: tampoco basta) ------- */

    /* Paso 2: envenenar la celda a traves de la ventana directa.  Si despues
     * la lectura directa sale distinta del veneno, la copia aterrizo de
     * verdad.  Sin esto no se puede distinguir "ejecuto lo ultimo" de
     * "ejecuto basura que ya estaba". */
    memset(direct, 0xA5, sum_blob_len);

    /* Paso 3: copiar el blob POR LA VENTANA DIRECTA.  Este es el punto del
     * ejercicio: el almacenamiento llega a la celda, no a una linea sucia. */
    memcpy(direct, sum_blob, sum_blob_len);

    /* Paso 4: verificar releyendo por la ventana directa, palabra a palabra.
     * Es la comprobacion que falta en el intento que dio excepcion. */
    esperado = BLOB_WORD(0);
    w = (uint32_t *)direct;
    leido = *w;
    kprintf("celda: esperado=0x%08x leido=0x%08x -> %s\r\n",
            (unsigned)esperado, (unsigned)leido,
            (esperado == leido) ? "OK" : "NO COINCIDE");
    if (esperado != leido) {
        psram_freemem(cached, sum_blob_len);
        return SYSERR;
    }

    /* Paso 5a: el desalojo.  'runblob thrash' barre 256 KB de PSRAM LEYENDO por
     * la ventana con cache, con paso de una linea: es el doble de la L2 (128 KB),
     * suficiente para que cualquier linea vieja que L2 tuviera para este bloque
     * salga disparada.  Son solo lecturas: no toca el contenido de nada.  Que
     * 0x88 y 0x48 son las MISMAS celdas ya no hace falta probarlo aqui -- es el
     * truco que usa la ruta de audio que va en el kernel desde hace semanas
     * (PSRAM_CPU_UNCACHED en device/psram/psramInit.c:36 y REC_PCM_DIRECT en
     * shell/xsh_dash.c): el DMA escribe y el CPU relee por +0x40000000.  Lo que
     * se comprueba aqui es el modelo de desalojo: si tras el barrido la lectura
     * por 0x48 ya muestra el blob y el salto funciona, la L2 rancia era la causa
     * unica. */
    if (desalojo) {
        sink = 0;
        for (scan = (volatile uint32_t *)PSRAM_CACHED_BASE;
             scan < (volatile uint32_t *)(PSRAM_CACHED_BASE + EVICT_SPAN);
             scan = (volatile uint32_t *)((uint8_t *)scan + EVICT_STRIDE)) {
            sink += *scan;
        }
        kprintf("[-thrash-] barrido de %lu KB por 0x48 hecho (checksum 0x%08x)\r\n",
                (unsigned long)(EVICT_SPAN / 1024U), (unsigned)sink);
        kprintf("[-thrash-] celda por 0x88 =0x%08x   lectura por 0x48 =0x%08x\r\n",
                *(volatile uint32_t *)direct, *(volatile uint32_t *)cached);
        kprintf("[-thrash-] si la lectura por 0x48 ya vale 0x%08x, la linea rancia\r\n"
                "            se desalojo y el salto por 0x48 tiene que ir\r\n",
                (unsigned)BLOB_WORD(0));
    }

    /* Paso 5b: saltar.  Aqui esta la medida que desmonta la explicacion simple.
     * La celda esta buena (paso 4), pero el fetch de instrucciones NO va a la
     * celda: sube icache(miss) -> L2.  Y L2 sigue teniendo la linea que dejo
     * el kernel cuando toco este bloque por 0x48, porque escribir por 0x88
     * (address-through) salva L1-D y L2 sin invalidarlos.  Por eso 'runblob'
     * truena con mcause=2 y mtval = basura vieja, aunque la celda sea correcta.
     *
     * 'runblob directo' es la prueba que lo aísla: se salta por la ventana
     * DIRECTA (0x88), por la que no hay linea de icache ni de L2 que valga, asi
     * que el codigo se lee recto de la celda.  'runblob thrash' es la prueba
     * decisiva (vease el paso 5a). */
    fn = (int (*)(int, int))(uintptr_t)(porDirecta ? direct : cached);
    kprintf("llamando a 0x%08lx por ventana %s%s...\r\n",
            (unsigned long)(uintptr_t)fn,
            porDirecta ? "DIRECTA (sin cache)" : "CON CACHE",
            desalojo ? " tras desalojar L2" : "");
    kprintf("sum(2,3)=%d   sum(40,2)=%d\r\n", fn(2, 3), fn(40, 2));

    /* Paso 6: liberar.  No reutilizar el bloque para otro blob sin invalidar
     * la icache (vease el encabezado). */
    psram_freemem(cached, sum_blob_len);

    return OK;
}
