/**
 * @file audioOut.c
 *
 * Anillo de PCM + sintetizador del tono + secuencia del camino de salida.
 *
 * Las paginas compartidas con el motor viven en PSRAM (.audioshare en el
 * linker) y se reparten como las del SD: el CPU las escribe por el acceso
 * directo 0x88......, que no pasa por el cache, y dentro de los descriptores
 * se anota la forma 0x48......, que es la direccion que entiende el AHB-DMA y
 * la unica que cae en su ventana.  Asi no hace falta ninguna operacion de
 * cache en el camino de datos.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include "audioOut.h"
#include "audioIn.h"
#include "psramcopy.h"
#include "i2s.h"
#include "ahbdma.h"
#include "device.h"
#include "board.h"
#include "gpio.h"
#include "clock.h"
#include <memory.h>
#include <string.h>
#include <thread.h>

/* Paginas definidas en compile/platforms/esp32p4/ld.script: una para los
 * descriptores y dos para las muestras del patron de tono, y dos mas para el
 * anillo de reproduccion que rellena quien llama. */
extern uint8_t _audio_desc_page[];
extern uint8_t _audio_buf_page[];
extern uint8_t _audio_stream_desc_page[];
extern uint8_t _audio_stream_buf_page[];

#define AUDIO_CPU_UNCACHED   0x40000000UL
#define audio_desc_cpu       ((volatile uint8_t *) \
                                  ((uintptr_t)_audio_desc_page + AUDIO_CPU_UNCACHED))
#define audio_buf_cpu        ((volatile uint8_t *) \
                                  ((uintptr_t)_audio_buf_page + AUDIO_CPU_UNCACHED))

/* Descriptor del motor: tres palabras, exactamente la estructura
 * dma_descriptor_t del SDK (core/hal/include/hal/dma_types.h) para el AHB-DMA
 * del P4: dw0 con tamano/longitud/fin de lista/propietario, el buffer y el
 * siguiente.  No hay cuarta palabra. */
typedef struct
{
    uint32_t dw0;
    uint32_t buffer;
    uint32_t next;
} link_desc;

#define DESC_LEN_SHIFT       12U             /**< bytes validos       [23:12] */
#define DESC_OWNER_DMA       (1U << 31)      /**< cedido al motor       [31]  */

/* Orden del modelo de memoria del SDK: publicar el descriptor y cederlo al
 * ultimo, con la barrera en medio para que el motor no vea el bit de dueño
 * antes que los campos. */
#define MEM_BARRIER()        asm volatile("fence rw, rw" ::: "memory")

#define OUT_FRAME_BYTES      (I2S_FRAME_BITS / 8U)   /* 16 bits x 2 slots */
#define OUT_BUF_BYTES        8192U                  /* las dos paginas     */
#define OUT_MAX_FRAMES       (OUT_BUF_BYTES / OUT_FRAME_BYTES)
#define OUT_CHUNK_FRAMES     512U

#define OUT_MIN_HZ           50U
#define OUT_MAX_HZ           8000U
#define OUT_FREQ_SLACK       96U      /* barrido de frecuencias alrededor       */

/* Volumen de arranque si el codec esta a cero (su valor de reset, -95,5 dB).
 * El registro es lineal en decibelios y 75 % cae justo en el escalon 191, que
 * es el 0 dB del DAC: ni atenuacion ni realce digital.  Mucho mas arriba se
 * entra en el realce del propio DAC y el tono empieza a recortarse. */
#define OUT_START_VOLUME     75U

/* Amplitud digital del tono: 32000 sobre 32767, es decir -0,3 dBFS.  A priori
 * parece obvio ir arriba, pero con el codec en 0 dB esto es lo que llena la
 * escala; antes estaban los dos recortando (esta onda al 25 % y el registro al
 * -19,5 dB) y el resultado era un tono casi inaudible. */
#define OUT_AMPLITUDE        32000

static bool running;
static bool ampOn;
static uint32_t ringFrames;

/* --- stream: el anillo que rellena quien llama ------------------------- */

/* Trozos de 512 tramas, los mismos que usa el RX, para que el tamano del
 * descriptor y el margen de lectura por delante del I2S sean la misma unidad en
 * las dos etapas.  32 trozos son 341 ms de aire: con ese colchon el ritmo de
 * quien escribe (la SD, el heap de capturas) no se oye. */
