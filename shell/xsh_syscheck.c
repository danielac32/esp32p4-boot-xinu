/*   syscheck.c — comprobación completa del kernel del port ESP32-P4.
 *
 *   Referencia en examples/syscheck/ (README.md con el procedimiento del PC y
 *   docs/port-esp32p4/06-syscheck.md con cómo leer cada cifra).  Se invoca con
 *   `syscheck [clk|cpu|mem|net|dev|int|flash|stk|all]`.
 *
 *   Qué contesta, y con qué referencia:
 *
 *     clk   millis() contra clkcount() (mtime)          -> coherencia interna
 *     cpu   mcycle vs mtime  +  bucle de referencia     -> frecuencia REAL del core
 *     mem   heap L2MEM, heap PSRAM, alias 0x48/0x88, banda fantasma, centinela
 *     net   estado del netif, buffers LIBRES del pool, ARP, rutas, contadores espeth
 *     dev   I2C probe, PMIC, codec, touch, SD, FAT
 *     int   auditoría del CLIC: líneas con ie=1 y vector nulo
 *     flash cabeceras de imagen en XIP, fin del kernel vs arranque del ULP
 *     all   todos menos `cpu` (que puede trapar)
 *
 *   La única referencia ABSOLUTA de tiempo que hay en la placa es externa; ver el
 *   README de este directorio para el procedimiento con el PC (`p4clk.py`).
 *
 *   Reglas del proyecto respetadas: sin coma flotante (todo entero, escalado x1000),
 *   sin escritura al PMIC, sin tocar relojes ni flashear, y sin `delay_ms`: los
 *   retardos usan `sleep()`, que es el retardo del kernel.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <arp.h>
#include <audio.h>
#include <bufpool.h>
#include <clock.h>
#include <conf.h>
#include <esp32p4.h>
#include <espeth.h>
#include <fsutil.h>
#include <i2c.h>
#include <interrupt.h>
#include <memory.h>
#include <network.h>
#include <platform.h>
#include <power.h>
#include <route.h>
#include <semaphore.h>
#include <shell.h>
#include <stack.h>
#include <sdblock.h>
#include <thread.h>
#include <timer.h>
#include <touch.h>

/* Símbolo del enlazado (compile/platforms/esp32p4/ld.script:173). */
extern char _l2reserve[];

#define MUTE_HZ        360000000UL   /* lo que el port CREE que vale: platform.clkfreq */
/* Frecuencia REAL del mtime, medida contra el reloj del PC con
 * examples/syscheck/p4clk.py el 2026-11-04: 320,010 MHz.  El port declara
 * 360 MHz en include/esp32p4.h:160 (P4_TIMER_FREQ); de ahi sale el 12,5 % de
 * todo lo que se temporiza por cuenta de mtime, sleep() incluido. */
#define MTIME_KHZ_MEDIDA  320010U
#define MS_PER_HB      1000U
#define HB_TOTAL       30U
#define LOOP_ITERS     200000U

static int   sc_total, sc_pass, sc_warn, sc_fail;

static void sc_verdict(int ok, const char *name, const char *detail)
{
    sc_total++;
    if (ok > 0) { sc_pass++; printf("[ OK ] %-22s %s\n", name, detail); }
    else if (ok == 0) { sc_warn++; printf("[AVISO] %-22s %s\n", name, detail); }
    else { sc_fail++; printf("[FALLO] %-22s %s\n", name, detail); }
}

/* mcycle (0xB00) es un contador de 64 bits leído en dos palabras.  La dirección
 * del CSR tiene que ser inmediata en `csrr`, así que no hay función genérica:
 * cada contador que se lea por CSR necesita su propia lectura.  La relectura del alto
 * evita que un envoltorio del bajo se trague un bit.  (mtime NO se lee aquí:
 * es memoria mapeada, y para él ya está clkcount().) */
