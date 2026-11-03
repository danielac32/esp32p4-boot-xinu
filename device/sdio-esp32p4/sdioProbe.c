/**
 * @file sdioProbe.c
 *
 * Sonda de enumeración SDIO del ESP32-C6 (WiFi6) sobre el SLOT 1 del
 * controlador SDMMC compartido.  W1: confirmar que el enlace físico y la
 * capa de comandos funcionan con el slave esp_hosted que ya corre en el C6.
 *
 * El C6 cuelga del MISMO bloque DesignWare que la microSD (slot 0), así que
 * aquí no se reinicializa el controlador: se reutiliza el motor de comandos
 * del host (sdSendCmdSlot/sdSetClockSlot/sdHostRegBase en sdHw.c) y cada
 * transacción va envuelta en el candado compartido del kernel
 * (sdmmcBusLock/Unlock) para no intercalar con la microSD.
 *
 * A diferencia del slot 0, los pads del slot 1 (GPIO14..19) NO son función
 * fija del IO_MUX: hay que enrutarlos por la matriz GPIO.  Cada pad se pone
 * en MCU_SEL=GPIO y se conecta su señal por gpio_matrix_out (salida, con el
 * índice de señal como función) y gpio_matrix_in (entrada, que selecciona el
 * pin dentro del registro de la señal).  Las índices de señal del slot 1 los
 * publica soc/gpio_sig_map.h: CLK=0, CMD=1, D0=2, D1=3, D2=4, D3=5.
 *
 * Todos los valores que se imprimen se LEEN del propio periférico (CMD52 de
 * lectura sobre el CCCR y el CMD5 de OCR), no de un caché del driver: eso es
 * lo que convierte esta sonda en una prueba real del bus.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <timer.h>                      /* delay_ms                          */

#include <gpio.h>                       /* gpio_matrix_out/in, IO_MUX_*      */
#include <sdio.h>                       /* struct sdioinfo + prototypes      */
#include <device/sdmmc-esp32p4/sdhost.h> /* sdSendCmdSlot/sdSetClockSlot/... */

/* ------------------------------------------------------------------ */
/* Slot 1: el C6                                                        */
/* ------------------------------------------------------------------ */
#define SDIO_SLOT           1U

/* Índices de señal del SDMMC slot 1 por la matriz GPIO (gpio_sig_map.h).
 * El reloj sólo sale; CMD y DAT0..3 son bidireccionales. */
#define SDIO_SIG_CLK        0U
#define SDIO_SIG_CMD        1U
#define SDIO_SIG_D0         2U
#define SDIO_SIG_D1         3U
#define SDIO_SIG_D2         4U
#define SDIO_SIG_D3         5U

/* Offsets del bloque SDHOST que el slot 1 necesita tocar.  El mapa de
 * registros vive en sdhost.h (privado del host); aquí sólo se nombran los
 * campos por-slot que el host no parametriza, accediendo vía sdHostRegBase. */
#define H_CLKENA            0x010U
#define H_CTYPE             0x018U
#define H_CLKDIV            0x008U
#define H_CLKSRC            0x00CU
#define H_RINTSTS           0x044U
#define H_STATUS            0x048U

/* Registro de selección de ENTRADA por la matriz: FUNC_IN_SEL_CFG[s] está en
 * GPIO_BASE + 0x158 + 4*s (s = índice de señal).  Campos: IN_SEL[5:0],
 * INV[6], SIG_IN_SEL[7] (1 = la señal se toma de la matriz, no del IOMUX). */
#define GPIO_IN_SEL_CFG(sig)    (GPIO_BASE + 0x158UL + 4UL * (sig))
#define O_SEL_MASK              0x1FFU          /* OUT_SEL[8:0]          */
#define O_OEN_SEL               (1UL << 10)     /* OEN_SEL: 1 = GPIO_ENABLE */
#define I_SEL_MASK              0x3FU           /* IN_SEL[5:0]           */
#define I_SIG_IN_SEL            (1UL << 7)      /* SIG_IN_SEL            */
#define P_IE                    (1UL << 9)      /* fun_ie del pad         */
#define P_MCU_SEL(s)            (((s) >> 12) & 0x7U)  /* 1 = matriz GPIO  */