#define STREAM_CHUNK_FRAMES  OUT_CHUNK_FRAMES
#define STREAM_CHUNK_BYTES   (STREAM_CHUNK_FRAMES * OUT_FRAME_BYTES)
#define STREAM_CHUNKS        32U
#define STREAM_RING_BYTES    (STREAM_CHUNKS * STREAM_CHUNK_BYTES)
#define STREAM_RING_FRAMES   (STREAM_RING_BYTES / OUT_FRAME_BYTES)

/* El motor no espera a que el I2S vacie para pedir el siguiente trozo: tiene
 * en vuelo el descriptor en curso y el FIFO del TX (64 palabras de 32 bits).
 * Se le dejan dos trozos, que es lo que separa la cabeza de escritura del punto
 * por el que va leyendo cuando el anillo esta a tope. */
#define STREAM_GUARD_BYTES   (2U * STREAM_CHUNK_BYTES)
#define STREAM_GUARD_MS      (STREAM_GUARD_BYTES * 1000U / \
                              (ES8311_SAMPLE_RATE_HZ * OUT_FRAME_BYTES) + 5U)

/* Esperas sin avance antes de dar la escritura por perdida: 500 de dos ms son
 * el doble de lo que tarda en vaciarse el anillo entero, asi que solo salta si
 * el reloj del I2S de verdad se paro. */
#define STREAM_STALL_LIMIT   500U

/* El anillo de reproduccion vive en las dos paginas que declara .audioshare en
 * ld.script, cada una dentro de su propia linea de cache y sin nada al lado.
 * Pedirlo al heap ponia la cabecera del bloque, escrita por la via con cache,
 * compartiendo linea con el principio de la lista: el motor enlazaba
 * descriptores rancios y la expulsion de esa linea pisaba luego la lista
 * publicada (medido en placa con la lista de captura). */
static volatile link_desc *streamDesc;
static uint8_t *streamRing;
static uint32_t streamDescBus;              /* 0x48......, la forma del motor */
static uint32_t streamRingBus;
static uint32_t streamWritten;        /* bytes escritos, monotono            */
static uint32_t streamGaps;     /* vueltas del motor por delante del escritor */
static bool streaming;

static void stream_release(void);

/**
 * @ingroup audio
 *
 * Seno de un ciclo completo repartido en @p len frames, evaluada en la
 * posicion @p u del ciclo.  Tabla de un cuarto de periodo con interpolacion
 * lineal y aritmetica entera: el error de la interpolacion no llega al uno por
 * mil de la amplitud, y como la posicion se calcula en entero el patron cierra
 * el bucle sin costura.
 */
static const int16_t quarter_wave[17] = {
    0, 3137, 6243, 9289, 12246, 15085, 17778, 20301, 22627,
    24736, 26607, 28221, 29564, 30622, 31385, 31846, OUT_AMPLITUDE
};

