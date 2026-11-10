/**
 * @file audioIn.c
 *
 * Captura del microfono: anillo de descriptores en PSRAM que llena el canal IN
 * del AHB-DMA desde el FIFO RX del I2S0, y su consumidor.
 *
 * La memoria sale del linker, no del heap: son las dos paginas que declara
 * .audioshare (una para la lista de descriptores y otra para las muestras, cada
 * una dentro de su propia linea de cache y sin nada mas al lado).  El reparto
 * de direcciones es el mismo que en device/sdmmc y en la etapa de tono: el CPU
 * escribe y lee solo por el acceso directo 0x88......, que no pasa por el
 * cache, y dentro de los descriptores se anota la forma 0x48......, que es la
 * unica que entiende el motor y la que cae en su ventana.  Pedirlo al heap
 * hacia que la cabecera del bloque compartiera linea con el principio de la
 * lista: el motor enlazaba descriptores rancios y la expulsion de esa linea
 * pisa luego la lista publicada (medido en placa).
 *
 * El modelo de trabajo es el del driver de referencia del I2S en recepcion, no
 * el del enlace con comprobacion de dueno: la lista es ciclica, el motor corre
 * y reescribe el trozo mas antiguo cuando el consumidor no llega, y la
 * posicion de escritura la da in_suc_eof_des_addr (el descriptor donde acabo de
 * ocurrir un fin de recepcion), que es lo unico que hay porque el RX del P4 no
 * tiene el contador de sincronizacion del TX.  Con ese registro se lleva una
 * cuenta de trozos producidos y consumidos: de ahi salen los bytes disponibles
 * y las tramas perdidas cuando el motor da una vuelta entera por delante.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>

#include "audioIn.h"
#include "psramcopy.h"
#include "i2s.h"
#include "ahbdma.h"
#include "device.h"
#include <stdio.h>

/* Descriptor del motor: las mismas tres palabras de dma_descriptor_t del SDK.
 * El dw0 lo reescribe el motor al cerrar el trozo: anota los bytes recibidos y
 * deja el resto como se publico. */
typedef struct
{
    uint32_t dw0;
    uint32_t buffer;
    uint32_t next;
} in_desc;

#define DESC_LEN_SHIFT       12U             /**< bytes validos       [23:12] */
#define DESC_SIZE_MASK       0xFFFU          /**< capacidad del buffer  [11:0] */

/* El montaje del driver de referencia (i2s_alloc_dma_desc): todos los
 * descriptores de recepcion salen con el eof a 1 y cedidos al motor, y ya no se
 * tocan.  Con la comprobacion de dueno apagada el bit de dueno le da igual al
 * motor; el de fin es el que avisa cuando un trozo quedo cerrado. */
#define DESC_SUC_EOF         (1U << 30)      /**< fin de recepcion      [30]  */
#define DESC_OWNER_DMA       (1U << 31)      /**< cedido al motor       [31]  */

/* Bits de in_int_raw del canal, en el orden del SVD: done[0], suc_eof[1],
 * err_eof[2], error de descriptor[3] y descriptor vacio[4].  Sin comprobacion
 * de dueno el ultimo no es senal de nada propio: lo que delata un desbordado
 * es que el motor pasa por delante del consumidor, y eso se mira en la cuenta
 * de trozos, no aqui. */
#define IN_RAW_ERR_EOF       (1U << 2)
#define IN_RAW_DSCR_ERR      (1U << 3)
#define IN_RAW_DSCR_EMPTY    (1U << 4)

#define IN_FRAME_BYTES       (I2S_FRAME_BITS / 8U)   /* 16 bits x 2 slots */

/* El trozo mide lo mismo que el evento de fin de recepcion del I2S
 * (I2S_RX_EOF_BYTES): asi cada fin deja un descriptor justo lleno, que es como
 * lo monta el driver de referencia. */
#define IN_CHUNK_BYTES       I2S_RX_EOF_BYTES
#define IN_CHUNK_FRAMES      (IN_CHUNK_BYTES / IN_FRAME_BYTES)
#define IN_DESC_COUNT        32U                     /* 65536 bytes de anillo */

#define AUDIO_CPU_UNCACHED   0x40000000UL

/* Las dos paginas que declara .audioshare en el script de enlaces: la lista de
 * descriptores en una y las muestras en la otra, de 4096 y de 65536 bytes, o
 * sea ninguna linea de cache compartida con nada que se toque por la via con
 * cache.  NOLOAD: solo las escribe este driver. */