#define F_CLKENA_CARD1      (1UL << SDIO_SLOT)        /*CLKENA[1]            */
#define F_CLKENA_LP1        (1UL << (16 + SDIO_SLOT)) /*CLKENA[17]           */
#define F_CTYPE_W4_CARD1    (1UL << SDIO_SLOT)        /*CTYPE[1]            */
#define F_CLKDIV_CARD1_POS  (8 * SDIO_SLOT)           /*CLKDIV[15:8]        */

/* Comandos SDIO (distintos de los de la tarjeta SD del slot 0). */
#define SDIO_CMD0_GO_IDLE       0U
#define SDIO_CMD3_SET_RCA       3U
#define SDIO_CMD5_IO_SEND       5U
#define SDIO_CMD7_SELECT        7U
#define SDIO_CMD52_RDWR_DIRECT  52U
#define SDIO_CMD53_RDWR_BLOCK   53U

/* OCR/condición de E/S del CMD5 (R4): bit 31 = la interfaz está lista. */
#define SDIO_CMD5_ARG           0x00FF8000UL
#define SDIO_R4_IO_READY        (1UL << 31)

/* Registros CCCR (función 0, espacio de direcciones bajo, raw=0). */
#define CCCR_IO_ENABLE          0x02U   /* bitmap de funciones habilitadas  */
#define CCCR_IO_READY           0x03U   /* bitmap de funciones listas       */
#define CCCR_CARD_INFO          0x08U   /* versión SDIO                     */
#define CCCR_FN0_INFO           0x0CU   /* 8 bytes: región de identificación */
#define CCCR_BLKSIZE_L          0x10U   /* tamaño de bloque de fn0 (bajo)    */
#define CCCR_BLKSIZE_H          0x11U   /* tamaño de bloque de fn0 (alto)    */
#define CCCR_BUS_IF_CONTROL     0x07U   /* 0=1-bit, 2=4-bit                 */

#define CCCR_FN1_EN             (1U << 1)   /* bit de la función 1          */

/* ------------------------------------------------------------------ */
/* Estado de la sonda (struct sdioinfo vive en <sdio.h>)               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Pads del slot 1 por la matriz GPIO                                   */
/* ------------------------------------------------------------------ */

/**
 * Dejar un pad del C6 en la matriz GPIO: MCU_SEL=GPIO, entrada habilitada
 * y 40 mA (igual que el slot 0 nativo, salvo el enrutado).  CMD y DAT llevan
 * pull-up para que un bus en reposo lea alto; CLK no.
 */
static void sdioPadToMatrix(uint8 pin, bool pullup)
{
    volatile uint32 *pad = (volatile uint32 *)IO_MUX_PIN_REG(IO_MUX_BASE, pin);
    uint32 val = *pad;

    val &= ~(IO_MCU_SEL_MASK | IO_FUN_WPD | IO_FUN_WPU | IO_FUN_DRV_MASK);
    val |= IO_MCU_SEL_GPIO | IO_FUN_IE | IO_FUN_DRV_40MA;
    if (pullup)
    {
        val |= IO_FUN_WPU;
    }
    *pad = val;
}

/**
 * Enrutar una señal del slot 1 a su pad por la matriz GPIO: salida por
 * gpio_matrix_out (la función del pad = índice de señal) e, salvo el reloj,
 * entrada por gpio_matrix_in (el registro de la señal apunta al pin).
 */
static void sdioRoute(uint8 pin, uint32 sig, bool isClk, bool pullup)
{
    sdioPadToMatrix(pin, pullup);
    gpio_matrix_out(pin, sig, FALSE, FALSE);
    if (!isClk)
    {
        /* OEN_SEL=0 en gpio_matrix_out: la salida la gobierna el SDMMC. */
        gpio_matrix_in(pin, sig, FALSE);
    }
}

/** Conectar los seis pads del C6 al slot 1 por la matriz GPIO. */
static void sdioPadsInit(void)
{
    sdioRoute(C6_SDIO_CLK, SDIO_SIG_CLK, TRUE,  FALSE);
    sdioRoute(C6_SDIO_CMD, SDIO_SIG_CMD, FALSE, TRUE);
    sdioRoute(C6_SDIO_D0,  SDIO_SIG_D0,  FALSE, TRUE);
    sdioRoute(C6_SDIO_D1,  SDIO_SIG_D1,  FALSE, TRUE);
    sdioRoute(C6_SDIO_D2,  SDIO_SIG_D2,  FALSE, TRUE);
    sdioRoute(C6_SDIO_D3,  SDIO_SIG_D3,  FALSE, TRUE);
}

