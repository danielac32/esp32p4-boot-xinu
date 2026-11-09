/**
 * @file ahbdma.c
 *
 * Canal TX 0 del motor AHB-DMA en modo enlace ciclico, al nivel de registro
 * (offsets crudos como en device/i2s y device/i2c, sin headers de Espressif).
 *
 * Los numeros de offset y los campos vienen del SVD de hw_ver1 del SDK
 * (core/soc/esp32p4/register/hw_ver1/soc/ahb_dma_reg.h y ahb_dma_struct.h) y
 * la secuencia de arranque es la del driver de referencia
 * (drivers/i2s_drv/i2s_drv.c, i2s_dma_init_tx).  Nada de aqui esta inventado.
 *
 * El motor tiene tres pares de canales; solo se usa el 0, y de él las dos
 * mitades: la TX alimenta al I2S0 en la salida y la IN recoge la del
 * microfono.  Ningun otro driver del arbol toca este bloque (el SD usa su
 * propio IDMAC dentro del SDHOST), asi que el par 0 esta entero libre.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include "ahbdma.h"
#include "esp32p4.h"

/* --- Mitad TX del canal 0 (bloque de 0xC0 bytes por canal) ----------- */
#define CH0_OUT_INT_RAW    0x30U
#define CH0_OUT_INT_ENA    0x38U
#define CH0_OUT_INT_CLR    0x3CU
#define CH0_OUT_CONF0      0xD0U
#define CH0_OUT_CONF1      0xD4U
#define CH0_OUT_LINK       0xE0U
#define CH0_OUT_DSCR       0xF0U
#define CH0_OUT_PRI        0xFCU
#define CH0_OUT_PERI_SEL   0x100U
#define CH0_OUT_LINK_ADDR  0x3B8U

/* --- Mitad IN del mismo canal 0 ------------------------------------- */
#define CH0_IN_INT_RAW     0x00U
#define CH0_IN_INT_ENA     0x08U
#define CH0_IN_INT_CLR     0x0CU
#define CH0_IN_CONF0       0x70U
#define CH0_IN_CONF1       0x74U
#define CH0_IN_LINK        0x80U
#define CH0_IN_STATE       0x84U
#define CH0_IN_SUC_EOF     0x88U
#define CH0_IN_DSCR        0x90U
#define CH0_IN_PRI         0x9CU
#define CH0_IN_PERI_SEL    0xA0U
#define CH0_IN_LINK_ADDR   0x3ACU
#define CH0_IN_FIFO_STATUS 0x78U

/* in_infifo_status[7:2]: palabras de 4 bytes retenidas en el FIFO del canal. */
#define IN_FIFO_CNT_MASK   (0x3FU << 2)
#define IN_FIFO_CNT_SHIFT  2U

/* --- Registro globales del bloque ------------------------------------ */
#define DMA_MISC_CONF      0x64U
#define DMA_INTR_MEM_START 0x3C4U
#define DMA_INTR_MEM_END   0x3C8U

#define DMA_REG(off)       (*(volatile uint32_t *)(AHB_DMA_BASE + (off)))

/* OUT_CONF0: rst[0] auto_wrback[2] eof_mode[3] dscr_burst[4] data_burst[5] */
#define OUT_RST            (1U << 0)
#define OUT_EOF_MODE       (1U << 3)

/* OUT_CONF1: check_owner[12] */
#define OUT_CHECK_OWNER    (1U << 12)

/* OUT_LINK: stop[0], start[1] y restart[2] son de disparo por escritura a 1;
 * park[3] es de lectura: 1 = el FSM del enlace esta inactivo. */
#define OUTLINK_STOP       (1U << 0)
#define OUTLINK_START      (1U << 1)
#define OUTLINK_PARK       (1U << 3)

/* IN_LINK: los mandos estan desplazados un bit respecto al OUT (stop[1],
 * start[2], restart[3]) y el bit 0 es auto_ret, que de reset vale 1 y se deja
 * asi: con la lista ciclica no hay eof que devuelva, pero si un descriptor
 * rechazado el FSM vuelve al principio en vez de colgarse.  park[4]. */
#define INLINK_AUTORET     (1U << 0)
#define INLINK_STOP        (1U << 1)
#define INLINK_START       (1U << 2)
#define INLINK_PARK        (1U << 4)

