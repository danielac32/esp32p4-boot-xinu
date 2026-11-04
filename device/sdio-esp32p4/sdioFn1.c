/**
 * @file sdioFn1.c
 *
 * Transporte de la FUNCION 1 del ESP32-C6 sobre el slot 1 (W2).  W1 dejo el
 * C6 enumerado y la funcion 1 habilitada; aqui se prueba el CAMINO DE DATOS:
 * los accesos CMD52/CMD53 al espacio de registros de la funcion 1 que el slave
 * esp_hosted expone, y la ventana de datos (FIFO) desde la que se leen y se le
 * escriben las tramas del canal de control.
 *
 * El modelo del driver de referencia (drivers/esp_hosted/esp_hosted_sdio.c) NO
 * usa un bloque de memoria compartida con anillos: todo el plano de control es
 * un puntero de sondeo sobre cinco registros de la funcion 1 mas la cola de la
 * ventana FIFO en 0x1F800.  Los registros (offsets dentro del espacio de la
 * funcion 1, enmascarados a 10 bits con 0x3FF):
 *
 *   0x044  TOKEN_RDATA   [31:16]&0xFFF = buffers H2C consumidos (productor C6)
 *   0x050  INT_RAW       interrupcion cruda dispositivo->host (bit23 = paquete
 *                        nuevo; bit7/b6 = throttle start/stop)
 *   0x060  PACKET_LEN    &0xFFFFF = bytes C2H acumulados (productor C6)
 *   0x08C  HOST_TO_SLAVE Escribe BIT(n) para levantar una IRQ al C6
 *   0x0D4  INT_CLR       Se reescribe INT_RAW para acknowledges
 *   0x1F800              fin de la ventana de datos; una trama de len bytes se
 *                        lee/escribe en 0x1F800 - len.
 *
 * Cada CMD53 usa sdTransferSlot() (la fase de datos del host, en sdHw.c) sobre
 * la rebote de 512 bytes; los registros de 4 bytes viajan en modo byte.  Todos
 * los valores impresos se LEEN del propio periférico, no de un caché.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>                       /* delay_ms                          */

#include <sdio.h>                        /* struct sdioinfo + sdioEnumerate   */
#include <gpio.h>                        /* gpio_set_dir/gpio_write + board.h */
#include <device/sdmmc-esp32p4/sdhost.h> /* sdSendCmdSlot/sdTransferSlot/...  */

/* ------------------------------------------------------------------ */
/* Slot y comandos SDIO                                                 */
/* ------------------------------------------------------------------ */
#define SDIO_SLOT           1U

#define SDIO_CMD52          52U
#define SDIO_CMD53          53U

#define SDIO_FN0            0U
#define SDIO_FN1            1U

/* Máscara del espacio de direcciones visible por CMD53/52 en la funcion 1
 * (el driver de referencia enmascara los offsets del slave con 0x3FF). */
#define FN_ADDR_MASK        0x3FFU

/* Registros de la funcion 1 (offsets ya dentro de la mascara de 10 bits). */
#define FN1_TOKEN_RDATA     0x044U
#define FN1_INT_RAW         0x050U
#define FN1_PACKET_LEN      0x060U
#define FN1_HOST_TO_SLAVE   0x08CU
#define FN1_INT_CLR         0x0D4U

/* CCCR (funcion 0) que el driver de referencia configura ANTES del primer
 * CMD53.  Sin ellos el slave esp_hosted mantiene DAT0 bajo (se ve en STATUS
 * bit 9 data_busy) y sdTransferSlot aborta en la limpieza previa. */
#define CCCR_INT_ENABLE     0x04U   /* bit0 = maestro, bit1 = IRQ de fn1      */
#define CCCR_INT_MASTER     (1U << 0)
#define CCCR_INT_FN1        (1U << 1)
#define CCCR_BUS_WIDTH      0x07U   /* [1:0]: 0=1-bit, 2=4-bit                */
#define CCCR_WIDTH_4BIT     2U
#define CCCR_BLKSIZE_FN0_L  0x10U   /* tamano de bloque de fn0 (bajo)         */
#define CCCR_BLKSIZE_FN0_H  0x11U   /* tamano de bloque de fn0 (alto)         */
#define CCCR_BLKSIZE_FN1_L  0x110U  /* fn1 = 0x100 + 0x10, accesible como fn0 */
#define CCCR_BLKSIZE_FN1_H  0x111U
#define FN1_BLOCK_SIZE      512U    /* 0x0200: bajo 0x00, alto 0x02           */

/* CTYPE del host: bit por slot, slot 1 = bit 1 -> bus de 4 lineas. */
#define HOST_CTYPE_W4_SLOT1 (1U << SDIO_SLOT)

/* Divisor de reloj para la fase de datos del slot 1: 40 MHz / (5*2) = 4 MHz.
 * Modesto a proposito: la matriz GPIO del C6 no es el IO_MUX, asi que se
 * empieza bajo y se sube solo cuando el CMD53 demuestre mover datos. */
#define SD_CARD_DIV_FN1     5U

/* Los dos divisores GLOBALES con los que se prueba la fase de datos, ambos con
 * el divisor por-slot en SALTO (card_div=0) para que el reloj que sale por la
 * pata sea exactamente el mismo que el CIU usa para muestrear:
 *   7  -> 22,857 kHz, el reloj de transferencia del driver de referencia
 *         (host_div=ceil(160000/25000)=7 y card_div=0; ver esp_hosted_sdio.c
 *        _calc_host_div y su set_clock del attach).
 *   16 -> 10,000 kHz, lo mas bajo que permiten los campos edge_* de 4 bits.
 * El reference NUNCA mueve un CMD53 con card_div != 0: su unica configuracion
 * de sondas es 400 kHz y ahi solo hace comandos por la linea CMD. */
#define SD_HOST_DIV_REF     7U
#define SD_HOST_DIV_SLOW    16U

/* Cola de la ventana de datos (0x20000 - 0x800). */
#define FN1_FIFO_END        0x1F800U

/* Bits de INT_RAW. */
#define INT_NEW_PACKET      (1U << 23)
#define INT_START_THROTTLE  (1U << 7)
#define INT_STOP_THROTTLE   (1U << 6)

/* Nudge de apertura del camino de datos (intr 0 hacia el slave). */
#define HOST_INTR_OPEN_DATA 0U

/* Máscaras de conteo del slave. */
#define SLAVE_LEN_MASK      0x000FFFFFUL   /* PACKET_LEN es acumulativo mod 2^20 */
#define TX_BUF_MASK         0x0FFFU        /* TOKEN es acumulado mod 4096         */

/* ------------------------------------------------------------------ */
/* CMD52: un byte del espacio de una funcion (raw=0)                    */
/* ------------------------------------------------------------------ */

/**
 * Leer o escribir un byte en la funcion \p fn.  Argumento:
 * [31]=write [30:28]=fn [27]=raw [25:9]=reg [7:0]=dato.  R5 trae el byte en
 * [7:0] para las lecturas.
 */
static bool fn1Cmd52Byte(uint8 fn, uint32 reg, bool write, uint8 *data)
{
    uint32 arg = ((uint32)write << 31) | ((uint32)fn << 28)
                 | ((reg & 0x1FFFFUL) << 9) | (uint32)*data;
    uint32 resp = 0;

    if (!sdSendCmdSlot(SDIO_SLOT, SDIO_CMD52, arg, 1, TRUE, FALSE, &resp))
    {
        return FALSE;
    }
    *data = (uint8)(resp & 0xFFU);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* CMD53 modo byte: una pasada de <= 512 bytes, autoincremento          */
/* ------------------------------------------------------------------ */

/* Diagnostico del primer fallo CMD53 de la sesion (lo imprime la sonda). */
static uint   s_cmd53ErrSeen;
static uint32 s_cmd53Err;
static uint32 s_cmd53Arg;
static uint32 s_cmd53Len;
static uint32 s_cmd53Status;   /* SDHOST_STATUS en el instante del fallo      */
static uint32 s_cmd53Ctype;    /* SDHOST_CTYPE: el ancho REAL del slot probesado */

/* Resultado de la ultima pasada, para que el envoltorio decida si hay que
 * reintentar en otro ancho de bus: `s_passAnnounced` es que el C6 SI declaro
 * un paquete (INT_RAW new_packet o PACKET_LEN distinto de 0) y
 * `s_passFifoOk` es que el CMD53 de la cola trajo los bytes.  Anunciado sin
 * bytes = el camino de comandos funciona y el de datos no, que es el patron
 * de un ancho de bus mal puesto (CMD52 jamas usa las lineas DAT). */
static uint   s_passAnnounced;
static uint   s_passFifoOk;

/* Puesta a punto del reloj de la pasada: con s_bypassSlotDiv el slot 1 se
 *枚举 con el divisor interno del slot EN SALTO (card_div=0) y la velocidad la
 * pone el divisor global del bloque, que es la unica combinacion con la que el
 * driver de referencia mueve datos.  Con s_bypassSlotDiv=0 se usa la escalera
 * vieja (divisor por-slot de trabajo / de sondeo). */
static uint   s_bypassSlotDiv;

/* Rebote para la unica lectura CMD53 de la sonda (la cola FIFO).  Estatica
 * para no pedir 512 bytes de pila a la tarea de la consola. */
static uint8  s_frame[SD_BLOCK_SIZE];

/* ------------------------------------------------------------------ */
/* Volcado del enrutado real de las patas del C6                        */
/* ------------------------------------------------------------------ */

/* Direcciones crudas: IO_MUX por pata (0x500E1000 + 4 + 4*pin) y matriz GPIO
 * (IN_SEL por senal en 0x500E0000+0x158+4*s, OUT_SEL por pata en
 * 0x500E0000+0x558+4*pin).  Se leen EN EL INSTANTE, no de ningun cache: si
 * alguna otra ruta reescribio la matriz, aqui se ve. */
#define FN1_GP_PAD(pin)     (0x500E1000UL + 4UL + 4UL * (pin))
#define FN1_GP_OUT(pin)     (0x500E0000UL + 0x558UL + 4UL * (pin))
#define FN1_GP_IN(sig)      (0x500E0000UL + 0x158UL + 4UL * (sig))

static void fn1DumpRouting(void)
{
    static const uint8 pins[6] = {14U, 15U, 16U, 17U, 18U, 19U};
    uint32 mux;
    uint32 out;
    uint32 in;
    uint32 k;

    kprintf("  ruta:");
    for (k = 0U; k < 6U; k++)
    {
        mux = *(volatile uint32 *)FN1_GP_PAD(pins[k]);
        out = *(volatile uint32 *)FN1_GP_OUT(pins[k]);
        kprintf(" p%u[mux=%u ie=%u pu=%u out=%u oe=%u]",
                (unsigned)pins[k], (unsigned)((mux >> 12) & 7U),
                (unsigned)((mux >> 9) & 1U), (unsigned)((mux >> 8) & 1U),
                (unsigned)(out & 0x1FFU), (unsigned)((out >> 10) & 1U));
    }
    kprintf("\r\n  ruta in:");
    for (k = 0U; k <= 5U; k++)
    {
        in = *(volatile uint32 *)FN1_GP_IN(k);
        kprintf(" s%u[%u%s]", (unsigned)k, (unsigned)(in & 0x3FU),
                (in & 0x80U) ? "m" : "-");
    }
    kprintf("\r\n");
}

/* GPIO_IN_REG y GPIO_ENABLE_W1TC del bloque GPIO (0x500E0000).  Se usan
 * crudos para no arrastrar la cabecera de gpio.h a este fichero. */
#define FN1_GP_IN_REG       (0x500E0000UL + 0x3CUL)
#define FN1_GP_EN_W1TC      (0x500E0000UL + 0x28UL)

/**
 * Diagnostico: averiguar QUIEN sostiene DAT0 a nivel bajo.
 *
 * Medido en placa: durante la fase de datos de un CMD53 de LECTURA la pata D0
 * se lee a 0 y el motor marca EBE (bit de fin erroneo).  Hay dos culpables
 * posibles y no se distinguen mirando solo el nivel: la tarjeta, que baja
 * DAT0 como "ocupada", o el propio host, que lo este conduciendo por tener el
 * habilitador de salida activo.
 *
 * Para separarlos se le quita al periferico el gobierno del habilitador:
 * OUT_SEL_CFG[14].OEN_SEL=1 hace que la pata tome el habilitador de
 * GPIO_ENABLE_REG[14], que se deja a 0.  O sea, la pata pasa a ser SOLO
 * entrada sin tocar el enrutado de la senal.  Si con el host fuera de la
 * linea el nivel sube, lo conducia el host; si sigue a 0, lo baja la tarjeta.
 * El enrutado se restaura antes de volver.
 */
static void fn1ProbeDat0(const char *tag)
{
    volatile uint32 *out    = (volatile uint32 *)FN1_GP_OUT(14U);
    volatile uint32 *in     = (volatile uint32 *)FN1_GP_IN_REG;
    volatile uint32 *en_w1c = (volatile uint32 *)FN1_GP_EN_W1TC;
    uint32 out_saved;
    uint32 lvl_host;
    uint32 lvl_free;
    uint32 busy_host;
    uint32 busy_free;

    lvl_host  = (*in >> 14) & 1U;
    busy_host = (SD_REG(SDHOST_STATUS) >> 9) & 1U;

    out_saved = *out;
    *en_w1c   = (1UL << 14);             /* GPIO_ENABLE[14] = 0              */
    *out      = out_saved | (1UL << 10); /* OEN_SEL = 1 -> manda GPIO_ENABLE */
    delay_ms(1);

    lvl_free  = (*in >> 14) & 1U;
    busy_free = (SD_REG(SDHOST_STATUS) >> 9) & 1U;

    *out = out_saved;                    /* OEN_SEL = 0 -> manda el periferico */
    delay_ms(1);

    kprintf("  dat0 %s: con host=%u (busy=%u)  host fuera=%u (busy=%u)  "
            "=> %s\r\n", tag, (unsigned)lvl_host, (unsigned)busy_host,
            (unsigned)lvl_free, (unsigned)busy_free,
            (lvl_free != 0U) ? "lo conducia el HOST" : "lo baja la TARJETA");
}

/**
 * Una sola fase de datos: mover exactamente \p len (1..512) bytes con la
 * funcion \p fn en modo byte (op_code=1, block_mode=0) desde/hacia \p addr.
 * El argumento CMD53:
 * [31]=write [30:28]=fn [27]=block_mode [26]=op_code [25:9]=addr [8:0]=count.
 * En modo byte un count de 0 significa 512.
 */
static bool fn1Cmd53Chunk(uint8 fn, uint32 addr, bool write, uint8 *in,
                          uint8 *out, uint32 len)
{
    uint32 count = len & 0x1FFU;
    uint32 arg;

    if (len == 0 || len > SD_BLOCK_SIZE)
    {
        return FALSE;
    }
    if (len == SD_BLOCK_SIZE)
    {
        count = 0U;
    }

    arg = ((uint32)write << 31) | ((uint32)fn << 28) | (1U << 26) /* op_code */
          | ((addr & 0x1FFFFUL) << 9) | (count & 0x1FFU);

    if (write)
    {
        sdBlockCopyIn(in, len);
    }
    if (!sdTransferSlot(SDIO_SLOT, SDIO_CMD53, arg, len, len, write))
    {
        /* Guardar el sd_hwerr del PRIMER fallo CMD53: el fallback CMD52 lo
         * pisa a 0 al triunfar, y sin esto la consola solo diria "fallback"
         * sin explicar donde se rompió la fase de datos (bit alto de sd_hwerr:
         * 0x1 inhibit, 0x2 error, 0x3 timeout, 0x4 DMA; bajo = RINTSTS/IDSTS). */
        if (s_cmd53ErrSeen == 0U)
        {
            s_cmd53ErrSeen = 1U;
            s_cmd53Err     = sd_hwerr;
            s_cmd53Arg     = arg;
            s_cmd53Len     = len;
            s_cmd53Status  = SD_REG(SDHOST_STATUS);
            s_cmd53Ctype   = SD_REG(SDHOST_CTYPE);
        }
        return FALSE;
    }
    if (!write)
    {
        sdBlockCopyOut(out, len);
    }
    return TRUE;
}

/**
 * Mover \p len (1..512) bytes por CMD53 respetando el quirk del host.
 *
 * El controlador no mueve de una vez un bloque de 4 o mas bytes cuyo tamano
 * no sea multiplo de 4: al escribir BYTCNT=26 se lee 28 y el CMD53 se queda
 * sin responder (ni CMD_DONE ni RTO).  ESP-IDF lo resuelve partiendo la
 * transferencia en la parte alineada y los 1..3 bytes que sobran
 * (sdmmc_io.c, "host quirk: SDIO transfer with length not divisible by 4
 * bytes has to be split into two transfers"), y eso es lo que se hace aqui.
 * Para 26 B son 24 + 2: el bloque grande y luego la cola corta.
 */
static bool fn1Cmd53Bytes(uint8 fn, uint32 addr, bool write, uint8 *data,
                          uint32 len)
{
    uint32 done = 0U;

    if (len == 0 || len > SD_BLOCK_SIZE)
    {
        return FALSE;
    }

    if (len >= 4U)
    {
        uint32 aligned = len & ~3U;

        if (!fn1Cmd53Chunk(fn, addr, write, data, data, aligned))
        {
            return FALSE;
        }
        done = aligned;
    }

    if (done < len)
    {
        if (!fn1Cmd53Chunk(fn, addr + done, write, data + done, data + done,
                           len - done))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Registros de 32 bits de la funcion 1 (CMD52 byte a byte)             */
/* ------------------------------------------------------------------ */

/* Metodo por el que respondio el ULTIMO acceso de registro.  Los registros de
 * 4 bytes se leen/escriben por CMD52 (cuatro pasadas de un byte): es el camino
 * que el slave esp_hosted responde siempre, sin fase de datos.  Queda 0 si el
 * acceso falla.  CMD53 se ejercita aparte, y solo, en la lectura explicita de
 * la cola FIFO (una trama real de len bytes), que es donde de verdad hace
 * falta mover datos. */
static uint s_lastRegMethod;

/** Leer un registro de 4 bytes little-endian de la funcion 1 (CMD52). */
static bool fn1ReadReg(uint32 off, uint32 *out)
{
    uint8 b[4];
    uint8 v;
    uint32 i;

    off &= FN_ADDR_MASK;

    for (i = 0; i < 4; i++)
    {
        v = 0;
        if (!fn1Cmd52Byte(SDIO_FN1, off + i, FALSE, &v))
        {
            s_lastRegMethod = 0U;
            return FALSE;
        }
        b[i] = v;
    }
    s_lastRegMethod = 2U;
    *out = (uint32)b[0] | ((uint32)b[1] << 8)
           | ((uint32)b[2] << 16) | ((uint32)b[3] << 24);
    return TRUE;
}

/** Escribir un registro de 4 bytes little-endian en la funcion 1 (CMD52). */
static bool fn1WriteReg(uint32 off, uint32 val)
{
    uint8 b[4];
    uint8 v;
    uint32 i;

    off &= FN_ADDR_MASK;
    b[0] = (uint8)(val);        b[1] = (uint8)(val >> 8);
    b[2] = (uint8)(val >> 16);  b[3] = (uint8)(val >> 24);

    for (i = 0; i < 4; i++)
    {
        v = b[i];
        if (!fn1Cmd52Byte(SDIO_FN1, off + i, TRUE, &v))
        {
            s_lastRegMethod = 0U;
            return FALSE;
        }
    }
    s_lastRegMethod = 2U;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Estado del camino, leido del propio periférico                       */
/* ------------------------------------------------------------------ */

/**
 * Imprimir STATUS/CLKENA/CLKDIV/CLKSRC descifrado (sdBusState, en sdHw.c).
 * Lo que decide el orden de la sonda es data_busy: con DAT0 afirmado el
 * comando interno "update clock" queda inhibido, y si ademas el reloj del slot
 * esta parado, el busy no se muestrea jamas.
 */
static void fn1BusState(const char *tag)
{
    sdBusState(tag, SDIO_SLOT);
}

/* ------------------------------------------------------------------ */
/* Puesta a punto CCCR del slave antes del primer CMD53                 */
/* ------------------------------------------------------------------ */

/**
 * Replicar, sobre la funcion 0 (CCCR) por CMD52, los pasos que el driver de
 * referencia hace entre fn_ready y su primer CMD53:
 *   - fijar el ancho de bus (\p want4 ? 4-bit : 1-bit; CCCR 0x07 codigo 2 o 0)
 *     y espejarlo en el bit de slot 1 de CTYPE del host;
 *   - fijar el tamano de bloque de fn0 y fn1 a 512 (CCCR 0x10/0x11, 0x110/0x111).
 *
 * El driver de referencia admite ambos anchos (config->bus_width vale 1 o 4),
 * asi que la sonda puede probarlos uno detras de otro: lo que NO se puede es
 * suponer 4-bit sin comprobarlo, porque los unicos accesos que han funcionado
 * hasta ahora (CMD5/CMD3/CMD7 y todos los CMD52) viajan por CMD, nunca por las
 * lineas DAT.  Una lectura CMD53 que muera con start-bit-error (RINTSTS bit13)
 * y end-bit-error (bit15) es exactamente el patron de "el host espera 4 lineas
 * y el esclavo no las esta conduciendo".
 *
 * La interrupcion SDIO (maestro + fn1) se arma en fn1ArmFn1Interrupt(), DESPUES
 * de la subida de reloj.  Si se arma antes, el C6 tiene un paquete de arranque
 * en su cola y afirma DAT0 (STATUS bit 9 data_busy) en cuanto se habilita; ese
 * estado inhibe el comando interno "update clock" de sdSetClockSlot, y con el
 * reloj del slot ya parado el busy jamas se drena (el deadlock que provoco el
 * barrido viejo).  El driver de referencia hace exactamente este orden: bus
 * width -> block size -> subida de reloj -> habilitar la interrupcion de la
 * funcion 1.
 *
 * En modo 4-bit la interrupcion de funcion viaja por DAT1; DAT0 queda libre
 * como linea de datos pura, pero el slave puede seguir afirmandolo como
 * "function busy" mientras procesa el paquete pendiente.  Por eso el reloj se
 * sube con la interrupcion aun desarmada.
 *
 * Imprime los valores releidos del propio CCCR para que sea prueba real.
 */
static void fn1SdioSetup(bool want4)
{
    uint8 bw, lo, hi, lo1, hi1;
    uint8 blkLo = (uint8)(FN1_BLOCK_SIZE & 0xFFU);   /* 512 -> 0x00 */
    uint8 blkHi = (uint8)(FN1_BLOCK_SIZE >> 8);      /* 512 -> 0x02 */
    volatile uint32 *reg = sdHostRegBase();
    uint  fails = 0U;

    /* Cada CMD52 se comprueba: si alguno falla el slave se queda como estaba y
     * el resto de la sonda (subida de reloj incluida) parte de un premise
     * falso.  Antes se ignoraban todos los resultados. */
#define FN1_SETUP(op, addr)                                                 \
    do {                                                                    \
        if (!(op)) {                                                        \
            fails++;                                                        \
            kprintf("  setup: CMD52 addr=0x%03x FALLO (hwerr=0x%08x)\r\n",  \
                    (uint32)(addr), sd_hwerr);                              \
        }                                                                   \
    } while (0)

    /* 1. Ancho de bus.  Primero se lo pide al slave por CMD52 (que todavia
     *    viaja en 1-bit: la respuesta R5 va por CMD, no por DAT) y despues se
     *    espeja en el bit de slot 1 de CTYPE del host.  Codigo 2 = 4-bit,
     *    codigo 0 = 1-bit. */
    bw = 0;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BUS_WIDTH, FALSE, &bw), CCCR_BUS_WIDTH);
    bw = (uint8)((bw & (uint8)~0x3U)
                 | (want4 ? CCCR_WIDTH_4BIT : 0U));
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BUS_WIDTH, TRUE, &bw), CCCR_BUS_WIDTH);
    if (want4)
    {
        reg[SDHOST_CTYPE / 4] |= HOST_CTYPE_W4_SLOT1;
    }
    else
    {
        reg[SDHOST_CTYPE / 4] &= ~HOST_CTYPE_W4_SLOT1;
    }
    bw = 0;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BUS_WIDTH, FALSE, &bw), CCCR_BUS_WIDTH);

    /* 2. Tamano de bloque fn0 = 512 (bajo 0x00, alto 0x02). */
    lo = blkLo; hi = blkHi;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN0_L, TRUE, &lo), CCCR_BLKSIZE_FN0_L);
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN0_H, TRUE, &hi), CCCR_BLKSIZE_FN0_H);

    /* 3. Tamano de bloque fn1 = 512 (CCCR 0x110/0x111, accedido como fn0). */
    lo1 = blkLo; hi1 = blkHi;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN1_L, TRUE, &lo1), CCCR_BLKSIZE_FN1_L);
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN1_H, TRUE, &hi1), CCCR_BLKSIZE_FN1_H);

    /* Relectura de los dos tamanos de bloque, para imprimir valores del
     * periférico y no los que se quisieron escribir. */
    lo = 0; hi = 0;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN0_L, FALSE, &lo), CCCR_BLKSIZE_FN0_L);
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN0_H, FALSE, &hi), CCCR_BLKSIZE_FN0_H);
    lo1 = 0; hi1 = 0;
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN1_L, FALSE, &lo1), CCCR_BLKSIZE_FN1_L);
    FN1_SETUP(fn1Cmd52Byte(SDIO_FN0, CCCR_BLKSIZE_FN1_H, FALSE, &hi1), CCCR_BLKSIZE_FN1_H);