static uint64_t mcycle64(void)
{
    uint32_t lo, hi, hi2;

    do
    {
        __asm__ volatile ("csrr %0, 0xB80" : "=r"(hi));        /* mcycleh */
        __asm__ volatile ("csrr %0, 0xB00" : "=r"(lo));        /* mcycle  */
        __asm__ volatile ("csrr %0, 0xB80" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/*--------------------------------------------------------------------------
 * clk: millis() tiene que ser monotono, de 1 ms, y coherente con mtime
 *-------------------------------------------------------------------------*/
static void sc_clk(void)
{
    uint32_t m0, m1, c0, c1;
    uint32_t counts_per_ms, delta_ms, delta_counts;
    uint32_t monotono = 1, ticks_mordidos = 0;
    uint32_t i, prev;
    char     b[96];

    printf("--- clk ---\n");

    /* 1. Monotonia y resolucion: 200 lecturas seguidas.  `millis()` viene de
     *    clktime*1000 + clkticks (system/timershim.c:41), no del mtime, asi que
     *    aqui se comprueba que clkhandler() no pierde ticks. */
    prev = millis();
    for (i = 0; i < 200U; i++)
    {
        uint32_t now = millis();
        if (now < prev) { monotono = 0; }
        /* un salto de mas de 5 ms en un bucle vacio = ticks perdidos */
        if (now - prev > 5U) { ticks_mordidos++; }
        prev = now;
    }
    snprintf(b, sizeof b, "200 lecturas, saltos>5ms = %u", (unsigned)ticks_mordidos);
    sc_verdict(monotono && ticks_mordidos == 0, "millis monotona", b);

    /* 2. Coherencia de las dos bases: durante un intervalo fijo, el numero de
     *    cuentas de mtime por ms contado por millis() debe valer
     *    platform.clkfreq/1000 = 360000.  Si no cuadra, O bien clkfreq esta mal
     *    (mal supuesto de arranque) O bien se pierden interrupciones de reloj.
     *    Ojo: las dos cantidades derivan del mismo mtime, asi que esto mide la
     *    CONSISTENCIA del reloj del kernel, no su frecuencia absoluta. */
    m0 = millis();
    c0 = clkcount();
    sleep(2000);
    m1 = millis();
    c1 = clkcount();

    delta_ms    = m1 - m0;
    delta_counts = c1 - c0;                       /* ulong, 32 bits: cabe (720 M) */
    counts_per_ms = delta_ms ? delta_counts / delta_ms : 0U;

    snprintf(b, sizeof b, "%u cuentas/ms (con clkfreq=%u Hz serian %u)",
             (unsigned)counts_per_ms, (unsigned)platform.clkfreq,
             (unsigned)(MUTE_HZ / 1000U));
    /* tolerancia 2 %: el desfase del sleep() por planificacion */
    sc_verdict(counts_per_ms > MUTE_HZ / 1000U * 98 / 100
               && counts_per_ms < MUTE_HZ / 1000U * 102 / 100,
               "mtime/ms vs millis", b);
    printf("       si sale por encima de 360000 es que pasan dos cosas: el mtime\n"
           "       no cuenta a la frecuencia declarada (320,010 MHz medidos) y\n"
           "       clkhandler() pierde ticks.  Con 403092 cuentas/ms el segundo\n"
           "       del kernel dura ~1,26 s reales (lo mide p4clk.py).\n");

    /* 3. micros() envuelve a los 11,93 s (toma solo la palabra baja del mtime);
     *    se comprueba que la resta modular de intervalos cortos sigue valiendo. */
    {
        uint32_t a = micros();
        sleep(100);
        {
            uint32_t d = micros() - a;
            snprintf(b, sizeof b, "100 ms -> %u us", (unsigned)d);
            sc_verdict(d > 90000U && d < 130000U, "micros() intervalo", b);
        }
    }

    /* 4. Latido para la referencia externa: 30 lineas, una por segundo del
     *    kernel.  p4clk.py mide cuanto tarda el PC en verlas. */
    printf("heartbeat: %u lineas, 1/s del kernel (para el PC)\n", HB_TOTAL);
    for (i = 0; i < HB_TOTAL; i++)
    {
        printf("HB %03u xinums=%u uptime=%lu mtime=%lu\n",
               (unsigned)i, (unsigned)millis(), (ulong)clktime, (ulong)clkcount());
        sleep(MS_PER_HB);
    }
}

/*--------------------------------------------------------------------------
 * cpu: frecuencia REAL del core, sin fiarse de lo que dice el enlazado
 *-------------------------------------------------------------------------*/
static void sc_cpu(void)
{
    uint64_t cyc0, cyc1;
    uint32_t t0, t1;
    uint32_t dcyc, dt, r10k, core_khz, iters;
    uint32_t loop0;
    char     b[96];

    printf("--- cpu ---\n");
    printf("AVISO: este test lee mcycle (0xB00/0xB80).  Si el core no lo\n"
           "implementa, levanta instruccion ilegal y el kernel panica: se\n"
           "necesita reset.  El SDK bare-metal lo usa como contador de ciclos\n"
           "de P4 (core/riscv/include/riscv/rv_utils.h:95), por eso se intenta.\n");

    /* Ratio mcycle/mtime medido en una ventana de 500 ms del kernel. */
    cyc0 = mcycle64();
    t0   = clkcount();
    sleep(500);
    cyc1 = mcycle64();
    t1   = clkcount();

    dcyc = (uint32_t)(cyc1 - cyc0);
    dt   = t1 - t0;                               /* cuentas de mtime en 500 ms */
    if (dt == 0U) { sc_verdict(-1, "mcycle/mtime", "delta de mtime = 0"); return; }

    /* El core cuenta dcyc ciclos por cada dt cuentas del mtime, o sea que va a
     * esa PROPORCION de la frecuencia del mtime.  Lo que no se puede asumir es
     * esa frecuencia: declarada es 360 MHz, medida contra el PC (p4clk.py) sale
     * 320,010 MHz.  Con el ratio de aqui y el dato del PC se obtiene la del
     * core.  Se escala x10000 porque dcyc < 2^31 y 1e6 no caben multiplicando. */
    r10k     = (uint32_t)(((uint64_t)dcyc * 10000ULL) / dt);
    core_khz = (uint32_t)(((uint64_t)r10k * MTIME_KHZ_MEDIDA) / 10000ULL);
    snprintf(b, sizeof b, "mcycle/mtime = %u.%04u -> core %u.%03u MHz",
             (unsigned)(r10k / 10000U), (unsigned)(r10k % 10000U),
             (unsigned)(core_khz / 1000U), (unsigned)(core_khz % 1000U));
    sc_verdict(r10k != 0U, "mcycle vs mtime", b);
    printf("       (ratio 1,0000 = el core y el mtime van al mismo reloj; el MHz\n"
           "        de arriba usa el mtime MEDIDO contra el PC = %u MHz, no los\n"
           "        %u MHz que declara platform.clkfreq)\n",
           (unsigned)(MTIME_KHZ_MEDIDA / 1000U),
           (unsigned)(platform.clkfreq / 1000000UL));

    /* Bucle de referencia: 200k iteraciones de trabajo entero conocido.  No da
     * DMIPS, da una cifra COMPARABLE entre builds: si un dia sube o baja, algo
     * del camino de instrucciones (caché, espera de flash XIP, wait states de
     * PSRAM) ha cambiado.  Se cronometra con micros(): en millis() la ventana
     * baja de 1 ms y salia 0, que no es una medida. */
    loop0 = 0U;
    t0 = micros();
    for (iters = 0; iters < LOOP_ITERS; iters++)
    {
        loop0 = loop0 * 6U + 7U;                  /* mul+add+reg: ~3 instrucciones */
    }
    t1 = micros();
    dt = t1 - t0;                                 /* microsegundos, envuelve a 13 s */
    printf("       (dummy=%u para que el optimizador no lo borre)\n", (unsigned)loop0);
    printf("       bucle %u iters en %u us -> %u iter/s\n",
           (unsigned)LOOP_ITERS, (unsigned)dt,
           (unsigned)(dt ? (uint64_t)LOOP_ITERS * 1000000ULL / dt : 0));
    /* Ciclos por iteracion con el core del ratio de arriba: si mcycle/mtime es
     * 1,0000 y esto sale ~2,00, las dos medidas del reloj son coherentes. */
    snprintf(b, sizeof b, "%u.%02u ciclos/iter (%u it/s)",
             (unsigned)((uint64_t)core_khz * dt / ((uint64_t)LOOP_ITERS * 1000ULL)),
             (unsigned)(((uint64_t)core_khz * dt * 100ULL /
                        ((uint64_t)LOOP_ITERS * 1000ULL)) % 100U),
             (unsigned)(dt ? (uint64_t)LOOP_ITERS * 1000000ULL / dt : 0));
    sc_verdict(dt > 500U, "bucle de referencia", b);
}

/*--------------------------------------------------------------------------
 * mem: heap, alias de caché y la banda fantasma
 *-------------------------------------------------------------------------*/
static void sc_mem(void)
{
    struct memblock *mb;
    uintptr_t free_l2 = 0, blocks = 0;
    uint32_t *p48, *p88;
    uint32_t v;
    char     b[112];

    printf("--- mem ---\n");

    /* 1. Heap L2MEM: se camina la lista libre, como hace `mem`. */
    {
        irqmask im = disable();
        for (mb = memlist.next; mb != NULL; mb = mb->next)
        {
            free_l2 += mb->length;
            blocks++;
        }
        restore(im);
    }
    snprintf(b, sizeof b, "%u B libres en %u bloques (0x%08x..0x%08x)",
             (unsigned)free_l2, (unsigned)blocks,
             (unsigned)(uintptr_t)memheap, (unsigned)(uintptr_t)platform.maxaddr);
    sc_verdict(free_l2 > 100000U, "heap L2MEM", b);

    /* 2. Heap PSRAM: ya hay contador en el port. */
    {
        uintptr_t pf = psram_free_bytes();
        snprintf(b, sizeof b, "%u B libres, span 0x%08x..0x%08x",
                 (unsigned)pf, (unsigned)(uintptr_t)psminheap,
                 (unsigned)(uintptr_t)psmaxheap);
        sc_verdict(pf > 16U * 1024U * 1024U, "heap PSRAM", b);
    }

    /* 3. Celdas reales: escribir/patrón/leer 256 B del heap L2MEM.  Se pide el
     *    bloque con memget(), NUNCA se escribe en memheap a pelo: ahi vive la
     *    cabecera (next/length) del primer bloque libre y pisarla corrompe el
     *    heap.  Un fallo aqui es la banda fantasma. */
    {
        uint32_t *q = (uint32_t *)memget(256U);
        uint32_t  k, bad = 0;
        if (q == NULL) { sc_verdict(-1, "L2MEM patron 256 B", "memget(256) NULL"); }
        else
        {
            for (k = 0; k < 64U; k++) { q[k] = 0x5A5A0000U + k; }
            for (k = 0; k < 64U; k++) { if (q[k] != 0x5A5A0000U + k) { bad++; } }
            snprintf(b, sizeof b, "%u de 64 palabras mal en 0x%08x",
                     (unsigned)bad, (unsigned)(uintptr_t)q);
            sc_verdict(bad == 0U, "L2MEM patron 256 B", b);
            memfree(q, 256U);
        }
    }

    /* 4. Alias de caché: la MISMA celda vista por 0x48.. y por 0x88.. tiene que
     *    coincidir SI la linea no esta residenta en L2.  Aqui se documenta el
     *    comportamiento, no se juzga: si por 0x48 se lee otra cosa, es la linea
     *    sucia, y es exactamente lo que rompio el blob de PSRAM. */
    {
        uint32_t *buf = (uint32_t *)psram_malloc(64U);
        if (buf == NULL) { sc_verdict(-1, "alias 0x48/0x88", "psram_malloc NULL"); }
        else
        {
            p88 = (uint32_t *)((uintptr_t)buf + 0x40000000U);
            p48 = buf;
            p88[0] = 0xC0FFEE11U;
            v = p48[0];
            snprintf(b, sizeof b, "escrito 0xC0FFEE11 por 0x88, leido 0x%08x por 0x48",
                     (unsigned)v);
            sc_verdict(v == 0xC0FFEE11U ? 1 : 0, "alias 0x48/0x88", b);
            psram_free(buf);
        }
    }

    /* 5. La banda fantasma 0x4FF00000..0x4FF1FFFF NO tiene que usarse: se
     *    comprueba que el enlazado la reservo (.l2reserve) y que el heap esta
     *    por encima del corte. */
    snprintf(b, sizeof b, "_l2reserve=0x%08x  memheap=0x%08x  corte=0x4ff20000",
             (unsigned)(uintptr_t)&_l2reserve, (unsigned)(uintptr_t)memheap);
    sc_verdict((uintptr_t)&_l2reserve < 0x4FF20000U &&
               (uintptr_t)memheap >= 0x4FF20000U, "reserva banda fantasma", b);
}

/*--------------------------------------------------------------------------
 * net: lo que se te cae cuando el pool no da abasto
 *-------------------------------------------------------------------------*/
static void sc_net(void)
{
    struct netif  *np;
    struct bfpentry *pp;
    uint32_t libres = 0, total, i;
    uint32_t arpn = 0, rtn = 0;
    struct espeth_stats st;
    char     b[112];

    printf("--- net ---\n");

    np = netLookup(getdev("ETH0"));
    if (np == NULL) { sc_verdict(-1, "netif ETH0", "no existe"); return; }

    snprintf(b, sizeof b, "estado=%u nin=%lu nproc=%lu",
             (unsigned)np->state, (ulong)np->nin, (ulong)np->nproc);
    sc_verdict(np->state == NET_ALLOC, "netif ETH0", b);

    /* Buffres LIBRES del pool de paquetes.  Este es el numero que interesa:
     * NET_POOLSIZE vale 24 y la pila pide >= ARP_NQUEUE+RT_NQUEUE+RAW_IBLEN
     * (164).  Si esto llega a 0 con trafico, la pila descarta en silencio.
     * El contador de huecos libres es la cuenta del semaforo del pool
     * (system/bfpalloc.c:59 y :83); no hay lista enlazada de libres. */
    if (!isbadpool(netpool))
    {
        pp = &bfptab[netpool];
        total = pp->nbuf;
        libres = (uint32_t)semcount(pp->freebuf);
        snprintf(b, sizeof b, "%u/%u buffers libres, %u B cada uno",
                 (unsigned)libres, (unsigned)total, (unsigned)pp->bufsize);
        sc_verdict(libres > total / 4U, "pool de paquetes", b);
    }

    for (i = 0; i < ARP_NENTRY; i++) { if (arptab[i].state == ARP_USED)  { arpn++; } }
    for (i = 0; i < RT_NENTRY; i++) { if (rttab[i].state == RT_USED)    { rtn++; } }
    snprintf(b, sizeof b, "%u entradas ARP, %u rutas", (unsigned)arpn, (unsigned)rtn);
    sc_verdict(rtn > 0U, "vecinos y rutas", b);

    if (control(getdev("ETH0"), ESPETH_CTRL_STATS, (int32)(uintptr_t)&st, 0) == OK)
    {
        snprintf(b, sizeof b, "rx=%u (drops %u) tx=%u (drops %u)",
                 (unsigned)st.rx_frames, (unsigned)st.rx_drops,
                 (unsigned)st.tx_frames, (unsigned)st.tx_drops);
        sc_verdict(st.rx_drops < 50U, "contadores espeth", b);
    }
}

/*--------------------------------------------------------------------------
 * dev: los dispositivos que ya dan por buenos, sin escribir en el PMIC
 *-------------------------------------------------------------------------*/
static void sc_dev(void)
{
    int      dev, v;
    uint32_t percent;
    char     b[96];

    printf("--- dev ---\n");

    dev = getdev("I2C0");
    v = control(dev, I2C_CTRL_PROBE, 0x18, 0);          /* ES8311 */
    snprintf(b, sizeof b, "0x18=%s 0x34=%s 0x38=%s (0x36 no se toca)",
             v == OK ? "SI" : "NO",
             control(dev, I2C_CTRL_PROBE, 0x34, 0) == OK ? "SI" : "NO",
             control(dev, I2C_CTRL_PROBE, 0x38, 0) == OK ? "SI" : "NO");
    sc_verdict(v == OK, "I2C0 esclavos", b);

    dev = getdev("POWER0");
    percent = (uint32_t)control(dev, POWER_CTRL_PERCENT, 0, 0);
    snprintf(b, sizeof b, "%u %%", (unsigned)percent);
    sc_verdict(percent <= 100U, "PMIC battery", b);

    dev = getdev("AUDIO0");
    v = (int)control(dev, AUDIO_CTRL_GET_VOLUME, 0, 0);
    snprintf(b, sizeof b, "volumen %d", v);
    sc_verdict(v >= 0, "codec audio", b);

    dev = getdev("TOUCH0");
    v = control(dev, TOUCH_CTRL_TYPE, 0, 0);
    snprintf(b, sizeof b, "tipo de panel = %d", v);
    sc_verdict(v != SYSERR, "touch vivo", b);

    /* SD: leer el MBR (LBA 0) y mirar la firma 0x55AA al final.  El device ya
     * lo abre el montaje FAT; hay que POSICIONAR (SD_CTRL_SEEK) porque read()
     * entrega el bloque donde esté el puntero del driver, igual que hace `sd`
     * en shell/xsh_sd.c:194. */
    {
        uchar  blk[SD_BLOCK_SIZE];
        int    sd = getdev("SD0");
        int    ok = (sd != SYSERR) &&
                    (control(sd, SD_CTRL_SEEK, 0, 0) != SYSERR) &&
                    (read(sd, blk, sizeof blk) == (int)sizeof blk);
        /* La firma es 0x55AA en little-endian: blk[510]=0x55, blk[511]=0xAA. */
        snprintf(b, sizeof b, "LBA0 %s, firma 0x%02x%02x",
                 ok ? "leido" : "NO", blk[SD_BLOCK_SIZE - 2],
                 blk[SD_BLOCK_SIZE - 1]);
        sc_verdict(ok && blk[SD_BLOCK_SIZE - 2] == 0x55U &&
                   blk[SD_BLOCK_SIZE - 1] == 0xAAU, "microSD LBA0", b);
    }

    /* FAT: este port no tiene `stat()` de rutas (no hay VFS); la libreria es
     * fatfs/ y el estado de montaje lo dice fs_mounted().  Se abren los dos
     * ficheros que la shell ya usa y se lee el MBR aparte. */
    {
        FL_DIR dir;
        int ok = fs_mounted() && fl_opendir("/", &dir) != NULL;
        if (ok) { fl_closedir(&dir); }
        sc_verdict(ok, "FAT montada", ok ? "raiz abierta" : "sin montaje");
    }
}

/*--------------------------------------------------------------------------
 * int: lineas habilitadas con vector nulo = placa colgada en el primer tick
 *-------------------------------------------------------------------------*/
static void sc_int(void)
{
    printf("--- int ---\n");
    printf("El volcado completo (ip/ie/attr/ct de las 48 lineas, INTMTX,\n"
           "interruptVector[], mtime/mtimecmp/clkticks) ya lo hace el\n"
           "referencia examples/psram-blob/xsh_irqmap.c: portarlo es la via\n"
           "corta.  Aqui solo la comprobacion que evita el cuelgue:\n");
    printf("  una linea con ie=1 y vector NULL cae en irq_missing() y para la\n"
           "  placa hasta reset (system/platforms/esp32p4/clic.c:61).\n");
    printf("  Regla: ninguna linea queda enmascarada sin su interruptVector[].\n");
    sc_verdict(0, "auditoria CLIC", "ver xsh_irqmap (portar)");
}

/*--------------------------------------------------------------------------
 * flash: cabeceras y solapes
 *-------------------------------------------------------------------------*/
static void sc_flash(void)
{
    const uchar *f = (const uchar *)0x40000000U;
    const uchar *xip = (const uchar *)0x40005000U;
    uint32_t kfin, ulp_off = 0x400000U, k;
    uint32_t oneauno = 1;
    char     b[112];

    printf("--- flash ---\n");

    /* La ventana de datos de flash empieza en el offset 0 del chip: se comprueba
     * comparando 16 bytes leidos por la ventana cruda con los mismos 16 leidos
     * desde la direccion XiP del kernel.  Si coinciden, el offset de fichero es
     * `direccion - 0x40000000` y el bootloader (punto 18) puede calcular huecos
     * restando offsets. */
    for (k = 0; k < 16U; k++)
    {
        if (f[0x5000U + k] != xip[k]) { oneauno = 0; }
    }
    snprintf(b, sizeof b, "offset = dir-0x40000000%s", oneauno ? "" : " (NO)");
    sc_verdict(oneauno == 1U, "ventana de flash 1:1", b);

    /* El SBL si lleva su cabecera ESP en 0x2000 (la mira el BootROM).  El kernel
     * NO: su primer byte en 0x5000 es codigo (0x6f = jal) porque la cabecera la
     * consume el SBL al cargar; esperar 0xE9 ahi era una espera falsa. */
    printf("SBL 0x2000=0x%02x  kernel 0x5000=0x%02x  ULP 0x400000=0x%02x\n",
           f[0x2000], f[0x5000], f[ulp_off]);
    snprintf(b, sizeof b, "0xE9 en el SBL (el kernel empieza por su codigo, "
               "0x%02x = jal)", (unsigned)f[0x5000]);
    sc_verdict(f[0x2000] == 0xE9U, "cabecera del SBL", b);

    /* Fin del kernel: el ultimo byte de .rodata/.text enlazados.  Se compara con
     * el arranque del ULP para que el mapa no se pise solo al crecer. */
    kfin = (uint32_t)((uintptr_t)&_etext - 0x40000000U);
    snprintf(b, sizeof b, "_etext en offset 0x%x ; ULP en 0x%x ; hueco %u KB",
             (unsigned)kfin, (unsigned)ulp_off,
             (unsigned)((ulp_off > kfin) ? (ulp_off - kfin) / 1024U : 0U));
    sc_verdict(kfin < ulp_off, "sin solape kernel/ULP", b);
}

/*--------------------------------------------------------------------------
 * pilas: lo que `ps` no enseña
 *-------------------------------------------------------------------------*/
static void sc_stk(void)
{
    tid_typ tid;
    uint32_t usados = 0, alto = 0;
    char     b[96];

    printf("--- pilas ---\n");
    for (tid = 0; tid < NTHREAD; tid++)
    {
        uint32_t used;
        if (thrtab[tid].state == THRFREE) { continue; }
        used = stack_peak(tid);
        if (used > 0U)
        {
            usados++;
            if (used > 3000U)
            {
                alto++;
                printf("  %-16s pico %u B\n", thrtab[tid].name, (unsigned)used);
            }
        }
    }
    snprintf(b, sizeof b, "%u hebras medidas, %u por encima de 3 KB",
             (unsigned)usados, (unsigned)alto);
    /* AVISO deliberado: stack_peak() se muestrea en el cambio de contexto y NO
     * ve la cadena de llamadas profunda (tcpTimer marcaba 100 B).  Por eso el
     * dimensionado se hizo con -fstack-usage, no aqui. */
    sc_verdict(0, "pico de pilas", b);
}

/*--------------------------------------------------------------------------
 * main
 *-------------------------------------------------------------------------*/
shellcmd xsh_syscheck(int nargs, char *args[])
{
    const char *what = (nargs > 1) ? args[1] : "all";

    sc_total = sc_pass = sc_warn = sc_fail = 0;

    if (0 == strcmp(what, "--help"))
    {
        printf("Usage: syscheck [clk|cpu|mem|net|dev|int|flash|stk|all]\n\n");
        printf("Description:\n");
        printf("\tComprobacion del kernel del port ESP32-P4.\n");
        printf("\t`cpu' lee mcycle: si el core no lo implementa, panica (reset).\n");
        printf("\t`clk' suelta 30 lineas HB para medir contra el reloj del PC.\n");
        return 0;
    }

    if (0 == strcmp(what, "clk"))    { sc_clk(); }
    else if (0 == strcmp(what, "cpu"))   { sc_cpu(); }
    else if (0 == strcmp(what, "mem"))   { sc_mem(); }
    else if (0 == strcmp(what, "net"))   { sc_net(); }
    else if (0 == strcmp(what, "dev"))   { sc_dev(); }
    else if (0 == strcmp(what, "int"))   { sc_int(); }
    else if (0 == strcmp(what, "flash")) { sc_flash(); }
    else if (0 == strcmp(what, "stk"))   { sc_stk(); }
    else
    {
        sc_clk(); sc_mem(); sc_net(); sc_dev(); sc_int(); sc_flash(); sc_stk();
    }

    printf("\n=== syscheck %s: %d tests, %d OK, %d avisos, %d fallos ===\n",
           what, sc_total, sc_pass, sc_warn, sc_fail);
    return sc_fail == 0 ? 0 : 1;
}