/* IN_CONF0: rst[0]; IN_CONF1: check_owner[12], que aqui se deja apagado. */
#define IN_RST             (1U << 0)

/* Los bits bajos de IN son los eventos RX: done[0], suc_eof[1], err_eof[2],
 * error de descriptor[3] y descriptor vacio[4], que sin comprobacion de dueno
 * ya no es el aviso de una captura perdida.  Se limpian todos al montar el
 * canal. */
#define IN_INT_ALL         0x1FU

/* MISC_CONF: clk_en[3] fuerza el reloj de registros; el de fabrica solo da
 * reloj mientras se escriben registros, y hay que leer el estado en bucle. */
#define DMA_REG_CLK_FORCE  (1U << 3)

/* Los cuatro bits bajos son los eventos TX: done, eof, error de descriptor y
 * total eof.  Se limpian todos al montar el canal. */
#define OUT_INT_ALL        0x0FU

/* Periferico del canal: 3 = I2S0 (soc/gdma_channel.h).  0x3F es el valor de
 * reset y el que usa el SDK para desconectar; deja el canal sin demanda. */
#define PERI_I2S0          3U
#define PERI_NONE          0x3FU

/* Ventana de memoria que el motor acepta.  Cubre L2MEM, L2ROM, flash y PSRAM
 * por el camino con cache (0x48......); el acceso directo del CPU
 * (0x88.......) cae fuera y una direccion fuera de la ventana se salda con
 * un error de descriptor, asi que los descriptores se escriben por 0x88 pero
 * se les pone dentro la forma 0x48. */
#define DMA_MEM_START      0x40000000U
#define DMA_MEM_END        0x4FFC0000U

/* Reloj de registros del motor: HP_SYS_CLKRST.soc_clk_ctrl1, bit 3
 * (reg_ahb_pdma_sys_clk_en).  La APB del bloque I2S la abre el driver de
 * relojes, no aqui. */
#define CLKRST_SOC_CLK_CTRL1  0x18U
#define C1_AHB_PDMA_CLK_EN    (1U << 3)

/* Igual que en device/sdmmc-esp32p4/sdHw.c: el bloque de cache sale de reset
 * con el acceso de los masters cerrado y un motor que lo atraviesa lee ceros.
 * ESP-IDF lo abre en cada arranque; aqui se escribe el registro directamente. */
#define CACHE_L2_BASE      0x3FF10000UL
#define CACHE_L2_CTRL      0x270U
#define L2_SHUT_DMA        (1U << 4)

#define PARK_GUARD         100000U

static bool linked;
static bool linkedIn;

/**
 * @ingroup audio
 *
 * Apertura de lo que el motor necesita antes de tocar ningun registro de
 * canal: su reloj de registros, el de la propia matriz de arbitraje y la
 * ventana de direcciones que acepta.
 */
static void dmaClocksOpen(void)
{
    *(volatile uint32_t *)(HP_SYS_CLKRST_BASE + CLKRST_SOC_CLK_CTRL1)
        |= C1_AHB_PDMA_CLK_EN;
    *(volatile uint32_t *)(CACHE_L2_BASE + CACHE_L2_CTRL) &= ~L2_SHUT_DMA;

    DMA_REG(DMA_MISC_CONF) |= DMA_REG_CLK_FORCE;
    DMA_REG(DMA_INTR_MEM_START) = DMA_MEM_START;
    DMA_REG(DMA_INTR_MEM_END) = DMA_MEM_END;
}

/**
 * Reset del FSM y del puntero del FIFO del canal.  OUT_RST es de
 * autolimpieza, asi que la segunda escritura vale para dejar el registro en
 * el modo EOF de fabrica (1) y con las dos rafagas de burst apagadas: el
 * driver de referencia las deja a 0 para los trozos de audio, porque los
 * burst imponen requisitos de alineacion entre variantes del chip.
 */
static void channelReset(void)
{
    DMA_REG(CH0_OUT_CONF0) = OUT_RST;
    DMA_REG(CH0_OUT_CONF0) = OUT_EOF_MODE;
}

/** Esperar a que el FSM del enlace se aparque, con tope. */
static bool waitParked(void)
{
    uint32_t guard = PARK_GUARD;

    while ((DMA_REG(CH0_OUT_LINK) & OUTLINK_PARK) == 0U)
    {
        if (--guard == 0U)
        {
            return false;
        }
    }
    return true;
}