extern uint8_t _audio_in_desc_page[];
extern uint8_t _audio_in_buf_page[];

/* Barrera de ordenacion: la lista se publica entera antes de soltar el motor. */
#define MEM_BARRIER()        asm volatile("fence rw, rw" ::: "memory")

static in_desc *desc;                 /* la zona, en forma uncached del CPU   */
static uint8_t *data;
static uint32_t descBus;              /* direccion tal y como la ve el motor  */
static uint32_t dataBus;
static uint32_t drainIdx;             /* descriptor que se esta entregando     */
static uint32_t drainOff;             /* dentro de ese descriptor              */
static uint32_t eofIdx;               /* ultimo visto en in_suc_eof_des_addr   */
static uint32_t eofStale;            /* posicion que dejo la sesion anterior  */
static bool eofSeen;
static uint64_t produced;             /* trozos cerrados por el motor          */
static uint64_t consumed;             /* trozos entregados al consumidor       */
static uint32_t dropped;
static bool running;

/** Indice del descriptor en @p addr dentro del anillo, o IN_DESC_COUNT si no
 *  es una direccion del anillo. */
static uint32_t in_desc_index(uint32_t addr)
{
    uint32_t off = addr - descBus;

    if (addr < descBus ||
        off >= IN_DESC_COUNT * (uint32_t)sizeof(in_desc) ||
        (off % (uint32_t)sizeof(in_desc)) != 0U)
    {
        return IN_DESC_COUNT;
    }
    return off / (uint32_t)sizeof(in_desc);
}

/**
 * @ingroup audio
 *
 * Avanzar la cuenta de produccion con el ultimo descriptor cerrado que reporta
 * el motor.  Entre dos miradas el motor puede haber dado como mucho una vuelta
 * menos un trozo; si se pasa de ahi, la cuenta real no se puede recuperar de
 * ningun registro y lo unico honesto es quedarse con los trozos mas nuevos.
 *
 * @return false si el motor todavia no ha cerrado ningun trozo
 */
static bool in_produce(void)
{
    uint32_t idx = in_desc_index(ahbdma_in_eof_desc());
    uint32_t move;

    if (idx >= IN_DESC_COUNT)
    {
        return false;
    }
    if (!eofSeen)
    {
        /* Primer avistamiento desde que se monto el anillo.  El motor empieza
         * siempre por el descriptor 0:
         *  - si el registro sigue en la posicion que dejo la sesion anterior,
         *    no ha cerrado nada todavia; la referencia tiene que ser 31 para
         *    que el primer cambio al 0 cuente UN trozo (con la posicion vieja
         *    como referencia, un 24 -> 0 contaba 8 y el lector se iba por
         *    delante del motor: copiaba el trozo que se estaba llenando y la
         *    toma entera salia a cero).
         *  - si ya cambio, ha cerrado 0..idx y hay idx+1 trozos buenos. */
        eofSeen = true;
        printf("audiodbg: primer eof stale=%u idx=%u\r\n", eofStale, idx);
        if (idx == eofStale)
        {
            eofIdx = IN_DESC_COUNT - 1U;
            produced = 0U;
        }
        else
        {
            eofIdx = idx;
            produced = (uint32_t)(idx + 1U);
        }
        return produced > consumed;
    }
    if (idx == eofIdx)
    {
        return true;
    }

    move = (idx + IN_DESC_COUNT - eofIdx) % IN_DESC_COUNT;
    produced += move;
    eofIdx = idx;

    /* El productor esta a una vuelta entera o mas por delante: la cabeza se
     * pone en el trozo mas antiguo que queda intacto y lo demas cuenta como
     * captura perdida. */
    if (produced - consumed >= IN_DESC_COUNT)
    {
        uint32_t lost = (uint32_t)(produced - consumed) - (IN_DESC_COUNT - 1U);

        dropped += lost * IN_CHUNK_FRAMES;
        consumed += lost;
        drainIdx = (uint32_t)(consumed % IN_DESC_COUNT);
        drainOff = 0U;
    }
    return true;
}

/** Bytes validos del trozo @p i: los que anoto el motor al cerrar el
 *  descriptor, con el tamano del trozo como techo. */
static uint32_t in_filled(uint32_t i)
{
    uint32_t len = (desc[i].dw0 >> DESC_LEN_SHIFT) & DESC_SIZE_MASK;

    if (0U == len || len > IN_CHUNK_BYTES)
    {
        return IN_CHUNK_BYTES;
    }
    return len;
}