/* ------------------------------------------------------------------ */
/* Reloj del slot 1                                                     */
/* ------------------------------------------------------------------ */

/**
 * Poner el slot 1 al divisor \p card_div dentro de la VENTANA DE REPOSO: el
 * unico instante en que este controlador admite un cambio de reloj es antes de
 * que el C6 afirme DAT0, y ese instante es justo despues del reset y antes del
 * CMD5 (medido: data_busy pasa a 1 en el propio CMD5 y ya no baja).  Con
 * data_busy=1 el comando interno "update clock" no se completa --ni siquiera
 * con wait_complete=0 ni con el reloj puesto--, el CMD_START queda clavado y
 * enqista el motor de comandos del BLOQUE, compartido con la microSD: a partir
 * de ahi el slot 0 tampoco responde.
 */
static bool sdioClockAt(uint8_t card_div)
{
    if (sdSetClockSlot(SDIO_SLOT, card_div))
    {
        return TRUE;
    }
    sdBusState("clock at fallo", SDIO_SLOT);
    return sdSetClockSlotNoWait(SDIO_SLOT, card_div);
}

/* ------------------------------------------------------------------ */
/* CMD52: acceso directo a un byte del CCCR                             */
/* ------------------------------------------------------------------ */

/**
 * Leer o escribir un byte en la función \p fn del espacio bajo (raw=0).
 * Codificación del argumento: [31]=write, [28]=fn, [27]=raw, [25:9]=reg,
 * [7:0]=dato.  La respuesta R5 trae el byte leído en [7:0].
 */