static int16_t sine_at(uint32_t u, uint32_t len)
{
    uint32_t quad = (u * 4U) / len;            /* cuadrante 0..3             */
    uint32_t in = (u * 4U) % len;              /* puesto dentro del cuadrante */
    uint32_t pos, rem, mag;

    /* En los cuadrantes impares la onda baja: se recorre la tabla del cuarto
     * en sentido contrario.  El signo lo ponen los cuadrantes 2 y 3. */
    if ((quad & 1U) != 0U)
    {
        in = len - in;
    }
    pos = (in * 16U) / len;                    /* indice 0..16 de la tabla */
    if (pos >= 16U)
    {
        mag = (uint32_t)quarter_wave[16];
    }
    else
    {
        rem = (in * 16U) % len;
        mag = (uint32_t)((int32_t)quarter_wave[pos] +
                         (((int32_t)quarter_wave[pos + 1] -
                           (int32_t)quarter_wave[pos]) * (int32_t)rem) /
                         (int32_t)len);
    }
    return (int16_t)((quad >= 2U) ? -(int32_t)mag : (int32_t)mag);
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b != 0U)
    {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

/**
 * @ingroup audio
 *
 * Frames del anillo para un tono exacto de @p hz hercios, 0 si no cabe.
 *
 * Con fs=48000 y una frecuencia entera, un periodo mide base = 48000/mcd(hz,
 * 48000) frames y solo ese numero cierra el bucle sin costura; cualquier
 * multiplo suyo tambien, y se toma el mas largo que quepa en las dos paginas
 * porque cuanto mas largo el anillo, menos vueltas del motor por segundo.  Es
 * lo que hace que 1000 Hz suene a 1000: su periodo son 48 frames, y el anillo
 * de 42 periodos son 2016.
 */
static uint32_t ring_for(uint32_t hz, uint32_t *periods_out)
{
    uint32_t g = gcd(hz, ES8311_SAMPLE_RATE_HZ);
    uint32_t base = ES8311_SAMPLE_RATE_HZ / g;
    uint32_t factor;

    if (base > OUT_MAX_FRAMES)
    {
        return 0U;
    }
    factor = OUT_MAX_FRAMES / base;
    *periods_out = (hz / g) * factor;
    return base * factor;
}

/**
 * @ingroup audio
 *
 * Frecuencia aplicable mas cercana a la pedida.  Se prueba la pedida y, si su
 * periodo no cabe exacto en el anillo, se aparta un hercio, dos, y asi hasta la
 * barra de barrido; entre las dos candidatas de un mismo desplazamiento se
 * queda con la del anillo mas largo, que es la que hace menos vueltas.
 */
static bool tone_pick(uint32_t req_hz, uint32_t *hz_out,
                      uint32_t *periods_out, uint32_t *frames_out)
{
    uint32_t slack;

    if (req_hz < OUT_MIN_HZ || req_hz > OUT_MAX_HZ)
    {
        return false;
    }

    for (slack = 0U; slack <= OUT_FREQ_SLACK; slack++)
    {
        uint32_t best_hz = 0U, best_len = 0U, best_periods = 0U;
        uint32_t k;

        for (k = 0U; k < (slack == 0U ? 1U : 2U); k++)
        {
            uint32_t hz = (k == 0U) ? (req_hz + slack) : (req_hz - slack);
            uint32_t periods, len;

            if (hz < OUT_MIN_HZ || hz > OUT_MAX_HZ)
            {
                continue;
            }
            len = ring_for(hz, &periods);
            if (len > best_len)
            {
                best_len = len;
                best_hz = hz;
                best_periods = periods;
            }
        }

        if (best_len != 0U)
        {
            *hz_out = best_hz;
            *periods_out = best_periods;
            *frames_out = best_len;
            return true;
        }
    }
    return false;
}

/** Escribir el anillo de muestras por el acceso directo del CPU. */
static void ring_fill(uint32_t periods, uint32_t frames)
{
    volatile int16_t *s = (volatile int16_t *)audio_buf_cpu;
    uint32_t i;

    for (i = 0U; i < frames; i++)
    {
        int16_t v = sine_at((i * periods) % frames, frames);

        /* Los dos slots con el mismo valor: la placa tiene un altavoz, y el
         * codigo no tiene por que inventarse un segundo canal. */
        s[2U * i] = v;
        s[2U * i + 1U] = v;
    }
}

/**
 * @ingroup audio
 *
 * Dejar montada la lista ciclica de descriptores y devolver la direccion del
 * primero tal y como la ve el motor.  Con el bit de fin de lista a 0 en todos
 * y el ultimo apuntando al primero, la lista no termina nunca: el motor la
 * repasa mientras el I2S pida datos.
 */
static uint32_t link_fill(uint32_t frames)
{
    volatile link_desc *d = (volatile link_desc *)audio_desc_cpu;
    uint32_t desc_base = (uint32_t)(uintptr_t)_audio_desc_page;
    uint32_t buf_base = (uint32_t)(uintptr_t)_audio_buf_page;
    uint32_t count = (frames + OUT_CHUNK_FRAMES - 1U) / OUT_CHUNK_FRAMES;
    uint32_t done = 0U;
    uint32_t i;

    for (i = 0U; i < count; i++)
    {
        uint32_t chunk = frames - done;
        uint32_t bytes;

        if (chunk > OUT_CHUNK_FRAMES)
        {
            chunk = OUT_CHUNK_FRAMES;
        }
        bytes = chunk * OUT_FRAME_BYTES;

        d[i].dw0 = 0U;
        d[i].buffer = buf_base + done * OUT_FRAME_BYTES;
        d[i].next = desc_base + ((i + 1U) % count) * (uint32_t)sizeof(*d);
        d[i].dw0 = bytes | (bytes << DESC_LEN_SHIFT);

        MEM_BARRIER();
        d[i].dw0 |= DESC_OWNER_DMA;
        done += chunk;
    }

    MEM_BARRIER();
    return desc_base;
}

/** Bajar el telon en el orden inverso al de subida. */
static void pipeline_teardown(void)
{
    if (ampOn)
    {
        gpio_write(I2S_AMP_POWER, false);
        ampOn = false;
    }
    (void)es8311_set_mute(true);

    /* La captura va en los relojes de la salida: el RX del P4 es esclavo y el
     * MCLK sale del dominio TX, asi que pararlos con una captura en marcha
     * dejaria al codec sin reloj y al motor de entrada aparcado en medio de un
     * buffer.  Se baja primero ella. */
    if (i2s_rx_running())
    {
        audio_in_stop();
    }

    ahbdma_tx_stop();
    (void)i2s_clocks_stop();
    ringFrames = 0U;
    streaming = false;
    running = false;
}

/**
 * @ingroup audio
 *
 * Lanzar lo que va despues del patron y la lista, igual para el tono que para
 * el stream: relojes, cuenta de sincronizacion a cero, enlace del motor, nivel
 * del DAC, quitar el silencio y conectar el amplificador lo ultimo.  Esta
 * secuencia es la validada en placa con el tono, asi que el stream la reutiliza
 * tal cual en lugar de tener una propia.
 *
 * @param desc_addr primer descriptor, en la forma que ve el motor
 * @param frames  frames del anillo que acaba de montarse
 * @return true si la salida quedo sonando
 */
static bool pipeline_launch(uint32_t desc_addr, uint32_t frames)
{
    int vol;

    if (!i2s_clocks_start())
    {
        return false;
    }
    /* Cuenta desde aqui: lo que se lea despues es datos emitidos por este
     * arranque, no lo que dejo el anterior. */
    i2s_tx_sync_reset();
    if (!ahbdma_tx_link(desc_addr))
    {
        (void)i2s_clocks_stop();
        return false;
    }

    /* El volumen se respeta si el usuario lo subio antes; con el codec a cero
     * (su valor de reset) se arranca en el nivel de prueba.  Se reescribe
     * SIEMPRE, aunque el registro ya devuelva el valor pedido: quien pidio el
     * nivel lo pidio con el MCLK parado, y la ganancia no la fija el registro
     * sino el motor de fade del DAC, que solo trabaja con reloj.  Fiarse del
     * readback dejaba muda la primera salida despues de arrancar.  Aqui el
     * reloj ya esta vivo, asi que esta es la primera escritura que cuenca. */
    vol = es8311_get_volume();
    if (vol == 0 || vol == SYSERR)
    {
        vol = OUT_START_VOLUME;
    }
    if (es8311_set_volume((uint32_t)vol) == SYSERR)
    {
        pipeline_teardown();
        return false;
    }
    if (!es8311_set_mute(false))
    {
        pipeline_teardown();
        return false;
    }

    /* Ultima pieza: la senal ya esta saliendo del DAC, asi que el
     * amplificador se conecta sobre un patron estable y no sobre el transient
     * del arranque. */
    gpio_set_dir(I2S_AMP_POWER, GPIO_OUTPUT);
    gpio_write(I2S_AMP_POWER, true);
    ampOn = true;

    running = true;
    ringFrames = frames;
    return true;
}

/**
 * @ingroup audio
 *
 * Arrancar el tono.  El orden es el que evita el golpe: primero el patron y la
 * lista (el DMA aun no corre y el I2S no da reloj), luego el resto.
 */
bool audio_out_start(uint32_t req_hz, uint32_t *applied_hz,
                     uint32_t *ring_frames)
{
    uint32_t hz, periods, frames, desc_addr;

    if (running)
    {
        pipeline_teardown();
    }
    stream_release();

    if (!tone_pick(req_hz, &hz, &periods, &frames))
    {
        return false;
    }

    ring_fill(periods, frames);
    desc_addr = link_fill(frames);

    if (!pipeline_launch(desc_addr, frames))
    {
        return false;
    }

    if (applied_hz != NULL)
    {
        *applied_hz = hz;
    }
    if (ring_frames != NULL)
    {
        *ring_frames = frames;
    }
    return true;
}

void audio_out_stop(void)
{
    pipeline_teardown();
    stream_release();
}

uint32_t audio_out_status(void)
{
    uint32_t raw = ahbdma_tx_raw();
    uint32_t st = 0U;

    if (running)
    {
        st |= AUDIO_OUT_ST_ON;
    }
    if (ahbdma_tx_running())
    {
        st |= AUDIO_OUT_ST_LINK;
    }
    if ((raw & 0x1U) != 0U)
    {
        st |= AUDIO_OUT_ST_DMA_DONE;
    }
    if ((raw & 0x2U) != 0U)
    {
        st |= AUDIO_OUT_ST_DMA_EOF;
    }
    if ((raw & 0x4U) != 0U)
    {
        st |= AUDIO_OUT_ST_DSCR_ERR;
    }
    if (i2s_tx_started())
    {
        st |= AUDIO_OUT_ST_TX_START;
    }
    if (i2s_tx_idle())
    {
        st |= AUDIO_OUT_ST_TX_IDLE;
    }
    if (ampOn)
    {
        st |= AUDIO_OUT_ST_AMP;
    }
    if (streaming)
    {
        st |= AUDIO_OUT_ST_STREAM;
    }
    return st;
}

uint32_t audio_out_dma_desc(void)
{
    return ahbdma_tx_current_desc();
}

uint32_t audio_out_sync_count(void)
{
    return i2s_tx_sync_count();
}

uint32_t audio_out_ring_frames(void)
{
    return ringFrames;
}

/**
 * @ingroup audio
 *
 * Bytes emitidos por el I2S desde el ultimo arranque.  El contador de
 * sincronizacion cuenta muestras de 16 bits y cada trama son dos, de ahi el
 * factor; es la unica posicion de lectura que da el periferico, y ya se uso en
 * la medida de flujo del tono.
 */
static uint32_t stream_emitted(void)
{
    return i2s_tx_sync_count() * 2U;
}

/**
 * @ingroup audio
 *
 * Trozos enlazados sobre el anillo de PSRAM, ciclicos y cedidos al motor, con
 * la misma receta que el patron del tono: el bit de dueno se pone al final,
 * despues de la barrera.  La lista no cambia nunca; lo que cambia es el
 * contenido del anillo.
 */
static void stream_link_build(void)
{
    volatile link_desc *d = streamDesc;
    uint32_t i;

    for (i = 0U; i < STREAM_CHUNKS; i++)
    {
        d[i].dw0 = 0U;
        d[i].buffer = streamRingBus + i * STREAM_CHUNK_BYTES;
        d[i].next = streamDescBus +
                    ((i + 1U) % STREAM_CHUNKS) * (uint32_t)sizeof(link_desc);
        d[i].dw0 = STREAM_CHUNK_BYTES | (STREAM_CHUNK_BYTES << DESC_LEN_SHIFT);
        MEM_BARRIER();
        d[i].dw0 |= DESC_OWNER_DMA;
    }
    MEM_BARRIER();
}

/** Bajar el anillo de stream; se llama con el enlace ya parado. */
static void stream_release(void)
{
    streamDesc = NULL;
    streamRing = NULL;
    streamWritten = 0U;
    streamGaps = 0U;
    streaming = false;
}

/**
 * @ingroup audio
 *
 * Montar la salida en modo stream.  El orden es el del tono, sin cambiar una
 * pieza: primero el anillo (en silencio) y su lista, y despues relojes, enlace,
 * codec y amplificador por pipeline_launch(), que es la secuencia validada.
 */
bool audio_out_stream_start(void)
{
    if (running)
    {
        pipeline_teardown();
    }
    stream_release();

    /* El heap de PSRAM devuelve la forma con cache, que es la que ve el motor;
     * el CPU se pone encima por el acceso directo, como en audioIn.c. */
    streamDescBus = (uint32_t)(uintptr_t)_audio_stream_desc_page;
    streamRingBus = (uint32_t)(uintptr_t)_audio_stream_buf_page;
    streamDesc = (volatile link_desc *)(uintptr_t)(streamDescBus +
                                                   AUDIO_CPU_UNCACHED);
    streamRing = (uint8_t *)(uintptr_t)(streamRingBus + AUDIO_CPU_UNCACHED);

    /* Anillo a silencio antes de enlazar: si el motor da una vuelta entera
     * porque quien escribe tarda, lo que suena es silencio, no el resto de la
     * memoria que hubo ahi antes. */
    psram_zero(streamRing, STREAM_RING_BYTES);
    stream_link_build();

    if (!pipeline_launch(streamDescBus, STREAM_RING_FRAMES))
    {
        stream_release();
        return false;
    }

    streamWritten = 0U;
    streaming = true;
    return true;
}

/**
 * @ingroup audio
 *
 * Entregar PCM al anillo.  La cabeza de escritura va por detras del punto por
 * el que lee el motor, medida la distancia con el contador de sincronizacion;
 * cuando la distancia llega al tamano del anillo menos el margen de lectura por
 * delante, se duerme una espera corta y se reintenta, que es lo que sostiene el
 * ritmo sin necesidad de cola ni de interrupciones.
 *
 * Se escriben siempre tramas enteras y sin dar la vuelta dentro de la misma
 * copia, porque un trozo que cruza el final del anillo son dos zonas del
 * descriptor distintas.  La copia va de palabra en palabra (psramcopy.h): el
 * anillo esta en la ventana directa de PSRAM y a byte la ventana no da los
 * 192.000 bytes/s que consume el I2S, que es el ritmo al que hay que ir
 * escribiendo para no dejar huecos en la cola del motor.
 */
uint32_t audio_out_stream_write(const void *buf, uint32_t nbytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t sent = 0U;
    uint32_t stalls = 0U;
    uint32_t want = nbytes & ~(uint32_t)(OUT_FRAME_BYTES - 1U);

    while (sent < want)
    {
        int32_t ahead;
        uint32_t room, pos, n;

        if (!streaming)
        {
            break;
        }

        /* Negativo quiere decir que el motor ya dio la vuelta y esta sonando
         * silencio: no hay nada en cola y el anillo esta entero libre.  Con PCM
         * ya escrito eso es un hueco de reproduccion, y es lo unico que lo
         * delata: el contador de muestras del I2S sigue sumando igual y el
         * write() no devuelve menos de lo que le pidieron. */
        ahead = (int32_t)(streamWritten - stream_emitted());
        if (ahead < 0)
        {
            if (0U != streamWritten)
            {
                streamGaps++;
            }
            ahead = 0;
        }
        if ((uint32_t)ahead + STREAM_GUARD_BYTES < STREAM_RING_BYTES)
        {
            room = STREAM_RING_BYTES - STREAM_GUARD_BYTES - (uint32_t)ahead;
        }
        else
        {
            room = 0U;
        }

        if (0U == room)
        {
            /* Sin hueco: el reloj del I2S es el que vacia, asi que se espera.
             * El tope es para el caso en que el reloj no vaya (parada por
             * consola o enlace caido): sin el, la escritura no volvia. */
            if (++stalls > STREAM_STALL_LIMIT)
            {
                break;
            }
            sleep(2);
            continue;
        }

        pos = streamWritten % STREAM_RING_BYTES;
        n = STREAM_RING_BYTES - pos;
        if (n > room)
        {
            n = room;
        }
        if (n > want - sent)
        {
            n = want - sent;
        }
        n &= ~(uint32_t)(OUT_FRAME_BYTES - 1U);
        if (0U == n)
        {
            if (++stalls > STREAM_STALL_LIMIT)
            {
                break;
            }
            sleep(2);
            continue;
        }

        stalls = 0U;
        psram_copy(streamRing + pos, src + sent, n);
        streamWritten += n;
        sent += n;
    }

    return sent;
}

/** Esperar a que lo ultimo escrito salga por el altavoz. */
void audio_out_stream_drain(void)
{
    uint32_t target = streamWritten;
    uint32_t stalls = 0U;

    /* Lo maximo que queda por sonar es una vuelta del anillo, un tercio de
     * segundo; el tope es para no colgarse si el reloj se paro por otro lado. */
    while (streaming && (int32_t)(target - stream_emitted()) > 0)
    {
        if (++stalls > 2 * STREAM_STALL_LIMIT)
        {
            break;
        }
        sleep(2);
    }

    /* Lo ultimo que queda dentro del contador cuando el anillo se vacia es lo
     * que el motor tiene por delante: dos trozos. */
    if (streaming)
    {
        sleep(STREAM_GUARD_MS);
    }
}

uint32_t audio_out_stream_queued(void)
{
    int32_t ahead;

    if (!streaming)
    {
        return 0U;
    }

    ahead = (int32_t)(streamWritten - stream_emitted());
    return (ahead > 0) ? (uint32_t)ahead / OUT_FRAME_BYTES : 0U;
}

uint32_t audio_out_stream_capacity(void)
{
    return streaming ? (STREAM_RING_BYTES / OUT_FRAME_BYTES) : 0U;
}

bool audio_out_streaming(void)
{
    return streaming;
}

/** Direccion del anillo de stream tal y como la ve el motor. */
uint32_t audio_out_stream_base(void)
{
    return streaming ? streamRingBus : 0U;
}

/** Huecos de reproduccion: veces que el motor alcanzo al escritor con PCM ya
 *  escrito.  Cada una es lo que falta de una vuelta de anillo sonado en silencio
 *  o repetido, y no se ve por ningun otro sitio. */
uint32_t audio_out_stream_gaps(void)
{
    return streamGaps;
}