#undef FN1_SETUP

    kprintf("fn1: setup CCCR (leido del slave): bus=0x%02x (pedido %s)  "
            "blksize fn0=0x%02x%02x  fn1=0x%02x%02x  CTYPE1=0x%08x  "
            "clk=%u kHz  fallos=%u  (int aun desarmada)\r\n",
            bw, want4 ? "4-bit" : "1-bit",
            hi, lo, hi1, lo1, reg[SDHOST_CTYPE / 4],
            (unsigned)sd_clk_khz, fails);
}

/**
 * Armar la interrupcion SDIO del slave (maestro + fn1) DESPUES de la subida
 * de reloj.  Desde este punto el C6 puede afirmar DAT1 (int errumpcion) o DAT0
 * (busy) cuando haya paquete nuevo; ya no se toca el reloj del slot, asi que
 * un busy a estas alturas no bloquea el update-clock (que solo se ejecuto en
 * sdSetClockSlot y ya termino).
 */
static void fn1ArmFn1Interrupt(void)
{
    uint8 ie;

    ie = 0;
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &ie);
    ie |= (uint8)(CCCR_INT_MASTER | CCCR_INT_FN1);
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, TRUE, &ie);
    ie = 0;
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &ie);
    kprintf("fn1: interrupcion armada  CCCR_INT_ENABLE=0x%02x (maestro|fn1)\r\n",
            ie);
}

/* ------------------------------------------------------------------ */
/* Vaciado de la cola C2H del slave                                    */
/* ------------------------------------------------------------------ */

/**
 * Consumir el paquete que el slave tenga en su cola ANTES de tocar el reloj
 * del slot 1.
 *
 * Este es el paso que faltaba y el que explica el enquistamiento del
 * update-clock medido en placa.  El firmware esp_hosted del C6 manda su
 * "init event" nada mas arrancar -- sin que el host le pida nada -- y mientras
 * ese paquete no se le lea mantiene DAT0 a nivel bajo (es su forma de decir
 * "funcion ocupada").  Con data_busy=1 el comando interno "update clock"
 * queda inhibido, y sdSetClockSlot baja CLKENA justo antes de lanzarlo: el
 * reloj deja de muestrear la linea, el busy jamas se drena y CMD_START no se
 * auto-limpia (motivo 3 en sd_clkdbg, STATUS=0x0001a306).  La unica salida
 * es consumir el paquete por CMD53, que es exactamente lo que hace
 * esp_hosted_sdio_transport_poll() del driver de referencia antes de
 * cualquier otro movimiento del enlace.
 *
 * Escribe en *pktLen el contador acumulado leido y devuelve los bytes
 * realmente leidos de la cola (0 si no habia nada accesible).
 */
static uint32 fn1DrainQueue(uint32 *pktLen)
{
    uint32 len = 0, raw = 0, addr, i, n;

    *pktLen = 0U;
    if (!fn1ReadReg(FN1_PACKET_LEN, &len))
    {
        fn1BusState("drain: PACKET_LEN no respondo");
        return 0U;
    }
    len &= SLAVE_LEN_MASK;
    *pktLen = len;
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    kprintf("  drain: PACKET_LEN(acumulado)=%u  INT_RAW=0x%08x "
            "new_packet=%u\r\n", len, raw,
            (raw & INT_NEW_PACKET) ? 1U : 0U);

    if (len == 0U || len > SD_BLOCK_SIZE)
    {
        return 0U;
    }

    addr = FN1_FIFO_END - len;
    memset(s_frame, 0, sizeof(s_frame));
    if (!fn1Cmd53Bytes(SDIO_FN1, addr, FALSE, s_frame, len))
    {
        fn1BusState("drain: CMD53 de la cola fallo");
        return 0U;
    }

    n = len < 48U ? len : 48U;
    kprintf("  drain: cola leida por CMD53 addr=0x%05x len=%u (primeros %u):",
            addr, len, n);
    for (i = 0; i < n; i++)
    {
        kprintf(" %02x", s_frame[i]);
    }
    kprintf("\r\n");

    /* Cabecera esp_hosted de 12 bytes: byte0 = if_type:4|if_num:4, byte1 =
     * flags (bit0 = mas fragmentos), len u16LE@2, offset u16LE@4 (=12),
     * checksum u16LE@6, seq u16LE@8. */
    if (len >= 12U)
    {
        uint32 sum = 0;

        for (i = 0; i < len; i++)
        {
            sum += s_frame[i];
        }
        kprintf("  drain: cabecera if_type=%u if_num=%u flags=0x%02x "
                "payload_len=%u offset=%u seq=%u checksum=0x%04x "
                "(bytesum=0x%04x)\r\n",
                (unsigned)(s_frame[0] >> 4), (unsigned)(s_frame[0] & 0xFU),
                (unsigned)s_frame[1],
                (uint32)s_frame[2] | ((uint32)s_frame[3] << 8),
                (uint32)s_frame[4] | ((uint32)s_frame[5] << 8),
                (uint32)s_frame[8] | ((uint32)s_frame[9] << 8),
                (uint16)((uint32)s_frame[6] | ((uint32)s_frame[7] << 8)),
                (uint16)(sum & 0xFFFFU));
    }
    return len;
}

/**
 * Acknowledge de las interrupciones crudas del slave ANTES de tocar el reloj.
 *
 * El driver de referencia hace el ack reescribiendo sobre INT_CLR (0x0D4) el
 * valor leido de INT_RAW (0x050); aqui se hace exactamente lo mismo, pero una
 * sola vez y al principio, porque un bit de interrupcion pendiente en el C6
 * mantiene la senal in-band afirmada y ese es el estado que vemos como
 * data_busy=1 en STATUS desde el CMD5.  Con data_busy=1 el comando interno
 * "update clock" NO se completa ni siquiera con wait_complete=0 (medido:
 * CMD_START clavado, RINTSTS=0, CLKDIV1 ya escrito), y al no completarse
 * enquista el motor de comandos -- que es compartido con el slot 0, o sea que
 * a partir de ahi la microSD tambien deja de responder.
 *
 * El ack es puro CMD52 (viaja por CMD, no por DAT), asi que funciona con el
 * esclavo ocupado.  Devuelve 1 si tras el ack el bus queda en reposo.
 */
