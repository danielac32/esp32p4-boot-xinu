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

/**
 * Mover \p len (1..512) bytes con la funcion \p fn en modo byte (op_code=1,
 * block_mode=0) desde/hacia \p addr.  El argumento CMD53:
 * [31]=write [30:28]=fn [27]=block_mode [26]=op_code [25:9]=addr [8:0]=count.
 * En modo byte un count de 0 significa 512.
 */
static bool fn1Cmd53Bytes(uint8 fn, uint32 addr, bool write, uint8 *data,
                          uint32 len)
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
        sdBlockCopyIn(data, len);
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
        sdBlockCopyOut(data, len);
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