static bool sdioCmd52(uint8 fn, uint32 reg, bool write, uint8 *data)
{
    uint32 arg = ((uint32)write << 31) | ((uint32)fn << 28)
                 | ((reg & 0x1FFFFUL) << 9) | (uint32)*data;
    uint32 resp = 0;

    if (!sdSendCmdSlot(SDIO_SLOT, SDIO_CMD52_RDWR_DIRECT, arg, 1, TRUE,
                       FALSE, &resp))
    {
        return FALSE;
    }
    *data = (uint8)(resp & 0xFFU);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Secuencia de enumeración                                             */
/* ------------------------------------------------------------------ */

/** Pulso de reset sobre C6_RESET (activo por bajo, CHIP_PU). */
static void sdioResetC6(void)
{
    gpio_set_dir(C6_RESET, GPIO_OUTPUT);
    gpio_write(C6_RESET, TRUE);
    delay_ms(5);
    gpio_write(C6_RESET, FALSE);          /* mantener en reset              */
    delay_ms(100);
    gpio_write(C6_RESET, TRUE);           /* liberar: el slave arranca      */
    delay_ms(1500);                       /* margen para que el firmware
                                             esp_hosted del C6 suba su SDIO.
                                             El driver de referencia espera
                                             1500 ms tras el reset: la funcion
                                             1 (la aplicacion esp_hosted) tarda
                                             mas en quedar lista que el propio
                                             slave SDIO, y unos pocos cientos
                                             de ms hacen que fn1 no aparezca
                                             nunca dentro de la ventana. */
}

/**
 * Dejar al C6 retenido en reset (CHIP_PU abajo) y salir.  Sirve para el
 * diagnostico: mientras el slave esta en reset sus pads del slot 1 no conducen
 * nada, asi que el DAT0 que afirma cuando tiene una interrupcion pendiente deja
 * de molestar al camino de datos COMPARTIDO del bloque --que es lo que se
 * sospecha que inhibe al slot 0--.  La siguiente pasada de la sonda vuelve a
 * pulsar el reset con sdioResetC6, asi que retenerlo no pierde nada.
 */
void sdioC6HoldReset(void)
{
    gpio_set_dir(C6_RESET, GPIO_OUTPUT);
    gpio_write(C6_RESET, FALSE);
    delay_ms(2);
}

/**
 * CMD0 (con SEND_INIT: los >=74 relojes de reinicio del slave) + CMD5 hasta
 * que el C6 declare su interfaz lista (R4[31]).  El CMD5 con arg 0x00FF8000
 * pide OCR a 2.0-3.6 V y no exige CRC.  El slave puede tardar en responder
 * mientras arranca, así que se sondea durante todo el margen sin abortar al
 * primer error: un RTO aquí es "aún no contesta", no "fallo de bus".
 */
static bool sdioGoIdleReady(uint32 *ocr)
{
    uint32 t0;
    uint32 resp = 0;
    uint8  tries;

    for (tries = 0; tries < 5; tries++)
    {
        if (sdSendCmdSlot(SDIO_SLOT, SDIO_CMD0_GO_IDLE, 0, 0, FALSE, TRUE,
                          NULL))
        {
            break;
        }
        delay_ms(2);
    }

    /* CMD5 con arg 0: lectura de OCR sin pedir voltaje (sonda de presencia). */
    (void)sdSendCmdSlot(SDIO_SLOT, SDIO_CMD5_IO_SEND, 0, 1, FALSE, FALSE,
                        &resp);

    t0 = sdNow();
    do
    {
        if (sdSendCmdSlot(SDIO_SLOT, SDIO_CMD5_IO_SEND, SDIO_CMD5_ARG, 1,
                          FALSE, FALSE, &resp)
            && (resp & SDIO_R4_IO_READY) != 0)
        {
            *ocr = resp;
            return TRUE;
        }
        delay_ms(1);
    } while (!sdTimedOut(t0, 1000));
    return FALSE;
}

/** CMD3 asigna la RCA (respuesta R5 [31:16]); CMD7 selecciona el dispositivo. */
static bool sdioSetRcaSelect(uint32 *rca)
{
    uint32 resp = 0;

    if (!sdSendCmdSlot(SDIO_SLOT, SDIO_CMD3_SET_RCA, 0, 1, TRUE, FALSE, &resp))
    {
        return FALSE;
    }
    *rca = (resp >> 16) & 0xFFFFUL;
    if (*rca == 0)
    {
        return FALSE;
    }
    return sdSendCmdSlot(SDIO_SLOT, SDIO_CMD7_SELECT, (*rca << 16), 1, TRUE,
                         FALSE, NULL);
}

/** Habilitar la función 1 (esp_hosted WLAN) en el CCCR y esperar su ready. */
static bool sdioEnableFn1(uint8 *cio_enable, uint8 *cio_ready)
{
    uint8 val;
    uint32 t0;

    val = 0;
    if (!sdioCmd52(0, CCCR_IO_ENABLE, FALSE, &val))
    {
        return FALSE;
    }
    val |= CCCR_FN1_EN;
    if (!sdioCmd52(0, CCCR_IO_ENABLE, TRUE, &val))
    {
        return FALSE;
    }
    val = 0;
    (void)sdioCmd52(0, CCCR_IO_ENABLE, FALSE, &val);
    *cio_enable = val;

    val = 0;
    t0 = sdNow();
    do
    {
        if (sdioCmd52(0, CCCR_IO_READY, FALSE, &val) && (val & CCCR_FN1_EN))
        {
            *cio_ready = val;
            return TRUE;
        }
        delay_ms(10);
    } while (!sdTimedOut(t0, 2500));
    *cio_ready = val;
    return FALSE;
}

/** Leer el CCCR informativo: versión SDIO (0x08) y la región fn0 + tamaño de bloque. */
static void sdioReadIds(struct sdioinfo *info)
{
    uint8 i;
    uint8 b;
    uint8 lo, hi;

    b = 0;
    (void)sdioCmd52(0, CCCR_CARD_INFO, FALSE, &b);
    info->sdio_ver = b;

    for (i = 0; i < 8; i++)
    {
        b = 0;
        (void)sdioCmd52(0, (uint32)(CCCR_FN0_INFO + i), FALSE, &b);
        info->fn0_info[i] = b;
    }

    lo = 0;
    hi = 0;
    (void)sdioCmd52(0, CCCR_BLKSIZE_L, FALSE, &lo);
    (void)sdioCmd52(0, CCCR_BLKSIZE_H, FALSE, &hi);
    info->blksize = (uint16)lo | ((uint16)hi << 8);
}

/**
 * Enumerar el C6 en el slot 1.  Devuelve OK sólo si el dispositivo respondió
 * y quedó seleccionado; rellena \p out con los valores leídos del periférico.
 * El candado del bus lo toma/suelta esta rutina: toda la secuencia es una
 * sola transacción lógica y la microSD no debe intercalar.
 */
/**
 * Enumerar el C6 en el slot 1 con el reloj de identification de 400 kHz (W1).
 */
int sdioEnumerate(struct sdioinfo *out)
{
    return sdioEnumerateAt(out, SD_CARD_DIV_PROBE, TRUE);
}

/**
 * Enumerar el C6 en el slot 1 poniendo el reloj del slot AL DIVISOR
 * \p card_div justo despues del reset, es decir dentro de la ventana de reposo
 * en la que el controlador todavia admite un "update clock" (ver
 * sdioClockAt).  El reloj ya no se mueve en toda la sesion: el driver de
 * referencia hace lo mismo (fija el reloj de trabajo en el attach).
 *
 * Con \p trace se vuelca el estado del bus paso a paso; la sonda W2 lo apaga
 * cuando re-intenta a 400 kHz para no duplicar el volcado.
 *
 * Devuelve OK solo si el dispositivo respondio y quedo seleccionado; rellena
 * \p out con los valores leidos del periférico.  El candado del bus lo toma/
 * suelta esta rutina: toda la secuencia es una sola transaccion logica y la
 * microSD no debe intercalar.
 */
int sdioEnumerateAt(struct sdioinfo *out, uint8_t card_div, bool trace)
{
    volatile uint32 *reg = sdHostRegBase();

    memset(out, 0, sizeof(*out));

    if (!sd_host_ready)
    {
        return SYSERR;                    /* el host SDMMC no está levantado */
    }

    sdmmcBusLock();

    /* CTYPE del slot 1 en 1-bit durante la identificación (bit 1 = 4-bit). */
    reg[H_CTYPE / 4] &= ~F_CTYPE_W4_CARD1;

    sdioPadsInit();
    sdioResetC6();
    if (trace)
    {
        sdBusState("1 pads+reset", SDIO_SLOT);
    }

    if (!sdioClockAt(card_div))
    {
        goto fail;
    }
    if (trace)
    {
        sdBusState("2 reloj", SDIO_SLOT);
    }

    if (!sdioGoIdleReady(&out->ocr))
    {
        goto fail;
    }
    if (trace)
    {
        sdBusState("3 CMD0+CMD5", SDIO_SLOT);
    }

    if (!sdioSetRcaSelect(&out->rca))
    {
        goto fail;
    }
    if (trace)
    {
        sdBusState("4 CMD3+CMD7", SDIO_SLOT);
    }

    if (!sdioEnableFn1(&out->cio_enable, &out->cio_ready))
    {
        goto fail;
    }
    if (trace)
    {
        sdBusState("5 habilitar fn1", SDIO_SLOT);
    }

    sdioReadIds(out);
    out->clk_khz = sd_clk_khz;
    out->up = TRUE;

    sdmmcBusUnlock();
    return OK;

fail:
    out->clk_khz = sd_clk_khz;
    sdmmcBusUnlock();
    return SYSERR;
}

/* ------------------------------------------------------------------ */
/* Presentación                                                         */
/* ------------------------------------------------------------------ */

/**
 * Volcar la configuración REAL del slot 1 leída de los propios registros, no
 * de variables del driver: el enrutado por matriz GPIO de los pads (salida y
 * entrada) y los campos por-slot del controlador.  Si el C6 no contesta, esto
 * dice si el problema es de enrutaje (pads/sig_in_sel mal) o de que el slave
 * simplemente no responde.
 */
static void sdioDumpConfig(void)
{
    volatile uint32 *reg = sdHostRegBase();
    uint32 pins[6];
    uint8  i;

    pins[0] = C6_SDIO_CLK;
    pins[1] = C6_SDIO_CMD;
    pins[2] = C6_SDIO_D0;
    pins[3] = C6_SDIO_D1;
    pins[4] = C6_SDIO_D2;
    pins[5] = C6_SDIO_D3;

    kprintf("  pads slot1 (esperado mcu=1, ie=1, out_sel=idx señal, oen=0):\r\n");
    for (i = 0; i < 6; i++)
    {
        volatile uint32 *pad  = (volatile uint32 *)(uintptr_t)P4_IOPAD(pins[i]);
        volatile uint32 *osel = (volatile uint32 *)(uintptr_t)P4_GPIO_OUT_SEL(pins[i]);

        kprintf("    pin %2u: mcu=%u ie=%u out_sel=%3u oen=%u\r\n",
                (unsigned)pins[i], (unsigned)P_MCU_SEL(*pad),
                (*pad & P_IE) ? 1U : 0U,
                (unsigned)(*osel & O_SEL_MASK),
                (*osel & O_OEN_SEL) ? 1U : 0U);
    }

    /* Entradas por la matriz: CMD..D3 son las señales 1..5 (CLK no entra). */
    kprintf("  in_sel (esperado sig_in_sel=1, in_sel=pin):\r\n");
    for (i = 1; i <= 5; i++)
    {
        volatile uint32 *isel = (volatile uint32 *)(uintptr_t)GPIO_IN_SEL_CFG(i);

        kprintf("    sig %u: sig_in_sel=%u in_sel=%2u (pin esperado=%u)\r\n",
                (unsigned)i, (*isel & I_SIG_IN_SEL) ? 1U : 0U,
                (unsigned)(*isel & I_SEL_MASK), (unsigned)pins[i]);
    }

    /* Uno por linea: kprintf no da valores fiables con cinco %x en la misma
     * llamada (se imprimian el mismo numero cinco veces). */
    kprintf("  host slot1: CLKDIV=0x%08x\r\n",    reg[H_CLKDIV / 4]);
    kprintf("  host slot1: CLKSRC=0x%08x\r\n",    reg[H_CLKSRC / 4]);
    kprintf("  host slot1: CLKENA=0x%08x\r\n",    reg[H_CLKENA / 4]);
    kprintf("  host slot1: CTYPE=0x%08x\r\n",     reg[H_CTYPE / 4]);
    kprintf("  host slot1: STATUS=0x%08x\r\n",    reg[H_STATUS / 4]);
    kprintf("  host slot1: RINTSTS=0x%08x\r\n",   reg[H_RINTSTS / 4]);
}

/** Imprimir el resultado de la enumeración (valores leídos del periférico). */
void sdioProbeReport(const struct sdioinfo *info)
{
    kprintf("SDHOST base = 0x%08x (slot0 CLK=%u CMD=%u D0=%u..D3=%u)\r\n",
            (uint32)(uintptr_t)sdHostRegBase(), SD_PIN_CLK, SD_PIN_CMD,
            SD_PIN_D0, SD_PIN_D3);
    sdioDumpConfig();

    if (!info->up)
    {
        kprintf("C6 (slot 1): SIN RESPUESTA (hwerr=0x%08x)\r\n", sd_hwerr);
        kprintf("  pads: CLK=%u CMD=%u D0=%u..D3=%u por matriz GPIO, "
                "RESET=%u\r\n",
                C6_SDIO_CLK, C6_SDIO_CMD, C6_SDIO_D0, C6_SDIO_D3, C6_RESET);
        return;
    }

    kprintf("C6 (slot 1): enumerado OK\r\n");
    kprintf("  OCR(R4)=0x%08x  RCA=0x%04x  clk=%u kHz\r\n",
            info->ocr, info->rca, info->clk_khz);
    kprintf("  io_enable=0x%02x  io_ready=0x%02x  sdio_ver=0x%02x\r\n",
            info->cio_enable, info->cio_ready, info->sdio_ver);
    kprintf("  fn0 blksize=%u  CCCR 0x0C..0x13: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
            info->blksize,
            info->fn0_info[0], info->fn0_info[1], info->fn0_info[2],
            info->fn0_info[3], info->fn0_info[4], info->fn0_info[5],
            info->fn0_info[6], info->fn0_info[7]);
    kprintf("  (el slave esp_hosted no rellena vendor/device en el CCCR: la\n"
            "   identidad y la version de firmware del C6 llegan por el RPC del\n"
            "   canal de control sobre la funcion 1 -- eso es la etapa W2.)\r\n");
}