/**
 * @ingroup audio
 *
 * Montar el canal y ponerlo a dar vueltas por la lista.  La propiedad de los
 * descriptores tiene que estar ya cedida al motor: con la comprobacion de
 * dueño activada (como en la referencia) un descriptor del CPU se rechaza con
 * un error de enlace.
 *
 * Sin interrupciones: todo se sondea desde la shell.
 */
bool ahbdma_tx_link(uint32_t first_desc)
{
    /* La ventana se comprueba aqui y no en el llamador: una lista montada con
     * el acceso directo del CPU (0x88.......) cae fuera, el motor la rechaza
     * con un error de descriptor y el unico sintoma seria el silencio.  Los
     * descriptores del AHB-DMA solo piden alineacion de 4 bytes. */
    if (first_desc < DMA_MEM_START || first_desc >= DMA_MEM_END ||
        (first_desc & 0x3U) != 0U)
    {
        return false;
    }

    dmaClocksOpen();

    DMA_REG(CH0_OUT_LINK) = OUTLINK_STOP;
    (void)waitParked();
    channelReset();

    DMA_REG(CH0_OUT_CONF1) = OUT_CHECK_OWNER;
    DMA_REG(CH0_OUT_PRI) = 1U;
    DMA_REG(CH0_OUT_INT_ENA) = 0U;
    DMA_REG(CH0_OUT_INT_CLR) = OUT_INT_ALL;
    DMA_REG(CH0_OUT_PERI_SEL) = PERI_I2S0;

    /* El puntero del enlace es un registro aparte, no un campo de OUT_LINK. */
    DMA_REG(CH0_OUT_LINK_ADDR) = first_desc;

    /* Escrito el enlace, se suelta el FSM.  Como la lista es ciclica, el
     * motor no ve nunca el final: repasa los descriptores mientras el I2S
     * pida datos, que es lo que hace falta para un tono continuo. */
    DMA_REG(CH0_OUT_LINK) = OUTLINK_START;
    linked = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Parar el enlace, resetear el canal y dejarlo sin periferico.  Se puede
 * llamar aunque no este enlazado: las escrituras son inocuas.
 */
void ahbdma_tx_stop(void)
{
    DMA_REG(CH0_OUT_LINK) = OUTLINK_STOP;
    (void)waitParked();
    channelReset();
    DMA_REG(CH0_OUT_PERI_SEL) = PERI_NONE;
    DMA_REG(CH0_OUT_INT_CLR) = OUT_INT_ALL;
    linked = false;
}

bool ahbdma_tx_running(void)
{
    return linked && (DMA_REG(CH0_OUT_LINK) & OUTLINK_PARK) == 0U;
}

uint32_t ahbdma_tx_raw(void)
{
    return DMA_REG(CH0_OUT_INT_RAW) & OUT_INT_ALL;
}

void ahbdma_tx_clear(uint32_t mask)
{
    DMA_REG(CH0_OUT_INT_CLR) = mask & OUT_INT_ALL;
}

uint32_t ahbdma_tx_current_desc(void)
{
    return DMA_REG(CH0_OUT_DSCR);
}

/** Esperar a que el FSM del enlace de entrada se aparque, con el mismo tope. */
static bool waitInParked(void)
{
    uint32_t guard = PARK_GUARD;

    while ((DMA_REG(CH0_IN_LINK) & INLINK_PARK) == 0U)
    {
        if (--guard == 0U)
        {
            return false;
        }
    }
    return true;
}

/** Reset del FSM RX y del puntero del FIFO del canal de entrada. */
static void inChannelReset(void)
{
    DMA_REG(CH0_IN_CONF0) = IN_RST;
    DMA_REG(CH0_IN_CONF0) = 0U;
}

/**
 * @ingroup audio
 *
 * Montar el canal de entrada y soltarlo por la lista ciclica.
 *
 * La diferencia con la salida esta en el modo de trabajo: aqui el motor llega
 * a memoria que escribe el periferico, y el driver de referencia del I2S en RX
 * **no** activa la comprobacion de dueno (esp_driver_i2s no llama a
 * gdma_apply_strategy): deja la lista ciclica correr y reescribir el trozo mas
 * antiguo, y la posicion la saca el consumidor de in_suc_eof_des_addr, el
 * descriptor donde ocurrio el ultimo fin de recepcion.  Con la comprobacion
 * activada el enlace se para en cuanto encuentra un descriptor sin ceder (y en
 * placa se ha visto que ademas levanta in_dscr_err y aparca el FSM al recalar
 * en un dueno que no le vale), que no es lo que necesita un micrfono: mejor
 * perder los primeros milisegundos que quedarse parado.
 *
 * @param first_desc direccion de bus del primer descriptor (forma con cache)
 * @return true si el enlace quedo en marcha
 */
bool ahbdma_in_link(uint32_t first_desc)
{
    if (first_desc < DMA_MEM_START || first_desc >= DMA_MEM_END ||
        (first_desc & 0x3U) != 0U)
    {
        return false;
    }

    dmaClocksOpen();

    DMA_REG(CH0_IN_LINK) = INLINK_STOP;
    (void)waitInParked();
    inChannelReset();

    /* IN_CONF1 a 0: sin comprobacion de dueno, como el RX de referencia. */
    DMA_REG(CH0_IN_CONF1) = 0U;
    DMA_REG(CH0_IN_PRI) = 1U;
    DMA_REG(CH0_IN_INT_ENA) = 0U;
    DMA_REG(CH0_IN_INT_CLR) = IN_INT_ALL;
    DMA_REG(CH0_IN_PERI_SEL) = PERI_I2S0;

    DMA_REG(CH0_IN_LINK_ADDR) = first_desc;

    DMA_REG(CH0_IN_LINK) = INLINK_START;
    linkedIn = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Parar el enlace de entrada, resetear el canal y dejarlo sin periferico.
 */
void ahbdma_in_stop(void)
{
    DMA_REG(CH0_IN_LINK) = INLINK_STOP;
    (void)waitInParked();
    inChannelReset();
    DMA_REG(CH0_IN_PERI_SEL) = PERI_NONE;
    DMA_REG(CH0_IN_INT_CLR) = IN_INT_ALL;
    linkedIn = false;
}

bool ahbdma_in_running(void)
{
    return linkedIn && (DMA_REG(CH0_IN_LINK) & INLINK_PARK) == 0U;
}

/**
 * @ingroup audio
 *
 * Bits crudos del canal de entrada.  Sin comprobacion de dueno el enlace no se
 * aparca por descriptores sin consumir, asi que estos avisos solo valen como
 * diagnostico: dscr_err si una descripcion esta mal formada y err_eof si el
 * periferico corto una recepcion antes de tiempo.
 */
uint32_t ahbdma_in_raw(void)
{
    return DMA_REG(CH0_IN_INT_RAW) & IN_INT_ALL;
}

void ahbdma_in_clear(uint32_t mask)
{
    DMA_REG(CH0_IN_INT_CLR) = mask & IN_INT_ALL;
}

uint32_t ahbdma_in_current_desc(void)
{
    return DMA_REG(CH0_IN_DSCR);
}

/** Descriptor cuyo buffer acabo de llenar el motor (in_suc_eof_des_addr): la
 *  unica posicion de escritura que da el RX del P4, porque no tiene el contador
 *  de sincronizacion del TX. */
uint32_t ahbdma_in_eof_desc(void)
{
    return DMA_REG(CH0_IN_SUC_EOF);
}

/** Nivel del FIFO de recepcion del canal, en palabras de 4 bytes.  Es lo que
 *  separa un atasco de memoria (nivel alto y quieto) de un periferico que no
 *  sirve datos (nivel casi siempre a cero). */
uint32_t ahbdma_in_fifo_words(void)
{
    return (DMA_REG(CH0_IN_FIFO_STATUS) & IN_FIFO_CNT_MASK) >> IN_FIFO_CNT_SHIFT;
}

/** Valor crudo de in_state: [17:0] son los bits bajos de la direccion del
 *  descriptor en curso y [22:20] la maquina de estados del enlace.  El SVD no
 *  documenta que significan los valores de la maquina, asi que solo vale como
 *  referencia estable al comparar dos momentos. */
uint32_t ahbdma_in_state(void)
{
    return DMA_REG(CH0_IN_STATE);
}