static int fn1AckPendingInterrupts(void)
{
    uint32 raw = 0, back = 0;

    if (!fn1ReadReg(FN1_INT_RAW, &raw))
    {
        kprintf("fn1: ack: INT_RAW no respondo (hwerr=0x%08x)\r\n", sd_hwerr);
        return 0;
    }
    kprintf("fn1: ack: INT_RAW=0x%08x -> INT_CLR (CMD52)\r\n", raw);
    if (raw != 0U)
    {
        (void)fn1WriteReg(FN1_INT_CLR, raw);
    }
    (void)fn1ReadReg(FN1_INT_RAW, &back);
    kprintf("fn1: ack: INT_RAW tras el ack=0x%08x\r\n", back);
    fn1BusState("tras ack");
    return (SD_REG(SDHOST_STATUS) & SD_STATUS_DATA_BUSY) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Sonda del transporte: lee los registros de la funcion 1 en crudo     */
/* ------------------------------------------------------------------ */

/**
 * Levantar la funcion 1 (reutilizando la enumeracion W1) y ejercitar el camino
 * de datos en las dos direcciones, imprimiendo los valores LEIDOS del slave:
 *   0a. vaciar la cola C2H del slave (init event de arranque) para que suelte
 *       DAT0; sin eso el update-clock del paso 0b queda inhibido.
 *   1. PACKET_LEN antes del nudge (esperado 0: el C6 aun no produce).
 *   2. nudge HOST_TO_SLAVE (CMD52, BIT(0)) para abrirle el camino de datos.
 *   3. sondeo de INT_RAW/PACKET_LEN hasta que el C6 declare un paquete nuevo.
 *   4. lectura CMD53 de 4 bytes de INT_RAW, PACKET_LEN y TOKEN_RDATA.
 *   5. acknowledge: reescribir INT_RAW sobre INT_CLR por CMD53 (multi-byte) y
 *      volver a leer INT_RAW (esperado 0) -- prueba CMD53 de ESCRITURA.
 * Devuelve OK si la funcion 1 respondio a los accesos, SYSERR si no.
 * \p want4 pide el ancho de bus de la pasada (CCCR 0x07 + CTYPE del slot 1).
 */
static int fn1ProbeOnce(uint want4)
{
    struct sdioinfo info;
    uint32 lenBefore = 0, lenNow = 0, intRaw = 0, token = 0;
    uint32 intRawAfter = 0;
    uint32 fifoAddr = 0, fifoLen = 0, fifoKhz = 0;
    uint32 drained = 0, pktLen = 0;
    uint8  nudge;
    uint   k, tries = 30;
    uint   enumTry;
    uint   mRead = 0, fifoOk = 0;
    uint   failDiv = 0, failKhz = 0, anyFail = 0;
    uint   clkUp = 0, quiet = 0;
    bool   up = FALSE;

    /* Reiniciar el diagnostico CMD53 de esta pasada. */
    s_cmd53ErrSeen = 0U;
    s_cmd53Err     = 0U;
    s_cmd53Arg     = 0U;
    s_cmd53Len     = 0U;
    s_cmd53Status  = 0U;
    s_cmd53Ctype   = 0U;

    if (!sd_host_ready)
    {
        kprintf("fn1: el host SDMMC no esta levantado (sd_host_ready=0)\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    /* El reloj del slot 1 solo se puede cambiar EN LA VENTANA DE REPOSO: nada
     * mas pulsar el reset del C6 y antes del CMD5, porque es el CMD5 el que
     * deja DAT0 afirmado (data_busy=1) y con ese bit arriba el "update clock"
     * no se completa y enqista el motor compartido con la microSD.  Por eso la
     * subida se hace DENTRO de la enumeracion (sdioEnumerateAt), como hace el
     * driver de referencia, que fija el reloj de trabajo en el attach y nunca
     * lo mueve en caliente.  Los intentos 0 y 1 piden ya el divisor de
     * trabajo; si el C6 no enumera a 20 MHz, el intento 2 vuelve al de
     * sondeo y ahi la subida queda prohibida (se omite en el paso 0b). */
    for (enumTry = 0; enumTry < 3 && !up; enumTry++)
    {
        uint8 div = (s_bypassSlotDiv != 0U)
                        ? 0U                                /* salto: reloj = global */
                        : ((enumTry < 2U) ? SD_CARD_DIV_WORK : SD_CARD_DIV_PROBE);

        if (sdioEnumerateAt(&info, div, TRUE) == OK && info.up)
        {
            up    = TRUE;
            clkUp = (div == SD_CARD_DIV_PROBE) ? 0U : 1U;
            break;
        }
        delay_ms(200);
    }
    if (!up)
    {
        sdmmcBusUnlock();
        kprintf("fn1: fallo la enumeracion previa (hwerr=0x%08x)\r\n", sd_hwerr);
        return SYSERR;
    }

    /* 0. Puesta a punto CCCR (ancho de bus + tamano de bloque) que el slave
     *    necesita antes de aceptar un CMD53; cada CMD52 se comprueba y se
     *    cuenta, porque un fallo aqui deja el premise de la sonda roto. */
    fn1SdioSetup(want4 != 0U);
    delay_ms(2);
    fn1BusState("tras setup CCCR");

    /* 0a. VACIAR LA COLA C2H ANTES DE TOCAR EL RELOJ (ver fn1DrainQueue): el
     *     init event de arranque es justo el paquete que mantiene DAT0 abajo
     *     y enquistaba el update-clock.  Tras leerlo, el bus debe quedar en
     *     reposo (data_busy=0) y la subida de reloj ya es segura. */
    drained = fn1DrainQueue(&pktLen);
    fn1BusState("tras vaciar la cola");

    /* 0a-b. Ack de las interrupciones crudas pendientes: es lo que suelta la
     *     senal in-band que vemos como data_busy. */
    quiet = (uint)fn1AckPendingInterrupts();

    /* 0b. RELOJ DE TRABAJO DEL SLOT 1.  Ya fue puesto dentro de la
     *     enumeracion (sdioEnumerateAt con SD_CARD_DIV_WORK), que es la unica
     *     ventana sin data_busy.  Aqui solo se comprueba que el CMD52 de
     *     registros sigue respondiendo a esa velocidad.
     *     Si la enumeracion cay6 al divisor de sondeo (el C6 no contesto a 20
     *     MHz), NO se intenta subir en caliente: con data_busy=1 el "update
     *     clock" no se completa y el CMD_START clavado enqista el motor de
     *     comandos del BLOQUE, compartido con la microSD -- medido: tras ese
     *     intento el 'ls' devolvia el directorio vacio.  La sonda corre then a
     *     400 kHz y deja constancia. */
    {
        bool   busyBefore = (SD_REG(SDHOST_STATUS) & SD_STATUS_DATA_BUSY) != 0;
        bool   atWork     = (clkUp != 0U);
        uint32 chk        = 0;

        kprintf("fn1: 0b reloj del slot=%u kHz (div=%u) puesto en %s  "
                "data_busy=%u quiet=%u\r\n", (unsigned)sd_clk_khz,
                (unsigned)((sd_clk_khz >= 10000U) ? (s_bypassSlotDiv != 0U ? 0U
                                                   : SD_CARD_DIV_WORK)
                                                  : SD_CARD_DIV_PROBE),
                atWork ? "la enumeracion" : "la enumeracion (sondeo)",
                busyBefore ? 1U : 0U, quiet);
        fn1BusState("0b");

        if (!atWork)
        {
            kprintf("     subida omitida: con data_busy=%u el update-clock "
                    "enquistaria el motor compartido con la microSD\r\n",
                    busyBefore ? 1U : 0U);
        }
        else if (!fn1ReadReg(FN1_PACKET_LEN, &chk))
        {
            /* El CMD52 enquiso a 20 MHz: un enlace a 4-bit por la matriz GPIO
             * puede no aguantar la velocidad.  Unica recuperacion posible:
             * volver a enumerar al divisor de sondeo (eso pulsa el reset del
             * C6 y suelta DAT0, y el update-clock vuelve a ser legal). */
            anyFail = 1U;
            failDiv = SD_CARD_DIV_WORK;
            failKhz = (uint32)sd_clk_khz;
            kprintf("fn1: 0b CMD52 PACKET_LEN enquiso a %u kHz "
                    "(hwerr=0x%08x) -- reenumerar\r\n",
                    failKhz, sd_hwerr);
            /* En modo "referencia" la reenumeracion vuelve a pedir SALTO (el
             * reloj lo pone el global, que el envoltorio cambiara si hace
             * falta); en modo viejo se baja al divisor de sondeo. */
            if (sdioEnumerateAt(&info,
                    (s_bypassSlotDiv != 0U) ? 0U : SD_CARD_DIV_PROBE,
                    FALSE) == OK && info.up)
            {
                clkUp = 0U;
                atWork = FALSE;
                kprintf("     reenumerado: clk=%u kHz\r\n",
                        (unsigned)sd_clk_khz);
            }
        }
        else
        {
            kprintf("fn1: 0b OK, CMD52 responde a %u kHz (PACKET_LEN=0x%08x)\r\n",
                    (unsigned)sd_clk_khz, chk);
        }
    }

    /* 0c. Armar la interrupcion SDIO (maestro + fn1).  Se hace AQUI, con el
     *     reloj ya en su sitio, y no dentro de fn1SdioSetup: si se arma antes
     *     de la subida, el C6 afirma DAT0 en cuanto tiene paquete y el
     *     update-clock de sdSetClockSlot queda inhibido (con el reloj parado
     *     por el propio update, el busy no se drena -> deadlock). */
    fn1ArmFn1Interrupt();

    /* 1. Longitud C2H antes de abrir el camino. */
    if (!fn1ReadReg(FN1_PACKET_LEN, &lenBefore))
    {
        sdmmcBusUnlock();
        kprintf("fn1: PACKET_LEN(0x060) no respondo (hwerr=0x%08x)\r\n",
                sd_hwerr);
        kprintf("     clkUp=%u clk=%u kHz CMD=0x%08x STATUS=0x%08x "
                "RINTSTS=0x%08x CLKENA=0x%08x\r\n",
                clkUp, (unsigned)sd_clk_khz,
                SD_REG(SDHOST_CMD), SD_REG(SDHOST_STATUS),
                SD_REG(SDHOST_RINTSTS), SD_REG(SDHOST_CLKENA));
        return SYSERR;
    }
    mRead = s_lastRegMethod;            /* como se leyo este registro */

    /* 2. Nudge: BIT(0) al registro HOST_TO_SLAVE por CMD52 (1 byte, como el
     *    write_reg_bytes de len=1 del driver de referencia). */
    nudge = (uint8)(1U << HOST_INTR_OPEN_DATA);
    if (!fn1Cmd52Byte(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, &nudge))
    {
        sdmmcBusUnlock();
        kprintf("fn1: nudge HOST_TO_SLAVE(0x08C) fallo (hwerr=0x%08x)\r\n",
                sd_hwerr);
        return SYSERR;
    }

    /* 3. Sondear hasta que el C6 ponga un paquete en su cola. */
    for (k = 0; k < tries; k++)
    {
        delay_ms(1);
        if (!fn1ReadReg(FN1_INT_RAW, &intRaw))
        {
            break;
        }
        (void)fn1ReadReg(FN1_PACKET_LEN, &lenNow);
        if (lenNow != 0 || (intRaw & INT_NEW_PACKET) != 0)
        {
            break;
        }
    }
    (void)fn1ReadReg(FN1_INT_RAW, &intRaw);
    (void)fn1ReadReg(FN1_PACKET_LEN, &lenNow);
    (void)fn1ReadReg(FN1_TOKEN_RDATA, &token);

    /* 4. LECTURA DE LA COLA FIFO (CMD53).  El reloj del slot 1 ya esta en el de
     *     trabajo (subido en reposo en el paso 0b); aqui NO se toca el reloj,
     *     solo se leen los len bytes de la cola en 0x1F800-len.  Que el esclavo
     *     afirme DAT0 ahora es normal y deseado: es justo la fase de datos del
     *     CMD53, y el reloj ya corre, asi que no hay riesgo de enquistar el
     *     "update clock" (eso solo pasaba al cambiar de reloj en caliente).
     *     Si el reloj de trabajo no se pudo subir (clkUp=0), el CMD53 seguira a
     *     400 kHz y, como ya se vio, el slave no conducira los bytes: se imprime
     *     igualmente para dejar constancia. */
    fifoLen = lenNow & SLAVE_LEN_MASK;
    fifoKhz = (uint32)sd_clk_khz;
    if (fifoLen != 0 && fifoLen <= SD_BLOCK_SIZE)
    {
        memset(s_frame, 0, sizeof(s_frame));
        fifoAddr = FN1_FIFO_END - fifoLen;
        s_cmd53ErrSeen = 0U;
        if (fn1Cmd53Bytes(SDIO_FN1, fifoAddr, FALSE, s_frame, fifoLen))
        {
            fifoOk = 1U;
        }
    }

    /* Dejar constancia del resultado para el envoltorio (reintento en 1-bit). */
    s_passFifoOk   = fifoOk;
    s_passAnnounced = (lenNow != 0U || (intRaw & INT_NEW_PACKET) != 0U) ? 1U : 0U;

    /* 5. Acknowledge de la interrupcion: reescribir INT_RAW sobre INT_CLR por
     *    CMD52 y volver a leer INT_RAW (esperado sin el bit new_packet). */
    if (intRaw != 0)
    {
        (void)fn1WriteReg(FN1_INT_CLR, intRaw);
        (void)fn1ReadReg(FN1_INT_RAW, &intRawAfter);
    }

    sdmmcBusUnlock();

    /* Presentacion: mapa de registros y valores leidos del periférico. */
    kprintf("fn1: mapa de la funcion 1 (offsets tras &0x3FF):\r\n");
    kprintf("  TOKEN=0x044  INT_RAW=0x050  PACKET_LEN=0x060  "
            "H2S_INTR=0x08C  INT_CLR=0x0D4  FIFO_END=0x%05x\r\n", FN1_FIFO_END);
    /* El ancho se lee del CTYPE del bloque (bit por slot), NO de
     * sd_host_width: esa variable es la de la microSD (slot 0) y mentiria en
     * la pasada de 1-bit del slot 1. */
    kprintf("  SDHOST base=0x%08x  clk tarjeta=%u kHz  CTYPE=0x%08x "
            "slot1=%u-bit\r\n",
            (uint32)(uintptr_t)sdHostRegBase(), sd_clk_khz,
            (uint32)(SD_REG(SDHOST_CTYPE) & 0x3U),
            (SD_REG(SDHOST_CTYPE) & HOST_CTYPE_W4_SLOT1) ? 4U : 1U);

    kprintf("fn1: PACKET_LEN antes del nudge        = 0x%08x (%u)\r\n",
            lenBefore & SLAVE_LEN_MASK, lenBefore & SLAVE_LEN_MASK);
    kprintf("fn1: cola C2H de arranque (vaciada en 0a) = %u bytes, "
            "PACKET_LEN leido = %u\r\n", drained, pktLen);
    kprintf("fn1: INT_RAW tras el nudge             = 0x%08x "
            "(new_packet=%u throttle=%u/%u)\r\n",
            intRaw, (intRaw & INT_NEW_PACKET) ? 1U : 0U,
            (intRaw & INT_START_THROTTLE) ? 1U : 0U,
            (intRaw & INT_STOP_THROTTLE) ? 1U : 0U);
    kprintf("fn1: PACKET_LEN tras el nudge          = 0x%08x (%u bytes)\r\n",
            lenNow & SLAVE_LEN_MASK, lenNow & SLAVE_LEN_MASK);
    kprintf("fn1: TOKEN_RDATA (consumidos H2C)      = 0x%08x (token=%u)\r\n",
            token, (token >> 16) & TX_BUF_MASK);
    kprintf("fn1: INT_RAW tras ack (CMD52 write)    = 0x%08x\r\n", intRawAfter);
    kprintf("fn1: metodo lectura registros = %s\r\n",
            (mRead == 1U) ? "CMD53" : (mRead == 2U) ? "CMD52"
                                                   : "FALLO");

    /* Prueba de la fase de datos: la cola FIFO leida por CMD53 al reloj que
     * funciono.  Si el barrido encontro un divisor donde el CMD52 enquiso, se
     * avisa (se revirtio a 400 kHz, el enlace queda usable). */
    if (anyFail != 0U)
    {
        kprintf("fn1: el CMD52 se enquiso al subir slot1 a div=%u (~%u kHz); "
                "revertido a sondeo\r\n", (unsigned)failDiv, failKhz);
    }
    kprintf("fn1: CMD53 FIFO tail addr=0x%05x len=%u clk=%u kHz -> %s\r\n",
            fifoAddr, fifoLen, fifoKhz, fifoOk ? "OK" : "FALLO");
    if (fifoOk != 0U)
    {
        uint32 hlen = fifoLen;
        uint32 hlenF, hseq, hoff, hcksumCalc;
        uint32 j;

        kprintf("  trama:");
        for (j = 0; j < hlen && j < 40U; j++)
        {
            kprintf(" %02x", s_frame[j]);
        }
        kprintf("%s\r\n", hlen > 40U ? " ..." : "");

        /* Decodificar la cabecera de 12 bytes de esp_hosted si cabe entera:
         * byte0 = if_type:4|if_num:4, byte1 = flags (bit0 MORE_FRAGMENT),
         * len u16LE@2, offset u16LE@4 (=12), checksum u16LE@6 (suma de bytes
         * de la trama), seq u16LE@8. */
        if (hlen >= 12U)
        {
            hlenF     = (uint32)s_frame[2] | ((uint32)s_frame[3] << 8);
            hoff      = (uint32)s_frame[4] | ((uint32)s_frame[5] << 8);
            hcksumCalc = 0;
            for (j = 0; j < hlen; j++)
            {
                hcksumCalc += s_frame[j];   /* byte-sum mod 2^32, se compara en 16 */
            }
            hseq      = (uint32)s_frame[8] | ((uint32)s_frame[9] << 8);
            kprintf("  cabecera: if_type=%u if_num=%u flags=0x%02x "
                    "payload_len=%u offset=%u seq=%u checksum=0x%04x "
                    "(bytesum=0x%04x)\r\n",
                    (unsigned)(s_frame[0] >> 4), (unsigned)(s_frame[0] & 0xFU),
                    (unsigned)s_frame[1], hlenF, hoff, hseq,
                    (uint16)((uint32)s_frame[6] | ((uint32)s_frame[7] << 8)),
                    (uint16)(hcksumCalc & 0xFFFFU));
        }
    }

    /* Donde se rompió la fase de datos CMD53 (bit alto de sd_hwerr = fase,
     * bit bajo = RINTSTS/IDSTS del momento del fallo). */
    if (s_cmd53ErrSeen != 0U)
    {
        const char *fase = (s_cmd53Err & 0xF0000000UL) == SD_HWERR_INHIBIT
                               ? "inhibit (CMD_START no despejo)"
                           : (s_cmd53Err & 0xF0000000UL) == SD_HWERR_ERR
                               ? "error de comando/dato"
                           : (s_cmd53Err & 0xF0000000UL) == SD_HWERR_TIMEOUT
                               ? "timeout (silencio del slave)"
                           : (s_cmd53Err & 0xF0000000UL) == SD_HWERR_DMA
                               ? "DMA/IDSTS dio up"
                               : "otra";
        kprintf("fn1: CMD53 fallo en %s: sd_hwerr=0x%08x "
                "arg=0x%08x len=%u status=0x%08x ctype=0x%08x\r\n",
                fase, s_cmd53Err, s_cmd53Arg, s_cmd53Len,
                s_cmd53Status, s_cmd53Ctype);
    }

    /* PRUEBA DE DIRECCION INVERSA.  El CMD53 de lectura mueve el dato ESCLAVO
     * -> host; un CMD53 de ESCRITURA mueve host -> esclavo por las MISMAS patas
     * DAT0..DAT3 (y el esclavo lo confirma con el CRC de su respuesta al final
     * del bloque).  Es el experimento que separa las dos explicaciones que
     * quedan: si la escritura pasa y la lectura no, el cable y el motor
     * IDMAC/FIFO estan bien y lo que falla es que el C6 no conduce la linea
     * cuando se le pide dato --es decir, lo que el esclavo espera para poner un
     * paquete en el FIFO, no la electrica.  Si la escritura falla igual, el
     * camino de datos del slot 1 esta muerto en los dos sentidos.
     *
     * Se hace DESPUES del diagnostico de la lectura para no pisarlo, y despues
     * de un sdBlockReset porque la lectura muerta deja data_state_mc_busy
     * clavado: con ese bit arriba la limpieza previa de sdTransferSlot
     * abortaria la escritura sin siquiera lanzarla. */
    {
        uint8 wbuf[4];
        uint32 back   = 0;
        bool   wOk;

        wbuf[0] = (uint8)(1U << HOST_INTR_OPEN_DATA);
        wbuf[1] = wbuf[2] = wbuf[3] = 0U;
        (void)sdBlockReset();
        s_cmd53ErrSeen = 0U;
        wOk = fn1Cmd53Bytes(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, wbuf, 4U);
        kprintf("fn1: CMD53 ESCRITURA 4B al doorbell 0x%03x -> %s "
                "(hwerr=0x%08x arg=0x%08x ctype=0x%08x)\r\n",
                (unsigned)FN1_HOST_TO_SLAVE, wOk ? "OK" : "FALLO",
                s_cmd53ErrSeen ? s_cmd53Err : sd_hwerr,
                s_cmd53ErrSeen ? s_cmd53Arg : 0U,
                (uint32)SD_REG(SDHOST_CTYPE));
        s_cmd53ErrSeen = 0U;
        if (fn1ReadReg(FN1_HOST_TO_SLAVE, &back))
        {
            kprintf("fn1: doorbell leido tras la escritura = 0x%08x\r\n", back);
        }
    }

    if (lenNow != 0 || intRaw != 0 || drained != 0)
    {
        kprintf("fn1: TRANSPORTE OK -- el C6 respondio a CMD53/52 y su cola "
                "C2H tiene datos.\r\n");
        return OK;
    }

    kprintf("fn1: sin datos (hwerr=0x%08x) -- el acceso a la funcion 1 no "
            "produjo paquetes; revisar reloj/ancho o el firmware del C6.\r\n",
            sd_hwerr);
    return SYSERR;
}

/* ------------------------------------------------------------------ */
/* Devolver el bloque compartido en estado usable                       */
/* ------------------------------------------------------------------ */

/* sdIdentify() vive en device/sdblock-esp32p4/sdCard.h, cabecera privada de
 * esa capa; se declara aqui en vez de exponerla, porque solo la usa esta sonda
 * para volver a presentar la microSD despues de tocar el bloque. */
extern bool sdIdentify(void);
extern const char *sd_fail;      /* en que paso paro la identificacion        */

/**
 * Dejar el SDHOST como lo dejo el arranque.  El bloque es UNO y los dos slots
 * comparten motor de comandos, IDMA y FIFO, asi que un CMD53 del C6 que muere
 * a media fase de datos (o un "update clock" enquistado) puede dejar el motor
 * ocupado y entonces la microSD no responde: `ls` sale vacio.  Esta rutina es
 * la misma secuencia del arranque --pads, reset de controlador e identificacion
 * de tarjeta-- y se comprueba con una lectura REAL del LBA 0, buscando la firma
 * 0x55AA del registro de arranque.  No depende de la FAT montada: si el bloque
 * responde, la firma esta.  COSTE: el reset del controlador tambien despeja al
 * C6, asi que esta llamada va SIEMPRE AL FINAL, cuando la sonda ya no necesita
 * el slot 1 (la siguiente pasada vuelve a enumerar desde un reset del slave).
 */
static void fn1RestoreSlot0(void)
{
    static uint8 mbr[SD_BLOCK_SIZE];
    bool ctrl, ident, read;

    /* Primero: retener al C6 en reset.  Con el slave retenido sus pads del slot 1
     * dejan de conducir, asi que si el que inhibia el camino de datos era el DAT0
     * del C6, aqui desaparece. */
    sdioC6HoldReset();

    /* Despues: reset de protocolo del bloque entero.  sdControllerInit solo
     * resetea si los bits de CTRL vienen alzados, y despues de un fallo vienen
     * bajados --por eso no limpiaba la maquina de estados de datos clavada--. */
    sdBlockReset();
    fn1BusState("tras sdBlockReset");

    /* Devolver el reloj GLOBAL del bloque al de la microSD: la sonda lo bajo
     * para probar la fase de datos con card_div=0.  El divisor global esta
     * aguas arriba del latch por-slot, asi que con solo pulsar el update de
     * HP_SYS_CLKRST el slot 0 recupera sus 20 MHz sin necesidad de un "update
     * clock" del CIU. */
    if (!sdSetHostDiv(SD_HOST_DIV))
    {
        kprintf("fn1: NO se pudo devolver el divisor global del SDMMC\r\n");
    }

    sdPadsInit();
    ctrl   = sdControllerInit();
    ident  = ctrl ? sdIdentify() : FALSE;
    read   = FALSE;

    if (ident)
    {
        sdmmcBusLock();
        read = sdReadBlock(0U);
        if (read)
        {
            sdBlockCopyOut(mbr, sizeof(mbr));
        }
        sdmmcBusUnlock();
    }

    kprintf("fn1: dejar el bloque como en el arranque: ctrl=%u "
            "ident=%u lectura LBA0=%u firma=0x%02x%02x -> %s\r\n",
            ctrl, ident, read, mbr[510], mbr[511],
            (read && mbr[510] == 0x55U && mbr[511] == 0xAAU)
                ? "microSD VIVA" : "microSD MUERTA");
    if (!ident)
    {
        kprintf("     identificacion parada en '%s', hwerr=0x%08x; "
                "estado del bus:\r\n", sd_fail, sd_hwerr);
        sdBusState("restore", 0U);
    }
}

/* ------------------------------------------------------------------ */
/* Entrada de la sonda                                                  */
/* ------------------------------------------------------------------ */

/**
 * Pasadas de la sonda.  Dos ejes se prueban, y por eso son dos bucles:
 *
 *   - el RELOJ (fuera): dos divisores globales con el divisor interno del slot
 *     EN SALTO, que es la unica combinacion con la que el driver de referencia
 *     mueve una fase de datos (22,857 MHz).  Si el dato aparece a 40 MHz pero
 *     no a 10 MHz es un problema de muestreo; si no aparece en ninguno de los
 *     dos, no es el reloj.
 *   - el ANCHO (dentro): 4-bit y, si el C6 anuncia paquete pero el CMD53 no
 *     saca ni un byte, otra pasada completa en 1-bit --el mismo driver admite
 *     bus_width=1 y el reset del slave de la segunda enumeracion suelta DAT0,
 *     que es lo que deja legal volver a poner el reloj--.
 *
 * El criterio de exito de cada pasada es laxo (le basta con que el C6 haya
 * respondido a los accesos), asi que la decision del reintento NO mira r: se
 * reintenta si hubo paquete ANUNCIADO y, sin embargo, el CMD53 no saco bytes.
 * Al terminar, SIEMPRE se devuelve el bloque compartido en estado usable para
 * la microSD (fn1RestoreSlot0 restaura tambien el divisor global).
 */
int sdioFn1TransportProbe(void)
{
    static const uint32_t hostDiv[] = { SD_HOST_DIV_REF, SD_HOST_DIV_SLOW };
    int    r = SYSERR;
    uint   k;

    for (k = 0U; k < sizeof(hostDiv) / sizeof(hostDiv[0]) && s_passFifoOk == 0U;
         k++)
    {
        if (!sdSetHostDiv(hostDiv[k]))
        {
            kprintf("fn1: divisor global %u rechazado por el CLKRST\r\n",
                    (unsigned)hostDiv[k]);
            continue;
        }
        s_bypassSlotDiv = 1U;

        /* Un CMD53 muerto deja data_state_mc_busy clavado, y con ese bit arriba
         * el "update clock" queda inhibido para los DOS slots: hay que resetear
         * el bloque antes de la siguiente pasada o el reloj nuevo no se aplica. */
        kprintf("fn1: ==== pasada con reloj coherente: global=%u -> %u kHz "
                "(slot en salto), reset del bloque=%u ====\r\n",
                (unsigned)hostDiv[k], (unsigned)sd_host_khz,
                (unsigned)sdBlockReset());
        r = fn1ProbeOnce(1U);

        if (s_passAnnounced != 0U && s_passFifoOk == 0U)
        {
            kprintf("fn1: anuncio sin bytes en 4-bit: reintento la pasada "
                    "entera en 1-bit, tras reset del bloque =%u\r\n",
                    (unsigned)sdBlockReset());
            fn1BusState("tras reset de bloque");
            r = fn1ProbeOnce(0U);
        }

        if (s_passFifoOk != 0U)
        {
            kprintf("fn1: el CMD53 movio bytes con el reloj coherente del "
                    "reference (%u kHz) -- queda fijado ese regimen\r\n",
                    (unsigned)sd_host_khz);
            break;
        }
    }

    s_bypassSlotDiv = 0U;
    fn1RestoreSlot0();
    return r;
}

/* ------------------------------------------------------------------ */
/* Diagnostico dirigido de la fase de datos (`wifi dma`)               */
/* ------------------------------------------------------------------ */

/**
 * Probar el CMD53 del slot 1 pieza por pieza, a 400 kHz y con la interrupcion
 * SDIO apagada.
 *
 * Punto de partida medido: `wifi fn1` demuestra que el C6 enumera, que todos
 * los CMD52 responden y que el slave anuncia un paquete de 26 bytes, pero la
 * lectura CMD53 de ese paquete muere con SBE|EBE (bit de arranque del bloque
 * mal) sin mover ni un byte.  Eso deja dos familias de causa:
 *
 *   (a) el slave no conduce las lineas DAT cuando se le pide dato, o
 *   (b) el host pide el dato de una forma que el slave no acepta.
 *
 * Cuatro pruebas separan ambas:
 *
 *   1. CMD53 de 4 bytes a TOKEN (0x044): la transferencia mas pequena y
 *      alineada que existe.  Si esta pasa, el camino de datos funciona y lo
 *      que falla es la direccion o la ventana de la cola.
 *   2. La cola con la longitud exacta anunciada (26): el caso real.
 *   3. La cola con la longitud REDONDEADA a multiplo de 4 (28): el driver de
 *      ESP-IDF redondea el tamano del buffer del DMA a multiplo de 4 antes de
 *      armar el descriptor; si el IDMAC del P4 lo exige, aqui se vera.
 *   4. Escritura de 4 bytes al doorbell: el sentido contrario.
 *
 * Se corre a 400 kHz (divisor de sondeo) para descartar el muestreo, y con
 * CCCR_INT_ENABLE=0: con el maestro armado y un paquete pendiente el C6 afirma
 * DAT1, y en 4 bits DAT1 es una linea de DATOS (el bit de arranque del bloque
 * exige DAT1..3=1), de modo que una interrupcion viva puede por si sola
 * romper el arranque del bloque.  Los dos anchos se prueban seguidos porque
 * el CMD52 nunca usa las lineas DAT: un fallo solo en 4 bits senala al ancho.
 *
 * Termina SIEMPRE con fn1RestoreSlot0(), que devuelve el bloque a la microSD.
 */
int sdioDmaDiag(void)
{
    struct sdioinfo info;
    uint8  v;
    uint8  nudge;
    uint32 pktLen;
    uint32 intRaw;
    uint32 lenNow;
    uint32 token;
    uint32 len;
    uint32 lenAl;
    uint32 i;
    bool   ok;
    uint   pass;

    if (!sd_host_ready)
    {
        kprintf("dma: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    /* Plan de medidas.  La variable que se mueve es CMD[13] (wait_complete):
     * la hipotesis es que con el C6 afirmando DAT0 (funcion ocupada) ese bit
     * deja el CMD53 cargado pero sin enviar nunca.  Se prueba primero sin el
     * bit (el candidato a arreglo), luego con el bit (la linea base que ya
     * fallo) y por ultimo en 1-bit sin el bit, para ver si es del ancho o del
     * camino de datos comun.  Todo a 400 kHz, donde el muestreo no puede
     * ser la excusa. */
    /* Plan de medidas.  Cada pasada re-enumera (el CMD53 fallido deja al C6
     * afirmando DAT0 y sordo a los CMD52; el CMD0 con SEND_INIT lo devuelve a
     * un estado conocido) y mueve UNA sola variable respecto a la anterior:
     *
     *   1. reloj de trabajo, 4 bits, interrupcion armada: es exactamente la
     *      configuracion de `wifi fn1`, donde el CMD53 SI obtiene respuesta.
     *      Aqui la traza dice que hace el motor y que conducen las patas.
     *   2. lo mismo en 1 bit, para separar un problema de ancho.
     *   3. reloj de sondeo (400 kHz): en la pasada anterior el CMD53 no obtuvo
     *      ni respuesta; se repite aqui para confirmar que es el reloj y no otra
     *      cosa la que cambia el comportamiento del esclavo.
     */
    static const struct
    {
        uint8 div;
        bool  raise;
        uint8 w4;
        uint8 inton;
        const char *tag;
    } plan[3] =
    {
        { SD_CARD_DIV_WORK,  TRUE,  1U, 1U, "trabajo 4-bit int armada" },
        { SD_CARD_DIV_WORK,  TRUE,  0U, 1U, "trabajo 1-bit int armada" },
        { SD_CARD_DIV_PROBE, FALSE, 1U, 0U, "sondeo 4-bit int apagada" },
    };

    if (!sd_host_ready)
    {
        kprintf("dma: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    for (pass = 0U; pass < 3U; pass++)
    {
        bool w4 = (plan[pass].w4 != 0U);

        sd_xfer_no_wait = 0U;

        /* El reloj se fija DENTRO de la enumeracion (ventana de reposo, antes
         * del CMD5): fuera de ella el update-clock no se completa. */
        if (sdioEnumerateAt(&info, plan[pass].div, plan[pass].raise) != OK
            || !info.up)
        {
            kprintf("dma: [%s] el C6 no enumero (hwerr=0x%08x)\r\n",
                    plan[pass].tag, sd_hwerr);
            continue;
        }
        kprintf("dma: ==== pasada %s  RCA=0x%04x  %u kHz ====\r\n",
                plan[pass].tag, (unsigned)info.rca, (unsigned)info.clk_khz);

        fn1SdioSetup(w4);
        delay_ms(2);

        /* La interrupcion SDIO se deja como en `wifi fn1` (armada) o se apaga,
         * segun la pasada: con el maestro armado y un paquete pendiente el C6
         * afirma DAT1, y en 4 bits DAT1 es una linea de DATOS. */
        if (plan[pass].inton)
        {
            fn1ArmFn1Interrupt();
        }
        else
        {
            v = 0;
            (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, TRUE, &v);
        }
        v = 0xFF;
        (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &v);
        kprintf("dma: CCCR_INT_ENABLE leido = 0x%02x\r\n", v);
        fn1BusState(plan[pass].tag);

        /* Una cola C2H de la pasada anterior no sirve como prueba: se vacia
         * (mejor esfuerzo) y se pide un paquete nuevo con el nudge.  NO se hace
         * un reset de bloque aqui: el CMD52 del drain y el del nudge van por el
         * camino de comandos, que queda clavado mientras la interrupcion del
         * esclavo este sin ackear (ver fn1AckPendingInterrupts). */
        pktLen = 0;
        (void)fn1DrainQueue(&pktLen);

        nudge = (uint8)(1U << HOST_INTR_OPEN_DATA);
        ok = fn1Cmd52Byte(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, &nudge);
        if (!ok)
        {
            kprintf("dma: nudge HOST_TO_SLAVE fallo (hwerr=0x%08x)\r\n",
                    sd_hwerr);
        }
        intRaw = 0;
        lenNow = 0;
        for (i = 0; i < 30U; i++)
        {
            delay_ms(1);
            (void)fn1ReadReg(FN1_INT_RAW, &intRaw);
            (void)fn1ReadReg(FN1_PACKET_LEN, &lenNow);
            if (lenNow != 0U || (intRaw & INT_NEW_PACKET) != 0U)
            {
                break;
            }
        }
        (void)fn1ReadReg(FN1_TOKEN_RDATA, &token);
        len = lenNow & SLAVE_LEN_MASK;
        kprintf("dma: cola C2H: PACKET_LEN=%u  INT_RAW=0x%08x  "
                "TOKEN=0x%08x\r\n", len, intRaw, token);

        /* Ack de la interrupcion justo como hace la sonda (0a/0b): reescribir
         * INT_RAW sobre INT_CLR y volver a leerlo.  Despues del nudge el bit
         * new_packet esta en marcha; sin este ack el esclavo queda sordo. */
        (void)fn1AckPendingInterrupts();

        fn1DumpRouting();
        fn1ProbeDat0("en reposo");

        /* 0. Contraprueba del camino de datos con un valor CONOCIDO.
         *
         * El CCCR de la funcion 0 (direccion 0x00) son constantes que la
         * tarjeta no puede cambiar.  Se leen por CMD52 (comando puro, sin
         * fase de datos) y por CMD53 (con fase de datos).  Si los dos caminos
         * coinciden, el motor transporta lo que el slave pone en el bus y el
         * problema estaria en la direccion/contenido de la cola; si el CMD53
         * devuelve ceros, lo que falla es el transporte tarjeta->host. */
        {
            uint8 c52[4];
            uint8 k;

            for (k = 0U; k < 4U; k++)
            {
                c52[k] = 0U;
                (void)fn1Cmd52Byte(SDIO_FN0, k, FALSE, &c52[k]);
            }
            memset(s_frame, 0, 4U);
            sd_xfer_trace = 1U;
            ok = fn1Cmd53Bytes(SDIO_FN0, 0x000U, FALSE, s_frame, 4U);
            sd_xfer_trace = 0U;
            kprintf("dma: [%u-bit] CCCR fn0 CMD52=%02x %02x %02x %02x  "
                    "CMD53=%s %02x %02x %02x %02x\r\n", w4 ? 4U : 1U,
                    c52[0], c52[1], c52[2], c52[3], ok ? "OK" : "FALLO",
                    s_frame[0], s_frame[1], s_frame[2], s_frame[3]);
            (void)sdBlockReset();
        }

        /* 1. La cola, con su longitud EXACTA.
         *
         * fn1Cmd53Bytes ya aplica el quirk del host: un bloque de 4 o mas
         * bytes que no sea multiplo de 4 se parte en la parte alineada y los
         * 1..3 bytes que sobran.  Para los 26 B de este evento son 24 + 2, y
         * asi el paquete se lee en su sitio (FIFO_END - len) sin relleno de
         * por medio.  Es ademas el primer movimiento de datos, que es el que
         * vacia la cola y devuelve DAT0 a reposo. */
        if (len != 0U && len <= SD_BLOCK_SIZE)
        {
            memset(s_frame, 0, sizeof(s_frame));
            sd_xfer_trace = 1U;
            ok = fn1Cmd53Bytes(SDIO_FN1, FN1_FIFO_END - len, FALSE,
                               s_frame, len);
            sd_xfer_trace = 0U;
            kprintf("dma: [%u-bit] CMD53 LEER %uB cola @0x%05x -> %s "
                    "(hwerr=0x%08x)\r\n", w4 ? 4U : 1U, len,
                    (unsigned)(FN1_FIFO_END - len), ok ? "OK" : "FALLO",
                    sd_hwerr);
            if (ok)
            {
                kprintf("     bytes:");
                for (i = 0; i < len && i < 40U; i++)
                {
                    kprintf(" %02x", s_frame[i]);
                }
                kprintf("%s\r\n", len > 40U ? " ..." : "");
            }
            (void)sdBlockReset();

            /* 2. Contraprueba: la misma cola pedida de UNA vez en 28 B.
             * Debe fallar: el host no mueve ese bloque no alineado. */
            lenAl = (len + 3U) & ~3U;
            if (lenAl != len && lenAl <= SD_BLOCK_SIZE)
            {
                memset(s_frame, 0, sizeof(s_frame));
                sd_xfer_trace = 1U;
                ok = fn1Cmd53Bytes(SDIO_FN1, FN1_FIFO_END - lenAl, FALSE,
                                   s_frame, lenAl);
                sd_xfer_trace = 0U;
                kprintf("dma: [%u-bit] CMD53 LEER %uB (redondeada) cola "
                        "@0x%05x -> %s (hwerr=0x%08x)\r\n",
                        w4 ? 4U : 1U, lenAl, (unsigned)(FN1_FIFO_END - lenAl),
                        ok ? "OK" : "FALLO", sd_hwerr);
                (void)sdBlockReset();
            }
        }
        else
        {
            kprintf("dma: [%u-bit] sin cola que leer (len=%u)\r\n",
                    w4 ? 4U : 1U, len);
        }

        /* 3. CMD53 de 4 bytes a un registro, ya con el FIFO vacio: sirve de
         * referencia del camino corto (4 B, byte a byte) sin el paquete de por
         * medio. */
        memset(s_frame, 0, 4U);
        sd_xfer_trace = 1U;
        ok = fn1Cmd53Bytes(SDIO_FN1, FN1_TOKEN_RDATA, FALSE, s_frame, 4U);
        sd_xfer_trace = 0U;
        kprintf("dma: [%u-bit] CMD53 LEER 4B @0x%03x -> %s (hwerr=0x%08x) "
                "%02x %02x %02x %02x\r\n",
                w4 ? 4U : 1U, (unsigned)FN1_TOKEN_RDATA, ok ? "OK" : "FALLO",
                sd_hwerr, s_frame[0], s_frame[1], s_frame[2], s_frame[3]);
        (void)sdBlockReset();

        /* 4. Escritura de 4 bytes al doorbell (sentido host -> slave). */
        {
            uint8 w[4];

            w[0] = (uint8)(1U << HOST_INTR_OPEN_DATA);
            w[1] = w[2] = w[3] = 0U;
            sd_xfer_trace = 1U;
            ok = fn1Cmd53Bytes(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, w, 4U);
            sd_xfer_trace = 0U;
            kprintf("dma: [%u-bit] CMD53 ESCRIBIR 4B @0x%03x -> %s "
                    "(hwerr=0x%08x)\r\n", w4 ? 4U : 1U,
                    (unsigned)FN1_HOST_TO_SLAVE, ok ? "OK" : "FALLO", sd_hwerr);
            (void)sdBlockReset();
        }
    }

    /* Dejar el motor como estaba: sin traza y con wait_complete puesto. */
    sd_xfer_trace = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ------------------------------------------------------------------ */
/* Diagnostico en modo PIO: el CMD53 sin IDMAC (`wifi pio`)              */
/* ------------------------------------------------------------------ */

/**
 * Un paso de CMD53 en modo PIO: mismo argumento y mismo troceo en parte alineada
 * + cola suelta que fn1Cmd53Bytes(), pero el dato lo mueve el CPU palabra a
 * palabra por BUFFIFO (sdTransferPio), sin descriptor, sin DBADDR y sin la
 * rebote en PSRAM.
 *
 * \p len es la longitud REAL de este paso (1..512); el arg pone ese mismo
 * numero en el campo de bytes y BLKSIZ = BYTCNT = len, byte-mode.
 */
static bool fn1Cmd53PioOnce(uint8 fn, uint32 addr, bool write, uint8 *data,
                            uint32 len)
{
    uint32 count = len;
    uint32 arg;

    if (len == SD_BLOCK_SIZE)
    {
        count = 0U;                       /* 512 = codificado como 0 */
    }
    arg = ((uint32)write << 31) | ((uint32)fn << 28) | (1U << 26) /* op_code */
          | ((addr & 0x1FFFFUL) << 9) | (count & 0x1FFU);

    if (!sdTransferPio(SDIO_SLOT, SDIO_CMD53, arg, data, len, len, write))
    {
        if (s_cmd53ErrSeen == 0U)
        {
            s_cmd53ErrSeen = 1U;
            s_cmd53Err     = sd_hwerr;
            s_cmd53Arg     = arg;
            s_cmd53Len     = len;
            s_cmd53Status  = SD_REG(SDHOST_STATUS);
            s_cmd53Ctype   = SD_REG(SDHOST_CTYPE);
        }
        return FALSE;
    }
    return TRUE;
}

/** CMD53 en PIO con el troceo de alineacion de 4 bytes (como fn1Cmd53Bytes). */
static bool fn1Cmd53Pio(uint8 fn, uint32 addr, bool write, uint8 *data,
                        uint32 len)
{
    uint32 done = 0U;

    if (len == 0 || len > SD_BLOCK_SIZE)
    {
        return FALSE;
    }
    if (len >= 4U)
    {
        uint32 aligned = len & ~3U;

        if (!fn1Cmd53PioOnce(fn, addr, write, data, aligned))
        {
            return FALSE;
        }
        done = aligned;
    }
    if (done < len)
    {
        if (!fn1Cmd53PioOnce(fn, addr + done, write, data + done, len - done))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Separar el IDMAC de la capa fisica (`wifi pio`).
 *
 * Con el camino de comandos probado (todos los CMD52 responden) y el IDMAC del
 * slot 1 sin cruzar nunca un byte, quedan dos explicaciones y hasta ahora no se
 * pueden distinguir: (a) el esclavo no conduce las lineas DAT cuando se le pide
 * dato, o (b) el dato si llega al bloque y es el motor de descriptors el que no
 * lo saca de la FIFO a la memoria.
 *
 * Este modo apaga el IDMAC (CTRL[5] y CTRL[25] a 0, BMOD e IDINTEN a 0) y saca
 * las palabras por BUFFIFO a mano.  No hay descriptor, ni DBADDR, ni pagina en
 * PSRAM de por medio, asi que el resultado es vinculante:
 *
 *   - salen los valores que ya da el CMD52  -> (b): el transporte funciona y el
 *     culpable es el IDMAC/descriptor/memoria del slot 1;
 *   - salen ceros con palabras=0            -> (a): el bloque no vio ni un bit.
 *
 * Cada pasada re-enumera y prueba los cuatro casos de `wifi dma` (CCCR de fn0,
 * TOKEN de fn1, la cola C2H con su longitud exacta y el doorbell), primero en
 * 4-bit y luego en 1-bit, que es donde la traza del DMA bajo el OWN del
 * descriptor.  Termina SIEMPRE con fn1RestoreSlot0() para dejar la microSD viva.
 */
int sdioPioDiag(void)
{
    struct sdioinfo info;
    uint8  nudge;
    uint8  v;
    uint8  c52[4];
    uint8  w[4];
    uint32 intRaw;
    uint32 len;
    uint32 lenNow;
    uint32 pktLen;
    uint32 token52;
    uint32 tokenPio;
    uint32 i;
    bool   ok;
    uint   pass;
    static const uint8 w4plan[2] = { 1U, 0U };

    if (!sd_host_ready)
    {
        kprintf("pio: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    for (pass = 0U; pass < 2U; pass++)
    {
        bool w4 = (w4plan[pass] != 0U);

        sd_xfer_no_wait = 0U;

        /* El reloj se fija dentro de la enumeracion (ventana de reposo). */
        if (sdioEnumerateAt(&info, SD_CARD_DIV_WORK, TRUE) != OK || !info.up)
        {
            kprintf("pio: [%u-bit] el C6 no enumero (hwerr=0x%08x)\r\n",
                    w4 ? 4U : 1U, sd_hwerr);
            continue;
        }
        kprintf("pio: ==== pasada %u-bit  RCA=0x%04x  %u kHz ====\r\n",
                w4 ? 4U : 1U, (unsigned)info.rca, (unsigned)info.clk_khz);

        fn1SdioSetup(w4);
        delay_ms(2);
        fn1ArmFn1Interrupt();
        v = 0xFF;
        (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &v);
        kprintf("pio: CCCR_INT_ENABLE leido = 0x%02x  clk=%u kHz  "
                "CTYPE=0x%08x\r\n", v, (unsigned)sd_clk_khz,
                (uint32)SD_REG(SDHOST_CTYPE));

        /* Pedido de paquete nuevo, igual que en `wifi dma`, para que la cola
         * tenga algo que leer y el esclavo tenga una razon de conducir DAT. */
        pktLen = 0;
        (void)fn1DrainQueue(&pktLen);
        nudge = (uint8)(1U << HOST_INTR_OPEN_DATA);
        if (!fn1Cmd52Byte(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, &nudge))
        {
            kprintf("pio: nudge HOST_TO_SLAVE fallo (hwerr=0x%08x)\r\n",
                    sd_hwerr);
        }
        intRaw = 0;
        lenNow = 0;
        for (i = 0U; i < 30U; i++)
        {
            delay_ms(1);
            (void)fn1ReadReg(FN1_INT_RAW, &intRaw);
            (void)fn1ReadReg(FN1_PACKET_LEN, &lenNow);
            if (lenNow != 0U || (intRaw & INT_NEW_PACKET) != 0U)
            {
                break;
            }
        }
        (void)fn1ReadReg(FN1_TOKEN_RDATA, &token52);
        len = lenNow & SLAVE_LEN_MASK;
        kprintf("pio: cola C2H: PACKET_LEN=%u  INT_RAW=0x%08x  TOKEN=0x%08x\r\n",
                len, intRaw, token52);
        (void)fn1AckPendingInterrupts();
        fn1ProbeDat0("en reposo");

        /* A. El mismo CCCR de la funcion 0 por los dos caminos.  Son constantes
         *    que el slave no puede cambiar: si el PIO coincide con el CMD52, el
         *    camino de datos cruza. */
        for (i = 0U; i < 4U; i++)
        {
            c52[i] = 0U;
            (void)fn1Cmd52Byte(SDIO_FN0, i, FALSE, &c52[i]);
        }
        memset(s_frame, 0, 4U);
        s_cmd53ErrSeen = 0U;
        sd_xfer_trace = 1U;
        ok = fn1Cmd53Pio(SDIO_FN0, 0x000U, FALSE, s_frame, 4U);
        sd_xfer_trace = 0U;
        kprintf("pio: [%u-bit] CCCR fn0  CMD52=%02x %02x %02x %02x | "
                "PIO=%s %02x %02x %02x %02x  palabras=%u hwerr=0x%08x\r\n",
                w4 ? 4U : 1U, c52[0], c52[1], c52[2], c52[3],
                ok ? "OK" : "FALLO",
                s_frame[0], s_frame[1], s_frame[2], s_frame[3],
                (unsigned)sd_pio_words, sd_hwerr);
        (void)sdBlockReset();

        /* B. TOKEN de la funcion 1, mismo registro por los dos caminos. */
        token52 = 0;
        (void)fn1ReadReg(FN1_TOKEN_RDATA, &token52);
        memset(s_frame, 0, 4U);
        s_cmd53ErrSeen = 0U;
        sd_xfer_trace = 1U;
        ok = fn1Cmd53Pio(SDIO_FN1, FN1_TOKEN_RDATA, FALSE, s_frame, 4U);
        sd_xfer_trace = 0U;
        tokenPio = (uint32)s_frame[0] | ((uint32)s_frame[1] << 8)
                   | ((uint32)s_frame[2] << 16) | ((uint32)s_frame[3] << 24);
        kprintf("pio: [%u-bit] TOKEN    CMD52=0x%08x | PIO=%s 0x%08x  "
                "palabras=%u bytes=%u hwerr=0x%08x\r\n",
                w4 ? 4U : 1U, token52, ok ? "OK" : "FALLO", tokenPio,
                (unsigned)sd_pio_words, (unsigned)sd_pio_bytes, sd_hwerr);
        (void)sdBlockReset();

        /* C. La cola C2H real, leida por PIO con su longitud exacta. */
        if (len != 0U && len <= SD_BLOCK_SIZE)
        {
            memset(s_frame, 0, sizeof(s_frame));
            s_cmd53ErrSeen = 0U;
            sd_xfer_trace = 1U;
            ok = fn1Cmd53Pio(SDIO_FN1, FN1_FIFO_END - len, FALSE,
                             s_frame, len);
            sd_xfer_trace = 0U;
            kprintf("pio: [%u-bit] cola %uB @0x%05x -> %s  palabras=%u "
                    "bytes=%u hwerr=0x%08x\r\n", w4 ? 4U : 1U, len,
                    (unsigned)(FN1_FIFO_END - len), ok ? "OK" : "FALLO",
                    (unsigned)sd_pio_words, (unsigned)sd_pio_bytes, sd_hwerr);
            if (sd_pio_bytes != 0U)
            {
                kprintf("     bytes:");
                for (i = 0U; i < len && i < 40U; i++)
                {
                    kprintf(" %02x", s_frame[i]);
                }
                kprintf("%s\r\n", len > 40U ? " ..." : "");
            }
            (void)sdBlockReset();
        }
        else
        {
            kprintf("pio: [%u-bit] sin cola que leer (len=%u)\r\n",
                    w4 ? 4U : 1U, len);
        }

        /* D. Sentido contrario: 4 bytes al doorbell por PIO. */
        w[0] = (uint8)(1U << HOST_INTR_OPEN_DATA);
        w[1] = w[2] = w[3] = 0U;
        s_cmd53ErrSeen = 0U;
        sd_xfer_trace = 1U;
        ok = fn1Cmd53Pio(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, w, 4U);
        sd_xfer_trace = 0U;
        kprintf("pio: [%u-bit] ESCRITURA 4B doorbell @0x%03x -> %s  "
                "palabras=%u hwerr=0x%08x\r\n", w4 ? 4U : 1U,
                (unsigned)FN1_HOST_TO_SLAVE, ok ? "OK" : "FALLO",
                (unsigned)sd_pio_words, sd_hwerr);
        (void)sdBlockReset();
    }

    sd_xfer_trace = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* Barrido de reloj del CMD53 en PIO (`wifi slow`)                       */
/* ==================================================================== */

/**
 * El reloj que nunca se probo.  `wifi pio` y `wifi dma` solo corrieron a
 * 20 MHz (SD_CARD_DIV_WORK), y 20 MHz es justo la unica velocidad del barrido
 * donde dos cosas juegan en contra:
 *   - el esquematico (U1 ESP32-C6FH8, pines 26..29 del módulo -> R15..R18 51K a
 *     ESP_3V3 -> nets GPIO14..GPIO17 del P4) pone pull-ups de 51K en las cuatro
 *     lineas de datos: empujar y soltar un 0 por ahi con C_L de 15-20 pF da
 *     bordes de cientos de ns, o sea inviables a 50 ns de periodo;
 *   - el slot 1 no tiene pines IOMUX (soc/sdmmc_pins.h solo define slot 0), así
 *     que CLK/DAT suben y bajan por la matriz GPIO, que añade retardo extra en
 *     los dos sentidos y no se puede compensar con las fases drv/sam del
 *     divisor global.
 * En cambio CMD52 SI responde a 400 kHz (fase W1, enumerado con
 * SD_CARD_DIV_PROBE) --el reloj bajo esta probado para el C6, lo que nunca se
 * probo es su fase de datos.
 *
 * Por cada divisor: re-enumera a ESE reloj, arma la fn1, pide un paquete, y
 * compara el mismo registro visto por CMD52 y por PIO (4 constantes del CCCR de
 * fn0 y TOKEN_RDATA de fn1).  Si a algn divisor bajo el PIO coincide con el
 * CMD52, el culpable es el tiempo de sena y no el alambre ni el protocolo.
 * 1-bit: si el pinaje es el del esquemático, con DAT0 basta.
 * Termina SIEMPRE con fn1RestoreSlot0() para dejar la microSD viva.
 */
int sdioPioClkDiag(void)
{
    struct sdioinfo info;
    static const uint8 divs[] = { 50U, 25U, 12U, 6U, 3U, SD_CARD_DIV_WORK };
    uint32 token52 = 0, tokenPio, d, i;
    uint8  c52[4];
    bool   ok;

    if (!sd_host_ready)
    {
        kprintf("slow: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    for (d = 0U; d < sizeof(divs) / sizeof(divs[0]); d++)
    {
        sd_xfer_no_wait = 0U;
        sd_xfer_trace   = 0U;

        if (sdioEnumerateAt(&info, divs[d], FALSE) != OK || !info.up)
        {
            kprintf("slow: div=%u el C6 ni enumero (hwerr=0x%08x)\r\n",
                    (unsigned)divs[d], sd_hwerr);
            continue;
        }
        fn1SdioSetup(FALSE);
        delay_ms(2);
        fn1ArmFn1Interrupt();

        /* Referencia por CMD52 (viaja solo por CMD: nunca falla por DAT). */
        for (i = 0U; i < 4U; i++)
        {
            c52[i] = 0U;
            (void)fn1Cmd52Byte(SDIO_FN0, i, FALSE, &c52[i]);
        }
        (void)fn1ReadReg(FN1_TOKEN_RDATA, &token52);

        /* Mismos registros por la fase de datos, en PIO. */
        memset(s_frame, 0, 4U);
        s_cmd53ErrSeen = 0U;
        ok = fn1Cmd53Pio(SDIO_FN0, 0x000U, FALSE, s_frame, 4U);
        kprintf("slow: div=%-3u %6u kHz | CCCR CMD52=%02x %02x %02x %02x "
                "PIO=%s %02x %02x %02x %02x pal=%u hw=0x%08x\r\n",
                (unsigned)divs[d], (unsigned)info.clk_khz,
                c52[0], c52[1], c52[2], c52[3], ok ? "OK  " : "FAIL",
                s_frame[0], s_frame[1], s_frame[2], s_frame[3],
                (unsigned)sd_pio_words, sd_hwerr);
        (void)sdBlockReset();

        memset(s_frame, 0, 4U);
        s_cmd53ErrSeen = 0U;
        ok = fn1Cmd53Pio(SDIO_FN1, FN1_TOKEN_RDATA, FALSE, s_frame, 4U);
        tokenPio = (uint32)s_frame[0] | ((uint32)s_frame[1] << 8)
                   | ((uint32)s_frame[2] << 16) | ((uint32)s_frame[3] << 24);
        kprintf("slow: div=%-3u %6u kHz | TOKEN CMD52=0x%08x PIO=%s 0x%08x "
                "pal=%u bytes=%u hw=0x%08x\r\n",
                (unsigned)divs[d], (unsigned)info.clk_khz, token52,
                ok ? "OK  " : "FAIL", tokenPio, (unsigned)sd_pio_words,
                (unsigned)sd_pio_bytes, sd_hwerr);
        (void)sdBlockReset();
    }

    sd_xfer_trace   = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* Quien conduce las lineas de datos (`wifi bus`)                        */
/* ==================================================================== */

/* Medido por `wifi pio`: en modo PIO no llega ni una palabra por DAT, o sea el
 * fallo no esta en la IDMAC ni en las paginas de PSRAM, esta en el alambre.
 * Y fn1ProbeDat0() ya deja el pad del P4 fuera de la linea (OEN_SEL=1 con
 * GPIO_ENABLE=0: el pad queda tri-estado y solo manda lo que haya fuera) y aun
 * asi D0 se lee a 0 con el pull-up interno puesto => lo baja un conductor
 * EXTERNO.  Quedan dos explicaciones y hay que separarlas:
 *
 *   1. El C6 conduce DAT0 bajo: la interrupcion SDIO en banda del esclavo
 *      (los 26 bytes de PACKET_LEN que nadie drena) o un "busy" que no suelta.
 *   2. La placa: un pulldown fisico, un corto, o que el pad que creemos DAT0
 *      no sea el que esta cableado a DAT0.
 *
 * La palanca para separarlas es el reset del C6, y se comprueba ANTES de fiarse
 * de el: si con CHIP_PU abajo el C6 sigue contestando a un CMD52, GPIO54 NO es
 * su reset (o no llega) y la foto "C6 en reset" de `wifi pads` no probaba nada.
 * Con el reset validado, la foto con el pad tri-estado y pull-up dice la
 * verdad: D0 sube => lo bajaba el C6 (caso 1); D0 sigue a 0 con pull-up y
 * pull-down alternando => la placa (caso 2).
 */

#define FN1_GP_EN           (0x500E0000UL + 0x20UL)   /* GPIO_ENABLE[0..31]   */
#define FN1_GP_EN_W1TS      (0x500E0000UL + 0x24UL)   /* escribir 1 = activar */
#define FN1_PAD_WPD         (1UL << 7)
#define FN1_PAD_WPU         (1UL << 8)
#define FN1_PAD_IE          (1UL << 9)
#define FN1_OEN_SEL         (1UL << 10)               /* en OUT_SEL_CFG[pin]  */

/* Pads del slot 1, en el orden en que se fotografian. */
#define FN1_NBUSH           6U
static const uint8 s_busPins[FN1_NBUSH] = { 14U, 15U, 16U, 17U, 19U, 18U };
static const char  s_busNames[FN1_NBUSH][4] = { "D0", "D1", "D2", "D3",
                                                "CMD", "CLK" };

/* Estado de cada pad guardado antes de tocarlo, para dejarlo exactamente igual. */
static uint32 s_busPadSave[FN1_NBUSH];
static uint32 s_busOutSave[FN1_NBUSH];
static uint32 s_busEnSave[FN1_NBUSH];

/**
 * Dejar el pad \p idx como pura lectura: entrada habilitada, resistencia
 * interna elegida (\p pullup) y la conduccion del P4 APAGADA por la via dura
 * (OEN_SEL=1 para que mande GPIO_ENABLE, y ese bit a 0), que es exactamente
 * gpio_ll_output_disable() de la referencia.  El enrutado de la senal no se
 * toca: con el pad tri-estado, GPIO_IN solo puede reflejar al conductor
 * externo.
 */
static void fn1PadTriState(uint idx, bool pullup)
{
    volatile uint32 *pad = (volatile uint32 *)(uintptr_t)
                               FN1_GP_PAD(s_busPins[idx]);
    volatile uint32 *out = (volatile uint32 *)(uintptr_t)
                               FN1_GP_OUT(s_busPins[idx]);
    uint32 p;

    s_busPadSave[idx] = *pad;
    s_busOutSave[idx] = *out;
    s_busEnSave[idx]  = (*(volatile uint32 *)(uintptr_t)FN1_GP_EN
                         >> s_busPins[idx]) & 1U;

    p = s_busPadSave[idx] & ~(FN1_PAD_IE | FN1_PAD_WPU | FN1_PAD_WPD);
    p |= FN1_PAD_IE | (pullup ? FN1_PAD_WPU : FN1_PAD_WPD);
    *pad = p;

    /* Orden deliberado: primero apagar la conduccion (GPIO_ENABLE=0) y solo
     * despues cambiar la fuente de OE, para no conducir un nivel de mas. */
    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << s_busPins[idx];
    *out = s_busOutSave[idx] | FN1_OEN_SEL;
}

/** Devolver el pad \p idx al estado exacto en que se encontro. */
static void fn1PadRestore(uint idx)
{
    volatile uint32 *pad = (volatile uint32 *)(uintptr_t)
                               FN1_GP_PAD(s_busPins[idx]);
    volatile uint32 *out = (volatile uint32 *)(uintptr_t)
                               FN1_GP_OUT(s_busPins[idx]);

    *out = s_busOutSave[idx];
    *pad = s_busPadSave[idx];
    if (s_busEnSave[idx] != 0U)
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TS = 1UL << s_busPins[idx];
    }
    else
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << s_busPins[idx];
    }
}

/**
 * Foto de las seis patas con el P4 fuera de la linea, repetida con pull-up y
 * con pull-down internos.  Una linea flotante lee 1 con pull-up y 0 con
 * pull-down; una linea conducida por fuera lee lo mismo en las dos tomas.
 * Cada pad se restaura despues de cada foto, asi que el bus vuelve a estar
 * como lo dejo el motor.
 */
static void fn1BusPhoto(const char *tag)
{
    uint32 lvl[FN1_NBUSH];
    uint32 busy[FN1_NBUSH];
    uint   shot;
    uint   i;

    for (shot = 0U; shot < 2U; shot++)
    {
        for (i = 0U; i < FN1_NBUSH; i++)
        {
            fn1PadTriState(i, (shot == 0U) ? TRUE : FALSE);
        }
        delay_ms(1);
        for (i = 0U; i < FN1_NBUSH; i++)
        {
            lvl[i] = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG
                      >> s_busPins[i]) & 1U;
        }
        busy[shot] = (SD_REG(SDHOST_STATUS) >> 9) & 1U;
        for (i = 0U; i < FN1_NBUSH; i++)
        {
            fn1PadRestore(i);
        }

        kprintf("  bus %s (%s, P4 fuera):", tag,
                (shot == 0U) ? "pull-up" : "pull-down");
        for (i = 0U; i < FN1_NBUSH; i++)
        {
            kprintf(" %s=%u", s_busNames[i], (unsigned)lvl[i]);
        }
        kprintf("   busy=%u  EN=0x%08x\r\n", (unsigned)busy[shot],
                *(volatile uint32 *)(uintptr_t)FN1_GP_EN);
    }
}

int sdioBusOwnershipDiag(void)
{
    struct sdioinfo info;
    uint8  cc;
    bool   answers;

    if (!sd_host_ready)
    {
        kprintf("bus: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    /* 1. Levantar el C6 como en las demas sondas (CMD5/CMD3/CMD7 + CCCR) para
     *    tener el camino de comandos vivo y poder comparar antes/despues. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_WORK, FALSE) != OK || !info.up)
    {
        kprintf("bus: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    kprintf("bus: C6 arriba RCA=0x%04x %u kHz  STATUS=0x%08x\r\n",
            (unsigned)info.rca, (unsigned)info.clk_khz,
            (uint32)SD_REG(SDHOST_STATUS));
    fn1DumpRouting();

    /* 2. Foto con el C6 vivo y el P4 fuera de la linea: lo que se lee es lo
     *    que conduce el esclavo (o la placa). */
    fn1BusPhoto("C6 vivo");

    /* 3. Validar la palanca ANTES de usarla.  Retener el C6 en reset y
     *    preguntarle un CCCR por CMD52: un C6 en reset no puede contestar.  Si
     *    contesta, GPIO54 no manda sobre el y las fotos "retenido" no separan
     *    al C6 de la placa. */
    gpio_set_dir(C6_RESET, GPIO_OUTPUT);
    gpio_write(C6_RESET, FALSE);              /* CHIP_PU abajo                */
    delay_ms(200);

    cc = 0xA5U;
    answers = fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &cc);
    kprintf("bus: con CHIP_PU=0 el C6 %s CMD52 (CCCR_INT_ENABLE=0x%02x, "
            "hwerr=0x%08x)\r\n", answers ? "CONTESTA" : "no contesta",
            (unsigned)cc, sd_hwerr);
    if (answers)
    {
        kprintf("bus: OJO: GPIO54 no resetea al C6 -> la foto 'retenido' "
                "sigue incluyendo al C6\r\n");
    }

    /* 4. Foto cruzada: C6 retenido (si el reset es real) Y el P4 fuera. */
    fn1BusPhoto("C6 retenido");

    /* 5. Liberar el C6 y repetir: si D0 vuelve a 0 con el esclavo recien
     *    arrancado, lo baja el propio slave al levantar (interrupcion en banda
     *    con paquete pendiente), no un resto de la transferencia anterior. */
    gpio_write(C6_RESET, TRUE);
    delay_ms(1500);
    fn1BusPhoto("C6 recien liberado");

    /* 6. Reenumerar y mirar el bloque: si data_busy sigue clavado con el slave
     *    otra vez arriba, el host no puede ni armar el reloj de datos. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_WORK, FALSE) == OK && info.up)
    {
        cc = 0U;
        (void)fn1Cmd52Byte(SDIO_FN1, FN1_INT_RAW, FALSE, &cc);
        kprintf("bus: tras reenumerar: INT_RAW[7:0]=0x%02x STATUS=0x%08x\r\n",
                (unsigned)cc, (uint32)SD_REG(SDHOST_STATUS));
    }
    else
    {
        kprintf("bus: tras liberar el C6 no reenumero (hwerr=0x%08x)\r\n",
                sd_hwerr);
    }
    fn1BusPhoto("tras reenumerar");

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* Prueba causal de DAT0 (`wifi dat`)                                    */
/* ==================================================================== */

/* Firmas medidas, todas coherentes entre si:
 *   - `wifi slow`: el CMD53 de lectura se comporta IGUAL a 400 kHz que a
 *     20 MHz -- una palabra y EBE (RINTSTS bit 15, "end bit error") y los
 *     bytes salen 00 00 00 00 mientras el CMD52 del MISMO registro da
 *     32 02 02 02.  O sea que no es velocidad, ni la IDMAC, ni la direccion.
 *   - Ese patron es exactamente lo que hace una linea DAT0 clavada a 0 durante
 *     toda la fase: el bit de start es 0 (parece valido), cada bit de dato se
 *     lee 0, y el bit de fin --que debe valer 1-- se lee 0 => EBE.
 *   - `wifi bus`: con el pad del P4 tri-estado y pull-up interno, D0 se lee 0
 *     incluso con el C6 retenido (GPIO54 validado: ahi el C6 no contesta ni a
 *     CMD52).  Con los 51K de pull-up fisicos de la red (esquematico R15..R18),
 *     un 0 franco significa que la sostiene algo mucho mas fuerte que 51K.
 * Quedan tres candidatos y este diagnostico los separa por causa-efecto:
 *
 *   A. El esclavo: esp_hosted mantiene DAT0 abajo mientras tenga una
 *      interrupcion en banda sin acknowledge.  Palanca: CCCR_INT_ENABLE=0 y
 *      ack de INT_RAW sobre INT_CLR, los dos por CMD52 (viajan solo por CMD,
 *      asi que funcionan con el bus atascado).  Si al desarmar D0 sube y al
 *      rearmar vuelve a bajar, EL ALAMBRE Y EL CONDUCTOR DEL C6 ESTAN BIEN.
 *   B. Nuestro pad conduciendo 0: se ve en el paso A porque el pad queda
 *      tri-estado durante la foto; si ahi D0 sube, el que conducia eramos
 *      nosotros.
 *   C. La placa (corto a masa o red equivocada): se ve conduciendo D0 como
 *      GPIO en push-pull y leyendo el backread.  Un pad del P4 (40 mA, ~25
 *      ohm) levanta cualquier pull/pull-down de 51K; si conduciendo 1 se lee
 *      0, la red esta cortocircuitada y ningun firmware puede mover datos
 *      por ahi.
 */

#define FN1_GP_OUT_W1TS     (0x500E0000UL + 0x08UL)
#define FN1_GP_OUT_W1TC     (0x500E0000UL + 0x0CUL)
#define FN1_GP_DATA         (0x500E0000UL + 0x04UL)  /* GPIO_OUT[0..31]        */
#define FN1_OUT_SEL_GPIO    256UL              /* OUT_SEL=256 -> GPIO_OUT_REG  */
#define FN1_PAD_MCU_MASK    (7UL << 12)        /* mcu_sel del pad              */
#define FN1_PAD_MCU_GPIO    (1UL << 12)        /* 1 = pad en la matriz GPIO    */

/** Nivel de la pata \p pin con el P4 fuera de la linea (pull del pad intacto). */
static uint32 fn1DatLevel(uint8 pin)
{
    volatile uint32 *out = (volatile uint32 *)(uintptr_t)FN1_GP_OUT(pin);
    uint32 saved = *out;
    uint32 lvl;

    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << pin;
    *out = saved | FN1_OEN_SEL;               /* OE mandado por GPIO_ENABLE=0 */
    delay_ms(1);
    lvl = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;
    *out = saved;
    return lvl;
}

/**
 * Prueba dura de la red: conducir la pata como GPIO a 1 y a 0 con las
 * resistencias internas apagadas y leer que pasa.  Sin cortocircuito, la
 * lectura sigue a la escritura.
 *
 * Medido en D0/D1: 1->0 0->0 y 1->1 0->1, o sea la pata NO hace caso.  Hay tres
 * explicaciones y esta funcion las separa leyendo el pad y el OUT_SEL de vuelta
 * despues de escribirlos:
 *   - la red la mantiene fuerte un conductor externo (el C6, o un corto): el pad
 *     responde, OUT_SEL queda como lo escribimos y la lectura no cambia.
 *   - el pad sigue enganchado a su funcion IOMUX (mcu_sel = 0): en ese caso
 *     OUT_SEL_CFG[pin] NO se usa, el pad lo gobierna la periferica y escribirle
 *     al GPIO es inocuo --que es justo el sintoma--.
 *   - el pad esta muerto (power domain/hold): la escritura al pad no queda.
 * Cada linea imprime mcu_sel, ie, wpu/wpd y drv del pad leido, el OUT_SEL leido
 * y el bit de GPIO_ENABLE, ademas del nivel con 1 y con 0.  El pad se deja
 * EXACTAMENTE como estaba (nivel de GPIO_OUT incluido) porque el barrido tambien
 * corre sobre un pin de control que si sabemos que funciona (TP_RST).
 */
static void fn1DriveAndRead(const char *name, uint8 pin, uint32 *hi, uint32 *lo)
{
    volatile uint32 *pad  = (volatile uint32 *)(uintptr_t)FN1_GP_PAD(pin);
    volatile uint32 *out  = (volatile uint32 *)(uintptr_t)FN1_GP_OUT(pin);
    volatile uint32 *data = (volatile uint32 *)(uintptr_t)FN1_GP_DATA;
    volatile uint32 *en   = (volatile uint32 *)(uintptr_t)FN1_GP_EN;
    uint32 padSave = *pad;
    uint32 outSave = *out;
    uint32 enSave  = *en;
    uint32 datSave = *data;
    uint32 padNow, outNow;

    /* Entrada habilitada, sin WPU/WPD, pad en la matriz GPIO (mcu_sel=1), la
     * pata al GPIO_OUT (OUT_SEL=256) y el OE mandado por GPIO_ENABLE: asi la
     * pata se enciende y se apaga sin interferir con nadie. */
    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << pin;
    *pad = (padSave & ~(FN1_PAD_WPU | FN1_PAD_WPD | FN1_PAD_MCU_MASK))
           | FN1_PAD_IE | FN1_PAD_MCU_GPIO;
    *out = (outSave & ~0x1FFUL) | FN1_OUT_SEL_GPIO | FN1_OEN_SEL;

    *data = datSave | (1UL << pin);                   /* nivel antes del OE    */
    *(volatile uint32 *)(uintptr_t)FN1_GP_OUT_W1TS = 1UL << pin;
    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TS  = 1UL << pin;
    delay_ms(1);
    *hi     = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;
    padNow  = *pad;
    outNow  = *out;

    *(volatile uint32 *)(uintptr_t)FN1_GP_OUT_W1TC = 1UL << pin;
    delay_ms(1);
    *lo = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;

    /* Devolver el pad y el GPIO a lo que estaban, incluido el nivel de OUT. */
    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << pin;
    *out = outSave;
    *pad = padSave;
    *data = datSave;
    if (enSave & (1UL << pin))
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TS = 1UL << pin;
    }

    kprintf("dat: %-4s p%-2u conducido 1->%u 0->%u | pad 0x%08x->0x%08x "
            "mcu=%u ie=%u wpu=%u wpd=%u drv=%u | out_sel=0x%08x en=0x%08x\r\n",
            name, (unsigned)pin, (unsigned)*hi, (unsigned)*lo,
            padSave, padNow,
            (unsigned)((padNow >> 12) & 7U),
            (unsigned)((padNow >> 9)  & 1U),
            (unsigned)((padNow >> 8)  & 1U),
            (unsigned)((padNow >> 7)  & 1U),
            (unsigned)((padNow >> 10) & 3U),
            outNow, (uint32)*en);
}

/**
 * Desarmar la interrupcion en banda del esclavo y acknowledgear la pendiente,
 * todo por CMD52: el INT_RAW leido reescrito sobre INT_CLR (0x0D4 fn1) byte a
 * byte y CCCR_INT_ENABLE (0x04 fn0) a 0.
 */
static void fn1DisarmInterrupt(void)
{
    uint32 raw = 0;
    uint8  v = 0x00U;
    uint   k;

    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    for (k = 0U; k < 4U; k++)
    {
        v = (uint8)(raw >> (8U * k));
        (void)fn1Cmd52Byte(SDIO_FN1, FN1_INT_CLR + k, TRUE, &v);
    }
    v = 0x00U;
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, TRUE, &v);
    v = 0xFFU;
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &v);
    kprintf("dat: desarmada: INT_RAW era 0x%08x, CCCR_INT_ENABLE ahora 0x%02x\r\n",
            raw, (unsigned)v);
}

int sdioDatDiag(void)
{
    struct sdioinfo info;
    uint8  c52[4];
    uint8  v;
    uint32 lvl, busy, hi, lo, i;
    bool   ok;

    if (!sd_host_ready)
    {
        kprintf("dat: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    /* A 400 kHz: el unico reloj donde esta probado que el C6 contesta. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_PROBE, FALSE) != OK || !info.up)
    {
        kprintf("dat: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    fn1SdioSetup(FALSE);
    delay_ms(2);
    fn1ArmFn1Interrupt();
    kprintf("dat: C6 arriba RCA=0x%04x %u kHz STATUS=0x%08x\r\n",
            (unsigned)info.rca, (unsigned)info.clk_khz,
            (uint32)SD_REG(SDHOST_STATUS));

    /* 1. Con la interrupcion ARMADA: nivel de D0 y del bloque. */
    lvl  = fn1DatLevel(14U);
    busy = (SD_REG(SDHOST_STATUS) >> 9) & 1U;
    kprintf("dat: int ARMADA     D0=%u  STATUS.data_busy=%u\r\n",
            (unsigned)lvl, (unsigned)busy);

    /* 2. Desarmar + ack por CMD52, y volver a mirar. */
    fn1DisarmInterrupt();
    delay_ms(5);
    lvl  = fn1DatLevel(14U);
    busy = (SD_REG(SDHOST_STATUS) >> 9) & 1U;
    kprintf("dat: int DESARMADA  D0=%u  STATUS.data_busy=%u  <= si sube, quien "
            "la bajaba era el esclavo o el host\r\n",
            (unsigned)lvl, (unsigned)busy);

    /* 3. Rearmar: la causa-efecto.  Si D0 vuelve a 0 al armar, la linea vive
     *    y obedece al esclavo => el alambre esta BIEN. */
    fn1ArmFn1Interrupt();
    delay_ms(5);
    lvl  = fn1DatLevel(14U);
    busy = (SD_REG(SDHOST_STATUS) >> 9) & 1U;
    kprintf("dat: int REARMADA   D0=%u  STATUS.data_busy=%u\r\n",
            (unsigned)lvl, (unsigned)busy);
    fn1DisarmInterrupt();
    delay_ms(5);

    /* 4. Primera fase de datos con el esclavo en reposo: 4 bytes del CCCR de
     *    fn0 por PIO, comparados con los mismos 4 por CMD52. */
    for (i = 0U; i < 4U; i++)
    {
        c52[i] = 0U;
        (void)fn1Cmd52Byte(SDIO_FN0, i, FALSE, &c52[i]);
    }
    memset(s_frame, 0, 4U);
    s_cmd53ErrSeen = 0U;
    ok = fn1Cmd53Pio(SDIO_FN0, 0x000U, FALSE, s_frame, 4U);
    kprintf("dat: CMD52=%02x %02x %02x %02x | PIO=%s %02x %02x %02x %02x "
            "pal=%u hw=0x%08x\r\n", c52[0], c52[1], c52[2], c52[3],
            ok ? "OK  " : "FAIL", s_frame[0], s_frame[1], s_frame[2],
            s_frame[3], (unsigned)sd_pio_words, sd_hwerr);
    (void)sdBlockReset();

    /* 5. Prueba de placa conduciendo los SEIS pads del slot 1, uno a uno, y el
     *    control: TP_RST (gpio29), una salida que si sabemos que funciona (el
     *    touch se resetea con ella).  Si el control obedece y los seis no, el
     *    problema esta en los pads del slot 1; si solo D0/D1 fallan, esas dos
     *    redes las mueve algo de fuera. */
    kprintf("dat: ---- control: pad de una salida que funciona ----\r\n");
    fn1DriveAndRead("CTRL", (uint8)TP_RST, &hi, &lo);
    kprintf("dat: ---- los seis pads del slot 1 ----\r\n");
    for (i = 0U; i < FN1_NBUSH; i++)
    {
        fn1DriveAndRead(s_busNames[i], s_busPins[i], &hi, &lo);
    }

    v = 0xFFU;
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &v);
    kprintf("dat: CCCR_INT_ENABLE final=0x%02x  STATUS=0x%08x\r\n",
            (unsigned)v, (uint32)SD_REG(SDHOST_STATUS));

    sd_xfer_trace   = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ================================================================== */
/* Donde esta el cable de datos (`wifi wires`)                         */
/* ================================================================== */

/*
 * Por que esta sonda.  `wifi dat` condujo los seis pads del slot 1 como GPIO
 * push-pull (40 mA, ~25 ohm) y leyo el backread: D2, D3, CMD y CLK obedecen
 * (1->1 0->0), y el control (TP_RST, una salida que si funciona) tambien.  D0
 * (gpio14) y D1 (gpio15) NO: clavados en 0 y en 1 sea lo que sea que escribamos
 * y sea cual sea el pull interno.  Un pad sano que conduce en push-pull gana
 * contra cualquier pull-up de 51K; si la lectura no sigue a la escritura, ese
 * pad no esta en la red que creemos (o la red la mueve un conductor fuerte).
 *
 * Las fotos de `wifi bus` ya decian lo mismo desde el otro lado: con el C6 en
 * reset (GPIO54 abajo y CMD52 sin respuesta --esa palanca si esta validada--),
 * D2/D3/CMD/CLK siguen al pull interno, o sea redes flotantes, mientras D0 se
 * queda en 0 y D1 en 1.  Las dos cosas juntas apuntan a que esas dos patas no
 * son DAT0/DAT1.
 *
 * En vez de seguir adivinando numeros, esta sonda pregunta por el alambre.  El
 * esclavo esp_hosted interrumpe EN BANDA: cuando tiene un paquete C2H que
 * entregar baja DAT0, y lo suelta cuando el host acknowledarea INT_CLR.  Eso es
 * un conductor real moviendo el pad que nosotros damos por malo, asi que sacando
 * dos instantaneas de los 48 receptores del P4 --una con la interrupcion
 * pendiente y otra despues del ack-- el pad que cambia de nivel ES el DAT0
 * verdadero, sin suponer nada del mapa de pines.
 *
 * Solo lee: de cada pad toma el bit fun_ie (el receptor) y lo devuelve tal cual
 * al terminar la instantanea; no toca direcciones, pulls ni salidas, asi que la
 * microSD, el LCD, el I2C y el touch se quedan con sus pads intactos.
 */

#define FN1_WIRES_NPAD    48U                     /* IO_MUX cubre 0..47       */
#define FN1_GP_IN1_REG    (0x500E0000UL + 0x40UL) /* GPIO_IN1: pads 32..47    */

/** Instantanea de los 48 pads con el receptor encendido (y nada mas). */
static void fn1WiresSnapshot(uint32 *in_lo, uint32 *in_hi)
{
    static uint32 saved[FN1_WIRES_NPAD];
    volatile uint32 *pad;
    uint   i;

    for (i = 0U; i < FN1_WIRES_NPAD; i++)
    {
        pad      = (volatile uint32 *)(uintptr_t)FN1_GP_PAD(i);
        saved[i] = *pad;
        *pad     = saved[i] | FN1_PAD_IE;
    }
    delay_ms(1);
    *in_lo = *(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG;
    *in_hi = *(volatile uint32 *)(uintptr_t)FN1_GP_IN1_REG;
    for (i = 0U; i < FN1_WIRES_NPAD; i++)
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_PAD(i) = saved[i];
    }
}

static void fn1WiresShow(const char *tag, uint32 in_lo, uint32 in_hi)
{
    kprintf("wires: %-12s IN=0x%08x IN1=0x%08x\r\n", tag, in_lo, in_hi);
}

/** Imprimir los pads que cambiaron entre dos instantaneas. */
static void fn1WiresDiff(const char *tag, uint32 a_lo, uint32 a_hi,
                         uint32 b_lo, uint32 b_hi)
{
    uint32 d_lo = a_lo ^ b_lo;
    uint32 d_hi = a_hi ^ b_hi;
    uint   i;
    uint   n = 0U;

    for (i = 0U; i < FN1_WIRES_NPAD; i++)
    {
        uint32 changed = (i < 32U) ? (d_lo >> i) : (d_hi >> (i - 32U));
        uint32 before  = (i < 32U) ? (a_lo >> i) : (a_hi >> (i - 32U));
        uint32 after   = (i < 32U) ? (b_lo >> i) : (b_hi >> (i - 32U));

        if ((changed & 1U) != 0U)
        {
            kprintf("wires: %s p%-2u %u -> %u\r\n", tag, (unsigned)i,
                    (unsigned)(before & 1U), (unsigned)(after & 1U));
            n++;
        }
    }
    if (n == 0U)
    {
        kprintf("wires: %s nadie cambio\r\n", tag);
    }
}

int sdioWiresDiag(void)
{
    struct sdioinfo info;
    uint32 a_lo, a_hi, b_lo, b_hi, c_lo, c_hi;
    uint32 raw = 0U;
    uint8  ie  = 0U;

    if (!sd_host_ready)
    {
        kprintf("wires: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    if (sdioEnumerateAt(&info, SD_CARD_DIV_PROBE, FALSE) != OK || !info.up)
    {
        kprintf("wires: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    fn1SdioSetup(FALSE);
    delay_ms(2);

    /* 1. Interrupcion pendiente: leer INT_RAW SIN acknowledgear y fotograficar
     *    los 48 receptores.  Si el cable de DAT0 esta en algun pad, ahi ese pad
     *    esta abajo. */
    fn1ArmFn1Interrupt();
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    (void)fn1Cmd52Byte(SDIO_FN0, CCCR_INT_ENABLE, FALSE, &ie);
    kprintf("wires: IRQ pendiente INT_RAW=0x%08x CCCR_INT_ENABLE=0x%02x "
            "STATUS=0x%08x\r\n", raw, (unsigned)ie,
            (uint32)SD_REG(SDHOST_STATUS));
    fn1WiresSnapshot(&a_lo, &a_hi);
    fn1WiresShow("pendiente", a_lo, a_hi);

    /* 2. Ack (INT_CLR = INT_RAW): el esclavo suelta DAT0.  Segunda foto: el pad
     *    que sube es el cable de DAT0. */
    (void)fn1AckPendingInterrupts();
    delay_ms(10);
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    kprintf("wires: tras el ack INT_RAW=0x%08x STATUS=0x%08x\r\n", raw,
            (uint32)SD_REG(SDHOST_STATUS));
    fn1WiresSnapshot(&b_lo, &b_hi);
    fn1WiresShow("ack", b_lo, b_hi);
    fn1WiresDiff("ack", a_lo, a_hi, b_lo, b_hi);

    /* 3. Provocar otra vez: el doorbell HOST_TO_SLAVE hace que el esclavo
     *    responda con un paquete C2H y vuelva a asserts la interrupcion. */
    fn1ArmFn1Interrupt();
    (void)fn1WriteReg(FN1_HOST_TO_SLAVE, 1U);
    delay_ms(50);
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    kprintf("wires: tras doorbell INT_RAW=0x%08x STATUS=0x%08x\r\n", raw,
            (uint32)SD_REG(SDHOST_STATUS));
    fn1WiresSnapshot(&c_lo, &c_hi);
    fn1WiresShow("doorbell", c_lo, c_hi);
    fn1WiresDiff("doorbell", b_lo, b_hi, c_lo, c_hi);

    /* 4. Referencia de los seis pads que damos por buenos en las dos primeras
     *    fotos, para ver si alguno se mueve aunque no salga en el diff. */
    kprintf("wires: 14=%u/%u 15=%u/%u 16=%u/%u 17=%u/%u 18=%u/%u 19=%u/%u "
            "(pendiente/ack)\r\n",
            (unsigned)((a_lo >> 14) & 1U), (unsigned)((b_lo >> 14) & 1U),
            (unsigned)((a_lo >> 15) & 1U), (unsigned)((b_lo >> 15) & 1U),
            (unsigned)((a_lo >> 16) & 1U), (unsigned)((b_lo >> 16) & 1U),
            (unsigned)((a_lo >> 17) & 1U), (unsigned)((b_lo >> 17) & 1U),
            (unsigned)((a_lo >> 18) & 1U), (unsigned)((b_lo >> 18) & 1U),
            (unsigned)((a_lo >> 19) & 1U), (unsigned)((b_lo >> 19) & 1U));

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* QuE pad del P4 es el DAT0 del C6 (`wifi hunt`)                      */
/* ==================================================================== */

/*
 * De donde viene esta sonda.
 *
 *   - `wifi dat` condujo los seis pads del slot 1 como GPIO push-pull (40 mA)
 *     y leyo el backread: D2 (16), D3 (17), CMD (19) y CLK (18) obedecen, y el
 *     control (TP_RST, gpio29) tambien.  D0 (14) clavado en 0 y D1 (15)
 *     clavado en 1: ni conduciendo ganan a nada, o sea que esos dos pads no estan
 *     detras de las redes que creiamos.
 *   - `wifi wires` fotograpio los 48 receptores del P4 en tres instantes y el
 *     unico pad que obedecio al esclavo fue el 15: con el doorbell
 *     HOST_TO_SLAVE el C6 declara paquete nuevo (INT_RAW bit23) y ese pad cae
 *     de 1 a 0 --exactamente lo que hace un esclavo afirmando la interrupcion
 *     en banda, que en modo 1-bit ES la linea DAT0--.
 *   - y mientras la entrada DAT0 del CIU siga apuntando al pad 14, todo CMD53
 *     lee ceros con end-bit-error a cualquier reloj (400 kHz..20 MHz) y en 1-bit
 *     y en 4-bit, aunque el CMD52 del mismo registro traiga 32 02 02 02.
 *
 * La prueba definitiva no mira niveles: pide datos.  La ENTRADA de cada senal
 * del slot 1 es un multiplexador independiente de su SALIDA --FUNC_IN_SEL_CFG[s]
 * elige que pad alimenta a la senal s, y FUNC_OUT_SEL_CFG[pin].OUT_SEL elige
 * que pad la recibe--.  Asi que se puede dejar la salida DAT0 en el pad 14 y
 * poner la entrada en otro pad.  Recorriendo pads, el que devuelva por CMD53 los
 * mismos bytes que el CMD52 (que viaja por CMD y esta probado que funciona) es
 * el cable de DAT0, sin suponer nada del mapa de pines.
 *
 * Seguridad: de cada pad se toca solo el bit de receptor (fun_ie) y, si el pad
 * esta libre, su funcion de pad (mcu_sel) para que la matriz GPIO pueda
 * verlo; ambos se devuelven tal cual.  Los pads de lo que ya funciona --LCD
 * (20/21/23/26/27/28), I2C (7/8), I2S (9..13), microSD (39..44), touch (29/50),
 * USB (24..27), reset del C6 (54), strap (6) y PWRON (49)-- jamas cambian de
 * mcu_sel, porque su salida por IOMUX fijo es lo que los mantiene vivos.  Lo
 * unico que se mueve de verdad es FUNC_IN_SEL_CFG[2] (la entrada DAT0 del slot
 * 1, senal que no usa nadie mas) y se restaura al valor leido.
 */

/* senal de ENTRADA DAT0 del slot 1 por la matriz GPIO: soc/gpio_sig_map.h
 * define SD_CARD_CDATA0_2_PAD_IN_IDX = 2 (CLK=0, CMD=1, D0..D3=2..5). */
#define FN1_HUNT_SIG_D0     2U
/* FUNC_IN_SEL_CFG[2]: GPIO_BASE + 0x158 + 4*senal; [5:0]=pad, [7]=SIG_IN_SEL. */
#define FN1_HUNT_IN_SEL     (0x500E0000UL + 0x158UL + 4UL * FN1_HUNT_SIG_D0)
#define FN1_IN_SIG_IN_SEL   (1UL << 7)

/* Candidatos, en orden de probabilidad: los seis pads con los que el bus ya
 * esta operando, luego los vecinos, luego los sueltos. */
static const uint8 s_huntPins[] = {
    14U, 15U, 16U, 17U, 18U, 19U,
    13U, 22U, 20U, 21U, 23U, 51U, 52U,
    0U, 1U, 2U, 3U, 4U, 5U,
    33U, 34U, 35U, 36U, 37U, 38U,
    45U, 46U, 47U, 48U, 54U, 6U
};
#define FN1_HUNT_NPIN       (sizeof(s_huntPins) / sizeof(s_huntPins[0]))

/**
 * Podeos pasar temporalmente este pad por la matriz GPIO?  Solo los que no
 * llevan senal de ningun periferico que funciona: los demas se leen (con
 * fun_ie) pero conservan su funcion de pad, que es lo que mantiene vivo el
 * LCD, el I2S, el I2C, la microSD o el reset del C6.
 */
static bool fn1HuntMcuAllowed(uint8 pin)
{
    if (pin >= 7U && pin <= 13U) return FALSE;   /* I2C (7/8) + I2S (9..13)  */
    if (pin >= 20U && pin <= 28U) return FALSE;  /* LCD + USB                */
    if (pin >= 39U && pin <= 44U) return FALSE;  /* microSD, slot 0 por IOMUX*/
    if (pin == 6U || pin == 29U || pin == 49U || pin == 50U
        || pin == 54U)
    {
        return FALSE;                            /* strap, TP_RST, PWRON,
                                                    TP_INT, CHIP_PU del C6     */
    }
    return TRUE;
}

/**
 * Poner la ENTRADA DAT0 del slot 1 en el pad \p cand y pedir \p len bytes por
 * CMD53 en PIO, comparandolos con los \p truth leidos por CMD52.  Imprime una
 * linea por intento y devuelve TRUE si coinciden todos.
 */
static bool fn1HuntTry(uint8 cand, uint8 fn, uint32 addr, const uint8 *truth,
                       uint32 len)
{
    volatile uint32 *isel = (volatile uint32 *)(uintptr_t)FN1_HUNT_IN_SEL;
    volatile uint32 *pad  = (volatile uint32 *)(uintptr_t)FN1_GP_PAD(cand);
    volatile uint32 *out  = (volatile uint32 *)(uintptr_t)FN1_GP_OUT(cand);
    uint32  iselSave = *isel;
    uint32  padSave  = *pad;
    uint32  outSave  = *out;
    uint32  enSave;
    uint32  st, hw;
    uint32  lvlBefore, lvlAfter;
    bool    ok, match, mcu;
    uint32  i;

    memset(s_frame, 0, len);

    /* El pad tiene que estar en la matriz GPIO para que su receptor llegue al
     * multiplexador; los protegidos no se mueven de su funcion de pad. */
    mcu = fn1HuntMcuAllowed(cand);
    enSave = (*(volatile uint32 *)(uintptr_t)FN1_GP_EN >> cand) & 1U;
    *pad = padSave | FN1_PAD_IE;
    if (mcu && (padSave & FN1_PAD_MCU_MASK) != FN1_PAD_MCU_GPIO)
    {
        *pad = (padSave & ~FN1_PAD_MCU_MASK) | FN1_PAD_IE | FN1_PAD_MCU_GPIO;
        /* Con el pad en la matriz, su conductor quedaria gobernado por
         * GPIO_ENABLE; se apaga por la via dura (OEN_SEL=1 + ENABLE=0) para
         * que durante la prueba el pad sea solo receptor, y se devuelve igual. */
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << cand;
        *out = outSave | FN1_OEN_SEL;
    }

    *isel = FN1_IN_SIG_IN_SEL | (uint32)cand;    /* fuente de DAT0 = pad cand */
    delay_ms(1);
    lvlBefore = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> cand) & 1U;

    ok = fn1Cmd53Pio(fn, addr, FALSE, s_frame, len);
    st = (uint32)SD_REG(SDHOST_STATUS);
    hw = sd_hwerr;
    if (cand < 32U)
    {
        lvlAfter = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> cand) & 1U;
    }
    else
    {
        lvlAfter = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN1_REG
                    >> (cand - 32U)) & 1U;
    }

    *isel = iselSave;                            /* restaurar la entrada       */
    if (mcu && (padSave & FN1_PAD_MCU_MASK) != FN1_PAD_MCU_GPIO)
    {
        *out = outSave;
    }
    *pad = padSave;
    if (enSave != 0U)
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TS = 1UL << cand;
    }
    delay_ms(1);

    match = ok;
    for (i = 0U; i < len && match; i++)
    {
        match = (s_frame[i] == truth[i]);
    }

    kprintf("hunt: in<-p%-2u%s nivel=%u/%u %s", (unsigned)cand,
            mcu ? "" : " (pad fijo)", (unsigned)lvlBefore, (unsigned)lvlAfter,
            ok ? "CMD53" : "CMD53-FALLO");
    for (i = 0U; i < len; i++)
    {
        kprintf(" %02x", s_frame[i]);
    }
    kprintf(" | CMD52:");
    for (i = 0U; i < len; i++)
    {
        kprintf(" %02x", truth[i]);
    }
    kprintf("  => %s  busy=%u hw=0x%08x\r\n",
            match ? "COINCIDE" : "no",
            (unsigned)((st & SD_STATUS_DATA_BUSY) ? 1U : 0U), hw);
    return match;
}

/**
 * Sentido host -> esclavo con el pad ganador tambien como SALIDA DAT0: un
 * CMD53 de escritura tiene que llegar.  La prueba es funcional y no invasiva:
 * se acknowledean por CMD53 los bits pendientes de INT_RAW reescribiendolos
 * sobre INT_CLR (0x0D4), que es justo lo que hace el driver de referencia, y se
 * vuelve a leer INT_RAW con CMD52 --que no usa las lineas DAT--.  Si algún bit
 * se limpio, el byte que escribio el CIU salio por ese pad y el esclavo lo
 * entiendio.
 */
static void fn1HuntWriteTest(uint8 cand)
{
    volatile uint32 *out  = (volatile uint32 *)(uintptr_t)FN1_GP_OUT(cand);
    volatile uint32 *pad  = (volatile uint32 *)(uintptr_t)FN1_GP_PAD(cand);
    uint32  outSave = *out, padSave = *pad;
    uint32  rawBefore = 0U, rawAfter = 0U;
    uint8   w[4];
    bool    ok;

    if (cand != C6_SDIO_D0)
    {
        /* pad a la matriz y la salida DAT0 del CIU a este pad; la antigua (14)
         * no se toca: si el pad 14 no esta en la red, conducir alli es
         * inofensivo, y si lo estuviera, ambas salidas llevan EL MISMO senal. */
        *pad = (padSave & ~FN1_PAD_MCU_MASK) | FN1_PAD_IE | FN1_PAD_MCU_GPIO;
        gpio_matrix_out(cand, FN1_HUNT_SIG_D0, FALSE, FALSE);
    }

    (void)fn1WriteReg(FN1_HOST_TO_SLAVE, 1U);    /* doorbell: paquete C2H      */
    delay_ms(50);
    if (!fn1ReadReg(FN1_INT_RAW, &rawBefore))
    {
        kprintf("hunt: escritura: INT_RAW no respondo (hw=0x%08x)\r\n",
                sd_hwerr);
        rawBefore = 0U;
    }
    kprintf("hunt: escritura: INT_RAW antes=0x%08x\r\n", rawBefore);

    if (rawBefore != 0U)
    {
        w[0] = (uint8)rawBefore;
        w[1] = (uint8)(rawBefore >> 8);
        w[2] = (uint8)(rawBefore >> 16);
        w[3] = (uint8)(rawBefore >> 24);
        ok = fn1Cmd53Pio(SDIO_FN1, FN1_INT_CLR, TRUE, w, 4U);
        (void)fn1ReadReg(FN1_INT_RAW, &rawAfter);
        kprintf("hunt: escritura: CMD53 %s 4B->INT_CLR, INT_RAW ahora=0x%08x "
                "=> %s\r\n", ok ? "OK  " : "FALLO", rawAfter,
                ((rawBefore & ~rawAfter) != 0U)
                    ? "ALGUN BIT SE LIMPIO: lo que escribimos nos llego"
                    : "no se limpio nada: nuestra salida no llega al esclavo");
    }
    else
    {
        kprintf("hunt: escritura: sin bits pendientes que limpiar, "
                "prueba no concluyente\r\n");
    }

    if (cand != C6_SDIO_D0)
    {
        *out = outSave;                          /* devolver el enrutado       */
        *pad = padSave;
    }
}

int sdioDatHuntDiag(void)
{
    struct sdioinfo info;
    uint8  truth[8];
    uint32 raw = 0U;
    uint32 plen = 0U;
    uint32 i, k;
    uint8  winner = 0xFFU;

    if (!sd_host_ready)
    {
        kprintf("hunt: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    /* Enumerar ya deja al C6 recien salido de su reset (sdioEnumerateAt pulsa
     * CHIP_PU 10 ms y espera 1500 ms, la politica del driver de referencia),
     * asi que INT_RAW aqui es el valor del arranque, no una sobra de las
     * pruebas anteriores. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_PROBE, FALSE) != OK || !info.up)
    {
        kprintf("hunt: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    fn1SdioSetup(FALSE);
    delay_ms(2);

    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    (void)fn1ReadReg(FN1_PACKET_LEN, &plen);
    kprintf("hunt: C6 recien enumerado: INT_RAW=0x%08x PACKET_LEN=0x%08x "
            "STATUS=0x%08x\r\n", raw, plen, (uint32)SD_REG(SDHOST_STATUS));

    /* Dejar al esclavo en reposo ANTES de barrer: sin CCCR_INT_ENABLE y con lo
     * pendiente acknowledeado por CMD52.  Si no, el esclavo tiene la interrupcion
     * en banda afirmada --sosteniendo DAT0 abajo-- y la fase de datos de todo
     * candidato lee ceros, que fue exactamente el falso negativo de la primera
     * version de esta sonda. */
    fn1DisarmInterrupt();
    delay_ms(5);

    /* Patron distintivo y SIN efecto sobre la linea: la cola que el doorbell
     * deja montada se anuncia en PACKET_LEN y alli sigue hasta que alguien la
     * lea, asi que es un valor no nulo y estable que podemos pedir por CMD53
     * mientras DAT0 esta en reposo. */
    (void)fn1WriteReg(FN1_HOST_TO_SLAVE, 1U);
    delay_ms(50);
    if (!fn1ReadReg(FN1_PACKET_LEN, &plen) || plen == 0U)
    {
        kprintf("hunt: el doorbell no dejo cola (PACKET_LEN=0x%08x): el patron "
                "de prueba puede ser todo ceros\r\n", plen);
    }
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    for (i = 0U; i < 4U; i++)
    {
        truth[i] = (uint8)(plen >> (8U * i));
    }
    kprintf("hunt: patron CMD52 (fn1 PACKET_LEN): %02x %02x %02x %02x   "
            "INT_RAW ahora=0x%08x  STATUS=0x%08x\r\n",
            truth[0], truth[1], truth[2], truth[3], raw,
            (uint32)SD_REG(SDHOST_STATUS));
    kprintf("hunt: pads 13..19 en reposo=%u%u%u%u%u%u%u (IN=0x%08x)\r\n",
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 13) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 14) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 15) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 16) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 17) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 18) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 19) & 1U),
            (uint32)*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG);

    kprintf("hunt: barrido de la ENTRADA DAT0, 4B de PACKET_LEN (fn1) por "
            "CMD53 en PIO contra su CMD52:\r\n");
    for (k = 0U; k < FN1_HUNT_NPIN; k++)
    {
        if (fn1HuntTry(s_huntPins[k], SDIO_FN1, FN1_PACKET_LEN, truth, 4U))
        {
            winner = s_huntPins[k];
            break;
        }
    }

    if (winner == 0xFFU)
    {
        /* Segunda vuelta contra el CCCR de fn0 (8 bytes, patron propio: 32 02
         * 02 02 .. 40) por si la ventana de la funcion 1 no admite CMD53 de
         * registro en este esclavo. */
        for (i = 0U; i < 8U; i++)
        {
            truth[i] = 0U;
            (void)fn1Cmd52Byte(SDIO_FN0, i, FALSE, &truth[i]);
        }
        kprintf("hunt: sin match en fn1; segunda vuelta con 8B del CCCR:\r\n");
        for (k = 0U; k < FN1_HUNT_NPIN; k++)
        {
            if (fn1HuntTry(s_huntPins[k], SDIO_FN0, 0x000U, truth, 8U))
            {
                winner = s_huntPins[k];
                break;
            }
        }
    }

    if (winner != 0xFFU)
    {
        kprintf("hunt: LA ENTRADA DAT0 DEL CIU COINCIDE CON EL PAD %u "
                "(el driver de referencia llama D0 al %u)\r\n",
                (unsigned)winner, (unsigned)C6_SDIO_D0);
        fn1HuntWriteTest(winner);
    }
    else
    {
        kprintf("hunt: ningun pad devolvio los bytes del CMD52: el problema "
                "no es (solo) el numero de pad de la entrada\r\n");
    }

    (void)sdBlockReset();          /* la maquina de datos pudo quedar muerta  */
    kprintf("hunt: tras sdBlockReset STATUS=0x%08x\r\n",
            (uint32)SD_REG(SDHOST_STATUS));

    sd_xfer_trace   = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* Quien manda los pads 14/15: hold de pad y dueno LP/HP (`wifi hold`)  */
/* ==================================================================== */

/*
 * De donde viene esta sonda.
 *
 *   - `wifi hunt` recorrio los 31 pads candidatos moviendo la ENTRADA DAT0 del
 *     CIU (FUNC_IN_SEL_CFG[2]) y NINGUNO devolvio por CMD53 los bytes que el
 *     CMD52 trae por CMD.  Todos leyeron un nivel estatico: los pads altos
 *     murieron por timeout (hwerr=0x30000004, ni siquiera bit de arranque) y los
 *     bajos por end-bit-error (0x40008000).  O sea que el esclavo no condujo
 *     NINGUN pad durante la fase de datos, ni el 15, que sin embargo si obedece
 *     a su interrupcion en banda (`wifi wires`).  El numero de pad de la entrada
 *     esta descartado: lo que falla es que el bus de datos nunca empieza.
 *   - y el patron de fallos tiene una firma rara: de los seis pads del slot 1,
 *     D2 (16), D3 (17), CLK (18) y CMD (19) obedecen al conductor GPIO, y los
 *     dos unicos que no hacen caso son D0 (14, clavado a 0) y D1 (15, clavado a
 *     1).  Es precisamente el par de pads que en el P4 NO es un pad normal.
 *
 *soc/esp32p4/register/hw_ver3/soc/lp_iomux_reg.h: los pads GPIO0..GPIO15 tienen
 * DOS duenos.  El IO_MUX de alta potencia (REG_IO_MUX_BASE=0x500E1000, pad n en
 * +0x04+4n, el que hemos venido usando) y el IO_MUX de bajo poder (LP_IOMUX =
 * DR_REG_LP_IOMUX_BASE = 0x50120000+0xB000 = 0x5012B000, pad n en +0x08+4n).  El
 * bit 4 del registro LP, MUX_SEL, dice quien manda: "1: use LP GPIO, 0: use
 * digital GPIO".  Con MUX_SEL=1 el pad ignora todo lo que escribamos en
 * IO_MUX_GPIO14_REG y en FUNC_OUT_SEL_CFG[14]: la escritura queda en el registro
 * HP (que es lo que leimos: mcu=1, out_sel con OEN_SEL) pero el pad sigue al otro
 * dueno.  Core/soc/esp32p4/gpio_periph.c:70 (GPIO_HOLD_MASK) anade la segunda
 * pieza: un bit de hold por pad, en LP_IOMUX_LP_PAD_HOLD_REG (0x5012B04C,
 * bits[15:0] para GPIO0..15) y en HP_SYSTEM_GPIO_O_HOLD_CTRL0/1 (0x500E5074 para
 * GPIO16..47 y 0x500E5078 para GPIO48..56).  Con el bit puesto la CONFIGURACION
 * del pad queda congelada: el registro se escribe pero el pad conserva direccion
 * y valor.  Eso encaja con lo medido mejor que un cable suelto: un pad congelado
 * como entrada con receptor encendido deja VER la linea (por eso el 15 sigue al
 * C6 en `wifi wires`) pero no deja conducirla (por eso no gana ni a 1 ni a 0), y
 * un pad congelado como salida a 0 explica el 14 clavado.
 *
 * Esta sonda lee la verdad de esos registros y actua solo si hace falta:
 *  1. fotografia del dominio LP (reloj del bloque y version, para poder distinguir
 *     "hold/mux en 0" de "el bloque no esta accesible y leimos basura"),
 *  2. prueba de conduccion y prueba de pull sobre 14/15 (mas el pad libre 22 de
 *     contraste, que no lleva nada en esta placa),
 *  3. suelta el hold de 14/15 (bits 14 y 15 del registro LP) y el de 16..19 (bits
 *     0..3 del HP_LO), apaga el conductor LP de 14/15 antes de quitarles el dueno
 *     y limpia MUX_SEL de esos dos pads,
 *  4. repite conduccion y pull, re-enumerar (sdioEnumerateAt -> sdioPadsInit
 *     vuelve a dejar los seis pads en la matriz GPIO, y ahora esa escritura debe
 *     llegar al pad) y pide otra vez los 8 bytes del CCCR por CMD53 con la entrada
 *     DAT0 en el pad 14 y luego en el 15, contra la verdad del CMD52.
 *
 * Seguridad: NO se toca el hold ni el mux de los pads 0..13 (ahi viven I2C 7/8 e
 * I2S 9..13), ni los registros HP_SYSTEM de hold mas alla de los bits 0..3, ni
 * nada de los dominios de poder.  Los bits de hold y de MUX_SEL que se limpian se
 * dejan limpios a proposito: son los pads del slot 1, que en esta placa no usa
 * nadie mas, y su estado "congelado" es lo que estamos corrigiendo.  Termina con
 * sdBlockReset() (la maquina de datos pudo quedar muerta) y fn1RestoreSlot0(),
 * igual que `wifi hunt`, y el CMD52 sigue siendo el unico camino que escribe en
 * el esclavo durante la fase de lectura.
 */

/* LP_IOMUX = DR_REG_LP_IOMUX_BASE (reg_base.h:158, LPPERIPH 0x50120000 + 0xB000) */
#define FN1_LP_IOM          0x5012B000UL
#define FN1_LP_IOM_CLK      (FN1_LP_IOM + 0x00UL)
#define FN1_LP_IOM_VER      (FN1_LP_IOM + 0x04UL)
#define FN1_LP_IOM_PAD(n)   (FN1_LP_IOM + 0x08UL + 4UL * (n))
#define FN1_LP_IOM_HOLD     (FN1_LP_IOM + 0x4CUL)   /* bits[15:0] = GPIO0..15  */
#define FN1_LP_PAD_DRV(m)   ((m) & 3U)              /* [1:0]                    */
#define FN1_LP_PAD_RDE      (1UL << 2)              /* pull-down del LP         */
#define FN1_LP_PAD_RUE      (1UL << 3)              /* pull-up del LP           */
#define FN1_LP_MUX_SEL      (1UL << 4)              /* 1 = el pad lo manda LP   */
#define FN1_LP_PAD_FUN(m)   (((m) >> 5) & 3U)       /* [6:5]                    */
#define FN1_LP_SLP_SEL      (1UL << 7)
#define FN1_LP_SLP_IE       (1UL << 8)
#define FN1_LP_SLP_OE       (1UL << 9)
#define FN1_LP_FUN_IE       (1UL << 10)

/* LP_GPIO = DR_REG_LP_GPIO_BASE (reg_base.h:157, LPPERIPH + 0xA000) */
#define FN1_LP_GPIO         0x5012A000UL
#define FN1_LP_GPIO_CLK     (FN1_LP_GPIO + 0x00UL)
#define FN1_LP_GPIO_VER     (FN1_LP_GPIO + 0x04UL)
#define FN1_LP_GPIO_OUT     (FN1_LP_GPIO + 0x08UL)
#define FN1_LP_GPIO_EN      (FN1_LP_GPIO + 0x14UL)
#define FN1_LP_GPIO_EN_CLR  (FN1_LP_GPIO + 0x1CUL)  /* W1TC: escribir 1 apaga   */
#define FN1_LP_GPIO_IN      (FN1_LP_GPIO + 0x30UL)

/* Relojes del dominio LP: bit 20 = ck_en_lp_iomux, default 1 (lpperi_struct.h;
 * ahi no hay bit de LP_GPIO, ese bloque trae su propio CLK_EN en +0x00, bit 0). */
#define FN1_LP_PERI_CLK     0x50120000UL
#define FN1_CK_LP_IOMUX     (1UL << 20)

/* HP_SYSTEM = DR_REG_HP_SYS_BASE (reg_base.h:119, HPPERIPH1 + 0x25000) */
#define FN1_HPSYS           0x500E5000UL
#define FN1_HP_HOLD_LO      (FN1_HPSYS + 0x74UL)    /* GPIO16..47               */
#define FN1_HP_HOLD_HI      (FN1_HPSYS + 0x78UL)    /* GPIO48..56               */

/** Fotografia del estado de hold/mux de los pads del slot 1. */
static void fn1HoldSnapshot(const char *tag)
{
    uint32 lpClk  = *(volatile uint32 *)(uintptr_t)FN1_LP_PERI_CLK;
    uint32 iomClk = *(volatile uint32 *)(uintptr_t)FN1_LP_IOM_CLK;
    uint32 iomVer = *(volatile uint32 *)(uintptr_t)FN1_LP_IOM_VER;
    uint32 gpioClk= *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_CLK;
    uint32 gpioVer= *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_VER;
    uint32 lpHold = *(volatile uint32 *)(uintptr_t)FN1_LP_IOM_HOLD;
    uint32 hpLo   = *(volatile uint32 *)(uintptr_t)FN1_HP_HOLD_LO;
    uint32 hpHi   = *(volatile uint32 *)(uintptr_t)FN1_HP_HOLD_HI;
    uint32 lp14   = *(volatile uint32 *)(uintptr_t)FN1_LP_IOM_PAD(14);
    uint32 lp15   = *(volatile uint32 *)(uintptr_t)FN1_LP_IOM_PAD(15);
    uint32 lpOut  = *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_OUT;
    uint32 lpEn   = *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_EN;
    uint32 lpIn   = *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_IN;

    kprintf("hold[%s]: LP_PERI_CLK=0x%08x (iomux=%u) | LP_IOM clk=0x%08x "
            "ver=0x%08x | LP_GPIO clk=0x%08x (en=%u) ver=0x%08x\r\n", tag, lpClk,
            (unsigned)((lpClk & FN1_CK_LP_IOMUX) ? 1U : 0U),
            iomClk, iomVer, gpioClk,
            (unsigned)(gpioClk & 1U), gpioVer);
    kprintf("hold[%s]: HOLD_LP=0x%08x (p14=%u p15=%u) HOLD_HP_LO=0x%08x "
            "(p16=%u p17=%u p18=%u p19=%u) HOLD_HP_HI=0x%08x\r\n", tag,
            lpHold,
            (unsigned)((lpHold >> 14) & 1U), (unsigned)((lpHold >> 15) & 1U),
            hpLo,
            (unsigned)(hpLo & 1U), (unsigned)((hpLo >> 1) & 1U),
            (unsigned)((hpLo >> 2) & 1U), (unsigned)((hpLo >> 3) & 1U), hpHi);
    kprintf("hold[%s]: LP_PAD14=0x%08x mux=%u rue=%u rde=%u ie=%u drv=%u fun=%u "
            "slp=%u/%u/%u | LP_PAD15=0x%08x mux=%u rue=%u rde=%u ie=%u\r\n", tag,
            lp14,
            (unsigned)((lp14 & FN1_LP_MUX_SEL) ? 1U : 0U),
            (unsigned)((lp14 & FN1_LP_PAD_RUE) ? 1U : 0U),
            (unsigned)((lp14 & FN1_LP_PAD_RDE) ? 1U : 0U),
            (unsigned)((lp14 & FN1_LP_FUN_IE) ? 1U : 0U),
            (unsigned)FN1_LP_PAD_DRV(lp14), (unsigned)FN1_LP_PAD_FUN(lp14),
            (unsigned)((lp14 & FN1_LP_SLP_SEL) ? 1U : 0U),
            (unsigned)((lp14 & FN1_LP_SLP_IE) ? 1U : 0U),
            (unsigned)((lp14 & FN1_LP_SLP_OE) ? 1U : 0U),
            lp15,
            (unsigned)((lp15 & FN1_LP_MUX_SEL) ? 1U : 0U),
            (unsigned)((lp15 & FN1_LP_PAD_RUE) ? 1U : 0U),
            (unsigned)((lp15 & FN1_LP_PAD_RDE) ? 1U : 0U),
            (unsigned)((lp15 & FN1_LP_FUN_IE) ? 1U : 0U));
    kprintf("hold[%s]: LP_GPIO out=0x%08x en=0x%08x | nivel LP_IN p14=%u p15=%u "
            "vs HP_IN p14=%u p15=%u\r\n", tag, lpOut, lpEn,
            (unsigned)((lpIn >> 14) & 1U), (unsigned)((lpIn >> 15) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 14) & 1U),
            (unsigned)((*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> 15) & 1U));
}

/**
 * Soltar lo que puede estar congelando los pads del slot 1: hold de 14/15 (bit
 * 14 y 15 del registro LP), hold de 16..19 (bits 0..3 del HP_LO) y la propiedad
 * LP de 14/15 (MUX_SEL).  Antes de quitar la propiedad se apaga el conductor LP
 * por la via W1TC para que el pad no quede mandado por dos bloques a la vez.
 * Los demas bits no se tocan.
 */
static void fn1HoldRelease(void)
{
    volatile uint32 *lpHold = (volatile uint32 *)(uintptr_t)FN1_LP_IOM_HOLD;
    volatile uint32 *hpLo   = (volatile uint32 *)(uintptr_t)FN1_HP_HOLD_LO;
    volatile uint32 *p14    = (volatile uint32 *)(uintptr_t)FN1_LP_IOM_PAD(14);
    volatile uint32 *p15    = (volatile uint32 *)(uintptr_t)FN1_LP_IOM_PAD(15);
    uint32 holdWas = *lpHold;
    uint32 loWas   = *hpLo;
    uint32 m14Was  = (*p14 & FN1_LP_MUX_SEL) ? 1U : 0U;
    uint32 m15Was  = (*p15 & FN1_LP_MUX_SEL) ? 1U : 0U;

    *(volatile uint32 *)(uintptr_t)FN1_LP_GPIO_EN_CLR = 3UL << 14;

    *lpHold = holdWas & ~(3UL << 14);
    *hpLo   = loWas & ~0x0FUL;
    *p14    = *p14 & ~FN1_LP_MUX_SEL;
    *p15    = *p15 & ~FN1_LP_MUX_SEL;
    delay_ms(1);

    kprintf("hold: suelto: HOLD_LP 0x%08x->0x%08x  HOLD_HP_LO 0x%08x->0x%08x  "
            "mux14 %u->%u  mux15 %u->%u\r\n", holdWas, (uint32)*lpHold, loWas,
            (uint32)*hpLo, m14Was,
            (unsigned)((*p14 & FN1_LP_MUX_SEL) ? 1U : 0U), m15Was,
            (unsigned)((*p15 & FN1_LP_MUX_SEL) ? 1U : 0U));
}

/**
 * Prueba de alambre: pad tri-estado (conductor apagado) y nivel leido con las
 * resistencias internas apagadas, luego solo con pull-up y luego solo con
 * pull-down.  Si el nivel sigue a la resistencia la red esta suelta: nadie la
 * conduce por fuera.  Si se queda clavado con las dos, la mueve un conductor
 * externo (el C6) o el pad no obedece a nada (congelado).  Se debuelve el pad y
 * el GPIO_ENABLE tal cual estaban; aqui solo se manejan pull y OE, nunca nivel.
 */
static void fn1PullTest(const char *name, uint8 pin)
{
    volatile uint32 *pad = (volatile uint32 *)(uintptr_t)FN1_GP_PAD(pin);
    volatile uint32 *out = (volatile uint32 *)(uintptr_t)FN1_GP_OUT(pin);
    uint32 padSave = *pad;
    uint32 outSave = *out;
    uint32 enWas   = (*(volatile uint32 *)(uintptr_t)FN1_GP_EN >> pin) & 1U;
    uint32 lvlOff, lvlUp, lvlDn;

    *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TC = 1UL << pin;
    *pad = (padSave & ~(FN1_PAD_WPU | FN1_PAD_WPD | FN1_PAD_MCU_MASK))
           | FN1_PAD_IE | FN1_PAD_MCU_GPIO;
    *out = outSave | FN1_OEN_SEL;                   /* OE mandado por ENABLE=0  */
    delay_ms(1);
    lvlOff = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;

    *pad = (*pad & ~FN1_PAD_WPD) | FN1_PAD_WPU;
    delay_ms(1);
    lvlUp = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;

    *pad = (*pad & ~FN1_PAD_WPU) | FN1_PAD_WPD;
    delay_ms(1);
    lvlDn = (*(volatile uint32 *)(uintptr_t)FN1_GP_IN_REG >> pin) & 1U;

    *out = outSave;
    *pad = padSave;
    if (enWas != 0U)
    {
        *(volatile uint32 *)(uintptr_t)FN1_GP_EN_W1TS = 1UL << pin;
    }
    delay_ms(1);

    kprintf("hold: %-4s p%-2u suelto=%u pullup=%u pulldown=%u -> %s\r\n", name,
            (unsigned)pin, (unsigned)lvlOff, (unsigned)lvlUp, (unsigned)lvlDn,
            (lvlUp != lvlDn)
                ? "RED SUELTA: gana nuestra resistencia, nadie conduce por fuera"
                : "nivel fijo: lo conduce un externo o el pad esta congelado");
}

int sdioPadHoldDiag(void)
{
    struct sdioinfo info;
    uint8  truth[8];
    uint32 hi, lo, i;
    bool   ok14, ok15;

    if (!sd_host_ready)
    {
        kprintf("hold: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    fn1HoldSnapshot("antes");

    kprintf("hold: conduccion y pull ANTES de tocar nada (16..19 obedecen, "
            "22 es un pad libre de esta placa, de contraste):\r\n");
    fn1DriveAndRead("D0", 14U, &hi, &lo);
    fn1PullTest("D0", 14U);
    fn1DriveAndRead("D1", 15U, &hi, &lo);
    fn1PullTest("D1", 15U);
    fn1PullTest("libre", 22U);

    fn1HoldRelease();
    fn1HoldSnapshot("despues");

    kprintf("hold: conduccion y pull DESPUES de soltar hold/mux:\r\n");
    fn1DriveAndRead("D0", 14U, &hi, &lo);
    fn1PullTest("D0", 14U);
    fn1DriveAndRead("D1", 15U, &hi, &lo);
    fn1PullTest("D1", 15U);

    /* Re-enumerar: sdioEnumerateAt llama a sdioPadsInit, que vuelve a poner los
     * seis pads del slot 1 en la matriz GPIO con sus senales; si el pad estaba
     * congelado esa escritura nunca llego, y ahora debe llegar. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_PROBE, FALSE) != OK || !info.up)
    {
        kprintf("hold: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    fn1SdioSetup(FALSE);
    delay_ms(2);

    /* Verdad por CMD52 (viaja por CMD, que si funciona) y pedimento por CMD53
     * con la entrada DAT0 puesta en el pad 14 y luego en el 15. */
    for (i = 0U; i < 8U; i++)
    {
        truth[i] = 0U;
        (void)fn1Cmd52Byte(SDIO_FN0, i, FALSE, &truth[i]);
    }
    kprintf("hold: verdad CMD52 del CCCR:");
    for (i = 0U; i < 8U; i++)
    {
        kprintf(" %02x", truth[i]);
    }
    kprintf("\r\n");

    ok14 = fn1HuntTry(14U, SDIO_FN0, 0x000U, truth, 8U);
    ok15 = fn1HuntTry(15U, SDIO_FN0, 0x000U, truth, 8U);

    if (ok14 || ok15)
    {
        kprintf("hold: EL CMD53 COINCIDE CON EL CMD52: el pad %u ya es DAT0 de "
                "verdad, el hold/mux LP era el bloqueo del transporte de datos\r\n",
                ok14 ? 14U : 15U);
    }
    else
    {
        kprintf("hold: sigue sin coincidir: hold y MUX_SEL no eran el bloqueo "
                "(o no era el unico); la fase de datos del esclavo no empieza\r\n");
    }

    (void)sdBlockReset();          /* la maquina de datos pudo quedar muerta  */
    kprintf("hold: tras sdBlockReset STATUS=0x%08x\r\n",
            (uint32)SD_REG(SDHOST_STATUS));

    sd_xfer_trace   = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* La ventana FIFO del esclavo y los modos de CMD53 (`wifi fifo`)       */
/* ==================================================================== */

/*
 * Donde estamos despues de `wifi hold`.
 *
 * El bloqueo fisico era la propiedad del pad: LP_IOMUX decide con MUX_SEL (bit 4)
 * si GPIO14 y GPIO15 los manda el GPIO de bajo poder o el GPIO digital, y tras el
 * arranque venian en 1 (medido: LP_PAD14=0x0000001a, LP_PAD15=0x0000041a).  Con
 * el pad liberado --sdioPadToMatrix ahora limpia ese bit antes de tocar el
 * IO_MUX-- el CMD53 por fin mueve bytes: `wifi pio` saca los cuatro primeros del
 * CCCR y coinciden con el CMD52, en 1-bit y en 4-bit a 20 MHz, con palabras=1 y
 * hwerr=0x00000000.  O sea que DAT0 y DAT1 estan vivos y el CIU del slot 1
 * funciona; el numero de pad y el DMA interno del host quedaron descartados.
 *
 * Lo unico que sigue mudo es TODO CMD53 dirigido a la funcion 1: la lectura de 4
 * bytes de TOKEN_RDATA (0x044), la ventana FIFO (0x1f7e6 para los 26 bytes que el
 * esclavo tiene en cola) y la ESCRITURA de 4 bytes al doorbell (0x08c).  El
 * mismo esclavo contesta a los CMD52 de esos registros, y el arg armado es
 * identico al del driver de referencia (esp_hosted_sdio.c:809): verificado sobre
 * el arg impreso, 0x94011804 = write|fn1|byte|increment|0x08c|4.  Y el propio
 * INT_RAW del C6 se queja de descriptores: tras el doorbell quedan TX_DSCR_ERR
 * (bit19) y RX_DSCR_ERR (bit20), que es el DMA interno del sdio_slave del C6
 * diciendo que no tiene colas montadas.
 *
 * Esta sonda deja de suponer y mide lo que decide el transporte:
 *   A. la ventana FIFO leida byte a byte por CMD52 --si esto sale, el transporte
 *      esp_hosted puede ir entero por CMD52 (un CMD52 por byte) y W2 deja de
 *      estar bloqueada;
 *   B. CMD53 byte-mode sobre la ventana FIFO (lo que intenta `wifi pio`);
 *   C. CMD53 en MODO BLOQUE (bit27=1, BLKSIZ=512) sobre la ventana --si el
 *      esclavo solo implementa transfers de bloque, aqui se ve--;
 *   D. CMD53 byte-mode sobre un registro de fn1 (control, debe fallar igual);
 *   E. CMD53 de ESCRITURA byte-mode sobre la ventana (el sentido H2C).
 *
 * Y en la segunda vuelta, ya con B y C en verde, lo que queda por medir:
 *   G. byte-mode con la longitud EXACTA del paquete (26, no multiplo de 4), que
 *      es justo como `wifi pio` lo pedia y como fallaba;
 *   H. byte-mode con la longitud redondeada a palabra, decodificando la
 *      cabecera esp_hosted (if_type/len/offset) para saber si es trama real;
 *   I. la escritura como la hace la referencia: MODO BLOQUE, 512 bytes, en
 *      END-frame_len con la trama delante y ceros detras;
 *   J/K. la reaccion del esclavo y los buffers H2S que anuncia TOKEN_RDATA.
 *
 * Cada intento imprime los primeros bytes sacados para poder reconocer una
 * cabecera esp_hosted (len, offset=16, canal) en vez de ceros.
 *
 * Seguridad: no se toca ningun pad ni ningun registro de reloj; todo lo que se
 * escribe en el esclavo son doorbells y la ventana de datos, por CMD.  Termina
 * con sdBlockReset() (una transfer fallida deja la maquina de datos torcida) y
 * fn1RestoreSlot0(), igual que las otras sondas, para dejar la microSD viva.
 */

/** CMD53 en PIO con el modo de bloque elegible (arriba fn1Cmd53PioOnce siempre
 *  byte-mode).  \p bytcnt va a BYTCNT y \p blksiz a BLKSIZ.
 *
 *  El campo count del arg NO es lo mismo en los dos modos y ahi estaba el error
 *  de esta sonda: en byte-mode son bytes (0x1FF de maximo, 0 = 512) y en
 *  modo bloque son BLOQUES (la referencia: count = len / 512, esp_hosted_sdio.c:
 *  817).  Poner 0 en modo bloque el esclavo lo lee como 512 bloques = 262144
 *  bytes y entonces la escritura jamas se da por terminada. */
static bool fn1PioRaw(uint8 fn, uint32 addr, bool write, uint8 *buf,
                      uint32 bytcnt, uint32 blksiz, bool blockMode)
{
    uint32 count;
    uint32 arg;

    if (blockMode)
    {
        count = bytcnt / FN1_BLOCK_SIZE;
    }
    else
    {
        count = (bytcnt == FN1_BLOCK_SIZE) ? 0U : (bytcnt & 0x1FFU);
    }

    arg = ((uint32)write << 31) | ((uint32)fn << 28)
          | (blockMode ? (1U << 27) : 0U) | (1U << 26)
          | ((addr & 0x1FFFFUL) << 9) | (count & 0x1FFU);

    return sdTransferPio(SDIO_SLOT, SDIO_CMD53, arg, buf, bytcnt, blksiz, write);
}

/** Volcado corto de una lectura: los n primeros bytes, sin pasar de 16. */
static void fn1DumpSome(const char *tag, const uint8 *b, uint32 n)
{
    uint32 i;

    kprintf("%s", tag);
    for (i = 0U; i < n && i < 16U; i++)
    {
        kprintf(" %02x", b[i]);
    }
    kprintf("%s\r\n", (n > 16U) ? " ..." : "");
}

/** Un intento aislado: lo ejecuta, lo imprime con su hwerr y, si fallo, limpia
 *  la maquina de datos y reenumera el C6 con el mismo divisor.  Hace falta
 *  aislarlos porque un CMD53 que no completo su fase de datos envenena al
 *  siguiente: en la primera vuelta el control sobre fn0 (test F) salio FALLO
 *  solo por el fallo de escritura que tenia delante. */
static bool fifoTry(struct sdioinfo *info, const char *tag, uint8 fn,
                    uint32 addr, bool write, uint8 *buf, uint32 bytcnt,
                    uint32 blksiz, bool blockMode, uint32 div)
{
    bool   ok;
    uint32 hw;

    ok = fn1PioRaw(fn, addr, write, buf, bytcnt, blksiz, blockMode);
    hw = sd_hwerr;
    kprintf("fifo: %s fn%u %s %uB%s @0x%08x -> %s  hw=0x%08x", tag,
            (unsigned)fn, write ? "ESCRITURA" : "lectura ", (unsigned)bytcnt,
            blockMode ? " (bloque)" : "", addr, ok ? "OK" : "FALLO", hw);
    if (!write)
    {
        fn1DumpSome(ok ? " |" : " |vacio", buf, 16U);
    }
    else
    {
        kprintf("\r\n");
    }

    if (!ok)
    {
        (void)sdBlockReset();
        if (sdioEnumerateAt(info, div, FALSE) != OK || !info->up)
        {
            kprintf("fifo:   tras el fallo el C6 no reenumero (hw=0x%08x)\r\n",
                    sd_hwerr);
        }
        fn1SdioSetup(FALSE);
        fn1DisarmInterrupt();
        delay_ms(2);
    }
    return ok;
}

int sdioFifoWindowDiag(void)
{
    struct sdioinfo info;
    uint8  fifo[64];
    uint8  blk[FN1_BLOCK_SIZE];
    uint8  nudge;
    uint32 pktLen, window, i, raw, token, len4, round;
    bool   ok;
    bool   f0w, f1wb, f1w1, f1w2;

    if (!sd_host_ready)
    {
        kprintf("fifo: el host SDMMC no esta levantado\r\n");
        return SYSERR;
    }

    sdmmcBusLock();

    /* 20 MHz: el reloj donde el CMD53 de fn0 ya esta probado que funciona. */
    if (sdioEnumerateAt(&info, SD_CARD_DIV_WORK, FALSE) != OK || !info.up)
    {
        kprintf("fifo: el C6 no enumero (hwerr=0x%08x)\r\n", sd_hwerr);
        sdmmcBusUnlock();
        fn1RestoreSlot0();
        return SYSERR;
    }
    fn1SdioSetup(FALSE);                  /* 1-bit: la interrupcion no ocupa DAT1 */
    fn1DisarmInterrupt();
    delay_ms(2);

    /* Provocar un paquete C2H: el mismo doorbell que usan las otras sondas. */
    nudge = (uint8)(1U << HOST_INTR_OPEN_DATA);
    if (!fn1Cmd52Byte(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, &nudge))
    {
        kprintf("fifo: doorbell por CMD52 fallo (hwerr=0x%08x)\r\n", sd_hwerr);
    }
    delay_ms(50);
    pktLen = 0;
    (void)fn1ReadReg(FN1_PACKET_LEN, &pktLen);
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    pktLen &= 0x000FFFFFUL;               /* [19:0] longitud acumulada          */
    kprintf("fifo: PACKET_LEN=%u  INT_RAW=0x%08x (new_packet=%u tx_dscr_err=%u "
            "rx_dscr_err=%u)\r\n", (unsigned)pktLen, raw,
            (unsigned)((raw & INT_NEW_PACKET) ? 1U : 0U),
            (unsigned)((raw & (1U << 19)) ? 1U : 0U),
            (unsigned)((raw & (1U << 20)) ? 1U : 0U));

    if (pktLen == 0U || pktLen > 0x30U)
    {
        kprintf("fifo: sin paquete pequeno que leer (o demasiado grande); "
                "la ventana FIFO no se puede probar con esta longitud\r\n");
    }
    window = FN1_FIFO_END - pktLen;

    /* A. ventana FIFO byte a byte por CMD52: el camino que si esta probado. */
    for (i = 0U; i < pktLen && i < 32U; i++)
    {
        fifo[i] = 0xEEU;
        (void)fn1Cmd52Byte(SDIO_FN1, window + i, FALSE, &fifo[i]);
    }
    fn1DumpSome("fifo: A CMD52 byte a byte @0x1f...", fifo,
                (pktLen < 32U) ? pktLen : 32U);
    kprintf("fifo:   (direccion de la ventana = 0x%08x = 0x%x - %u)\r\n",
            window, (unsigned)FN1_FIFO_END, (unsigned)pktLen);

    /* B. CMD53 byte-mode sobre la ventana, 4 bytes. */
    memset(fifo, 0, sizeof(fifo));
    ok = fn1PioRaw(SDIO_FN1, window, FALSE, fifo, 4U, 4U, FALSE);
    kprintf("fifo: B CMD53 byte-mode 4B @0x%08x -> %s", window,
            ok ? "OK" : "FALLO");
    fn1DumpSome("", fifo, 4U);
    kprintf("fifo:   hw=0x%08x STATUS=0x%08x\r\n", sd_hwerr,
            (uint32)SD_REG(SDHOST_STATUS));

    /* C. CMD53 en modo bloque: bloque completo de 512 Terminando en 0x1f800. */
    memset(blk, 0, sizeof(blk));
    ok = fn1PioRaw(SDIO_FN1, FN1_FIFO_END - FN1_BLOCK_SIZE, FALSE, blk,
                   FN1_BLOCK_SIZE, FN1_BLOCK_SIZE, TRUE);
    kprintf("fifo: C CMD53 BLOCK-mode 512B @0x%08x -> %s",
            (unsigned)(FN1_FIFO_END - FN1_BLOCK_SIZE), ok ? "OK" : "FALLO");
    fn1DumpSome("", blk, 16U);
    kprintf("fifo:   hw=0x%08x STATUS=0x%08x fifo_count=%u\r\n", sd_hwerr,
            (uint32)SD_REG(SDHOST_STATUS), (uint32)(SD_REG(SDHOST_STATUS) >> 16));

    /* D. control: CMD53 byte-mode sobre un registro de fn1.  La referencia
     * tampoco lo consigue: read_reg_bytes (esp_hosted_sdio.c:969) intenta el
     * CMD53 y, si falla, lee byte a byte por CMD52. */
    memset(fifo, 0, sizeof(fifo));
    (void)fifoTry(&info, "D registro fn1   ", SDIO_FN1, FN1_PACKET_LEN, FALSE,
                  fifo, 4U, 4U, FALSE, SD_CARD_DIV_WORK);

    /* E. los dos controles sobre fn0, con el bus sano: leer el CCCR y
     * ESCRIBIR el tamano de bloque de fn0 con el valor que ya tiene (512 ->
     * 0x00 0x02), o sea sin cambiar nada.  Este es el discriminante de todo lo
     * que queda: si la escritura CMD53 sobre fn0 responde y la de fn1 no, el
     * host y los pads estan bien y lo que falla es la cola de recepcion del
     * esclavo; si fn0 tambien falla, la direccion de escritura del slot 1 esta
     * rota (matriz/OE del pad) y hay que mirar de ese lado. */
    memset(fifo, 0, sizeof(fifo));
    (void)fifoTry(&info, "E1 fn0 lectura   ", SDIO_FN0, 0x000U, FALSE, fifo,
                  4U, 4U, FALSE, SD_CARD_DIV_WORK);
    fifo[0] = 0x00U;                       /* BLKSIZEL  (0x010) = 0x00         */
    fifo[1] = 0x02U;                       /* BLKSIZEH  (0x011) = 0x02 -> 512  */
    fifo[2] = 0x00U;
    fifo[3] = 0x00U;
    f0w = fifoTry(&info, "E2 fn0 escritura ", SDIO_FN0, 0x010U, TRUE, fifo,
                  4U, 4U, FALSE, SD_CARD_DIV_WORK);

    /* F. la ventana en byte-mode con las longitudes que importan, aisladas:
     * 26 exactos (lo que pedía `wifi pio`), redondeado a palabra y 4 sueltos.
     * Cada lectura saca paquetes de la cola del esclavo, asi que la longitud
     * se vuelve a leer del propio PACKET_LEN antes de cada intento. */
    for (round = 0U; round < 3U; round++)
    {
        pktLen = 0U;
        (void)fn1ReadReg(FN1_PACKET_LEN, &pktLen);
        pktLen &= 0x000FFFFFUL;
        if (pktLen == 0U || pktLen > sizeof(fifo))
        {
            kprintf("fifo: F%d omitida: PACKET_LEN=%u (sin paquete pequeno que "
                    "leer; pruebo doorbell)\r\n", round + 1, (unsigned)pktLen);
            nudge = (uint8)(1U << HOST_INTR_OPEN_DATA);
            (void)fn1Cmd52Byte(SDIO_FN1, FN1_HOST_TO_SLAVE, TRUE, &nudge);
            delay_ms(50);
            continue;
        }
        len4 = (pktLen + 3U) & ~3U;
        if (len4 > sizeof(fifo))
        {
            len4 = sizeof(fifo);
        }
        memset(fifo, 0, sizeof(fifo));
        (void)fifoTry(&info, "F1 ventana exacta", SDIO_FN1, FN1_FIFO_END - pktLen,
                      FALSE, fifo, pktLen, pktLen, FALSE, SD_CARD_DIV_WORK);

        memset(fifo, 0, sizeof(fifo));
        (void)fifoTry(&info, "F2 ventana a4  ", SDIO_FN1, FN1_FIFO_END - len4,
                      FALSE, fifo, len4, len4, FALSE, SD_CARD_DIV_WORK);
        kprintf("fifo:   cabecera: if=%u ifnum=%u flags=0x%02x len=%u offset=%u"
                " hw=0x%08x\r\n", (unsigned)(fifo[0] & 0x0FU),
                (unsigned)(fifo[0] >> 4), fifo[1],
                (unsigned)((uint16)fifo[2] | ((uint16)fifo[3] << 8)),
                (unsigned)((uint16)fifo[4] | ((uint16)fifo[5] << 8)), sd_hwerr);

        memset(fifo, 0, sizeof(fifo));
        (void)fifoTry(&info, "F3 ventana 4B  ", SDIO_FN1, FN1_FIFO_END - 4U,
                      FALSE, fifo, 4U, 4U, FALSE, SD_CARD_DIV_WORK);
        break;                     /* una ronda basta: los paquetes se agotan */
    }

    /* G. las escrituras sobre fn1, ahora si con el campo count correcto en modo
     * bloque (1 bloque, no 0 = 512 bloques).  Cuatro variantes: byte-mode en la
     * ventana, bloque dentro de la ventana (0x1f600, sin cruzar el final),
     * bloque en END-frame_len (la direccion exacta de la referencia) y byte-
     * mode sobre un registro (INT_CLR, lo que write_reg_bytes intenta).  La
     * trama es cabecera real (PRIV, 14 bytes) con patron detras y ceros hasta
     * el bloque: no es una RPC valida, aqui solo se mide la fase de datos. */
    for (i = 0U; i < FN1_BLOCK_SIZE; i++)
    {
        blk[i] = 0U;
    }
    blk[0] = 0x05U;                        /* if_type=PRIV(5), if_num=0        */
    blk[2] = 0x0EU;                        /* len=14                            */
    blk[4] = 0x0CU;                        /* offset=12 = sizeof(cabecera)      */
    blk[8] = 0x01U;                        /* seq_num=1                         */
    for (i = 12U; i < 26U; i++)
    {
        blk[i] = (uint8)(0xA0U + i);
    }
    for (i = 0U; i < 4U; i++)
    {
        fifo[i] = (uint8)(0xA0U + i);
    }
    f1wb = fifoTry(&info, "G1 byte en ventana", SDIO_FN1, window, TRUE, fifo,
                   4U, 4U, FALSE, SD_CARD_DIV_WORK);
    f1w1 = fifoTry(&info, "G2 bloque 0x1f600", SDIO_FN1,
                   FN1_FIFO_END - FN1_BLOCK_SIZE, TRUE, blk,
                   FN1_BLOCK_SIZE, FN1_BLOCK_SIZE, TRUE, SD_CARD_DIV_WORK);
    f1w2 = fifoTry(&info, "G3 bloque END-26", SDIO_FN1, FN1_FIFO_END - 26U,
                   TRUE, blk, FN1_BLOCK_SIZE, FN1_BLOCK_SIZE, TRUE,
                   SD_CARD_DIV_WORK);
    (void)fifoTry(&info, "G4 byte INT_CLR  ", SDIO_FN1, FN1_INT_CLR, TRUE,
                  fifo, 4U, 4U, FALSE, SD_CARD_DIV_WORK);

    /* H. la misma escritura a reloj de arranque (~400 kHz): si aqui pasa y a
     * 20 MHz no, lo que sobra es el tiempo de linea. */
    (void)fifoTry(&info, "H  bloque lento  ", SDIO_FN1,
                  FN1_FIFO_END - FN1_BLOCK_SIZE, TRUE, blk, FN1_BLOCK_SIZE,
                  FN1_BLOCK_SIZE, TRUE, SD_CARD_DIV_PROBE);

    /* I. reaccion del esclavo: INT_RAW, la longitud pendiente y los buffers
     * H2S que anuncia TOKEN_RDATA (0x044) bits [27:16]; la referencia lee ese
     * numero antes de escribir (esp_hosted_sdio.c:1070) y con 0 no manda nada.
     */
    pktLen = 0U;
    raw = 0U;
    token = 0U;
    (void)fn1ReadReg(FN1_PACKET_LEN, &pktLen);
    (void)fn1ReadReg(FN1_INT_RAW, &raw);
    (void)fn1ReadReg(FN1_TOKEN_RDATA, &token);
    pktLen &= 0x000FFFFFUL;
    kprintf("fifo: I PACKET_LEN=%u INT_RAW=0x%08x (new_packet=%u tohost=%u "
            "tx_done=%u rd_ack=%u) TOKEN=0x%08x buffers libres=0x%03x\r\n",
            (unsigned)pktLen, raw,
            (unsigned)((raw & INT_NEW_PACKET) ? 1U : 0U),
            (unsigned)((raw & (1U << 18)) ? 1U : 0U),
            (unsigned)((raw & (1U << 14)) ? 1U : 0U),
            (unsigned)((raw & (1U << 22)) ? 1U : 0U), token,
            (unsigned)((token >> 16) & 0xFFFU));

    if (f0w && !f1w1 && !f1w2)
    {
        kprintf("fifo: veredicto: el host y los pads escriben bien (fn0 SI) y "
                "la funcion 1 del C6 no acepta ninguna escritura CMD53: la cola "
                "de recepcion del esclavo esta vacia o su firmware no la monta.\r\n");
    }
    else if (!f0w)
    {
        kprintf("fifo: veredicto: ni fn0 contesta la escritura CMD53: la "
                "direccion de escritura del slot 1 (matriz/OE del pad o el CIU) "
                "es la que falla, no el esclavo.\r\n");
    }
    else
    {
        kprintf("fifo: veredicto: la escritura CMD53 sobre fn1 SI pasa (%s%s%s): "
                "el transporte H2C queda desbloqueado.\r\n",
                f1wb ? "byte-ventana " : "", f1w1 ? "bloque-1f600 " : "",
                f1w2 ? "bloque-END-26 " : "");
    }

    (void)sdBlockReset();
    kprintf("fifo: tras sdBlockReset STATUS=0x%08x\r\n",
            (uint32)SD_REG(SDHOST_STATUS));

    sd_xfer_trace   = 0U;
    sd_xfer_no_wait = 0U;

    sdmmcBusUnlock();
    fn1RestoreSlot0();
    return OK;
}

/* ==================================================================== */
/* Devolver el bloque a manos de la microSD (`wifi up` si falla)        */
/* ==================================================================== */

/**
 * Exportacion de fn1RestoreSlot0() para el shell: el attach de esp_hosted
 * comparte el bloque SDHOST con la microSD y, si aborta con la maquina de
 * datos torcida (CMD_START clavado por el data_busy del C6), el candado se
 * suelta pero el motor queda inservible y `ls` deja de listar.  Este envoltorio
 * hace la misma recuperacion que las sondas --reset de protocolo, divisor
 * global de vuelta a 40 MHz, pads del slot 0, sdControllerInit + sdIdentify y
 * lectura real del LBA 0-- y retiene el C6 en reset mientras; el proximo
 * `wifi up` le vuelve a pulsar el CHIP_PU (setup_reset_pin), asi que no deja al
 * coprocesador apagado de forma permanente.
 */
int sdioRestoreSlot0(void)
{
    fn1RestoreSlot0();
    return OK;
}