/**
 * @ingroup audio
 *
 * Montar el anillo sobre las paginas que ya reserva el linker: no hay peticion
 * que pueda fallar ni memoria que devolver, y el tamano del anillo es fijo
 * (32 trozos de un fin de recepcion del I2S).
 *
 * @return true si la captura quedo en marcha
 */
bool audio_in_start(void)
{
    uint32_t i;

    if (running)
    {
        return true;
    }

    /* El CPU por el acceso directo y el motor con la forma que entiende: las
     * dos apuntan a las mismas celdas y nadie pasa por el cache. */
    descBus = (uint32_t)(uintptr_t)_audio_in_desc_page;
    dataBus = (uint32_t)(uintptr_t)_audio_in_buf_page;
    desc = (in_desc *)(uintptr_t)(descBus + AUDIO_CPU_UNCACHED);
    data = (uint8_t *)(uintptr_t)(dataBus + AUDIO_CPU_UNCACHED);

    /* El anillo se deja a cero antes de soltar el motor: lo que el motor no
     * escribe no puede salir como basura. */
    psram_zero(data, IN_DESC_COUNT * IN_CHUNK_BYTES);

    /* DIAGNOSTICO TEMPORAL: centinela en los 16 primeros bytes de cada trozo;
     * si al parar siguen ahi es que el motor no ha pasado por ese trozo. */
    for (i = 0U; i < IN_DESC_COUNT; i++)
    {
        uint32_t j;

        for (j = 0U; j < 16U; j++)
        {
            data[i * IN_CHUNK_BYTES + j] = 0xDEU;
        }
    }

    for (i = 0U; i < IN_DESC_COUNT; i++)
    {
        desc[i].buffer = dataBus + i * IN_CHUNK_BYTES;
        desc[i].next = descBus + ((i + 1U) % IN_DESC_COUNT) *
                       (uint32_t)sizeof(in_desc);
        desc[i].dw0 = DESC_SUC_EOF | DESC_OWNER_DMA | IN_CHUNK_BYTES |
                      (IN_CHUNK_BYTES << DESC_LEN_SHIFT);
    }
    MEM_BARRIER();

    /* Primero el reloj, despues la ruta del micro.  i2s_rx_start() es lo que
     * sube el MCLK del dominio TX, del que vive el codec; es8311_start_mic()
     * escribe entre otras cosas la ganancia digital del ADC (0x17), y esa
     * ganancia la carga el fade del ADC, que solo funciona con reloj.  Con el
     * orden antigo la primera toma despues de arrancar se quedaba sin ganancia
     * aplicada y solo registraba el transitorio de encendido del PGA. */
    if (!i2s_rx_start())
    {
        return false;
    }
    if (!es8311_start_mic())
    {
        (void)i2s_rx_stop();
        return false;
    }
    /* Posicion de fin de recepcion que dejo la sesion anterior.  Se apunta
     * antes de enlazar el motor, que es cuando el registro aun es el viejo:
     * in_produce() la usa para no contar como propio un trozo que no lo es. */
    eofStale = in_desc_index(ahbdma_in_eof_desc());

    if (!ahbdma_in_link(descBus))
    {
        (void)i2s_rx_stop();
        (void)es8311_stop_mic();
        return false;
    }

    drainIdx = 0U;
    drainOff = 0U;
    eofIdx = 0U;
    eofSeen = false;
    produced = 0U;
    consumed = 0U;
    dropped = 0U;
    running = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Bajar la captura en el orden inverso al de subida: primero el motor, para
 * que nadie escriba en el anillo mientras se suelta; despues el RX, y al final
 * la ruta del codec, que es la que deja de mover electrones.
 */
void audio_in_stop(void)
{
    if (!running)
    {
        return;
    }

    {
        uint32_t m = 0U;
        uint32_t i, j;

        for (i = 0U; i < IN_DESC_COUNT; i++)
        {
            bool sent = true;

            for (j = 0U; j < 16U; j++)
            {
                if (data[i * IN_CHUNK_BYTES + j] != 0xDEU)
                {
                    sent = false;
                    break;
                }
            }
            if (!sent)
            {
                m |= (1U << i);
            }
        }
        printf("audiodbg: eof=0x%08x idx=%u prod=%u cons=%u drain=%u/%u mask=0x%08x\r\n",
               ahbdma_in_eof_desc(), in_desc_index(ahbdma_in_eof_desc()),
               (unsigned)produced, (unsigned)consumed, drainIdx, drainOff, m);
    }

    ahbdma_in_stop();
    (void)i2s_rx_stop();
    (void)es8311_stop_mic();

    desc = NULL;
    data = NULL;
    running = false;
}

/**
 * @ingroup audio
 *
 * true mientras el anillo este montado; no toca ningun registro, que es lo que
 * necesita el read() del device para decidir si hay que arrancar la captura.
 */
bool audio_in_running(void)
{
    return running;
}

/**
 * @ingroup audio
 *
 * Entregar lo capturado.  La cabeza va del ultimo trozo entregado al ultimo
 * trozo cerrado por el motor (el que marca in_suc_eof_des_addr), de modo que
 * nunca se lee el trozo que se esta llenando ahora mismo.  Si el consumidor
 * pide menos de lo que cabe en un descriptor se recuerda el desplazamiento y el
 * trozo se da por entregado al vaciarse.
 *
 * La vuelta del anillo se copia de palabra en palabra (psramcopy.h): a byte la
 * ventana directa de PSRAM no llega a los 192.000 bytes/s del I2S y el motor se
 * pasa por delante del consumidor a mitad de la toma.
 *
 * @return bytes entregados, 0 si todavia no hay ninguno
 */
uint32_t audio_in_read(void *dst, uint32_t nbytes)
{
    uint32_t total = 0U;
    uint32_t filled;
    uint32_t ready;
    uint32_t n;

    if (!running || NULL == dst || 0U == nbytes)
    {
        return 0U;
    }

    if (!in_produce())
    {
        return 0U;
    }

    while (total < nbytes && consumed < produced)
    {
        MEM_BARRIER();
        filled = in_filled(drainIdx);
        ready = filled - drainOff;

        n = nbytes - total;
        if (n > ready)
        {
            n = ready;
        }

        psram_copy((uint8_t *)dst + total, data + drainIdx * IN_CHUNK_BYTES +
                   drainOff, n);
        total += n;
        drainOff += n;

        if (drainOff == filled)
        {
            drainOff = 0U;
            drainIdx = (drainIdx + 1U) % IN_DESC_COUNT;
            consumed++;
        }
    }

    return total;
}

uint32_t audio_in_ring_bytes(void)
{
    return running ? IN_DESC_COUNT * IN_CHUNK_BYTES : 0U;
}

uint32_t audio_in_dropped(void)
{
    return dropped;
}

/**
 * @ingroup audio
 *
 * Estado traducido de los registros del motor y del I2S.  Los bits crudos se
 * limpian aqui, que es el unico sitio donde se miran: sin interrupciones, si no
 * se limpian el aviso siguiente no dice nada nuevo.
 */
uint32_t audio_in_status(void)
{
    uint32_t raw;
    uint32_t st = 0U;

    if (!running)
    {
        return 0U;
    }

    st |= AUDIO_IN_ST_ON;
    if (ahbdma_in_running())
    {
        st |= AUDIO_IN_ST_LINK;
    }
    else
    {
        st |= AUDIO_IN_ST_PARKED;
    }
    if (i2s_rx_started())
    {
        st |= AUDIO_IN_ST_RX;
    }
    if (es8311_mic_running())
    {
        st |= AUDIO_IN_ST_MIC;
    }

    (void)in_produce();

    raw = ahbdma_in_raw();
    if ((raw & IN_RAW_ERR_EOF) != 0U)
    {
        st |= AUDIO_IN_ST_ERR_EOF;
    }
    if ((raw & IN_RAW_DSCR_ERR) != 0U)
    {
        st |= AUDIO_IN_ST_DSCR_ERR;
    }
    if ((raw & IN_RAW_DSCR_EMPTY) != 0U)
    {
        st |= AUDIO_IN_ST_EMPTY;
    }
    ahbdma_in_clear(raw);

    return st;
}

uint32_t audio_in_dma_desc(void)
{
    return running ? ahbdma_in_current_desc() : 0U;
}

/** Descriptor cerrado mas reciente (in_suc_eof_des_addr): la posicion del
 *  motor sobre el anillo. */
uint32_t audio_in_eof_desc(void)
{
    return running ? ahbdma_in_eof_desc() : 0U;
}

/** Bytes en mano del consumidor, de los que quedan intactos en el anillo. */
uint32_t audio_in_queued(void)
{
    if (!running)
    {
        return 0U;
    }
    (void)in_produce();
    if (produced <= consumed)
    {
        return 0U;
    }
    return (uint32_t)((produced - consumed) * IN_CHUNK_BYTES - drainOff);
}

/** Direccion del anillo de capturas en la forma que ve el motor. */
uint32_t audio_in_ring_base(void)
{
    return running ? dataBus : 0U;
}
