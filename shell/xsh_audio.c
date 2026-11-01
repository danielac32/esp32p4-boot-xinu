/**
 * @file     xsh_audio.c
 *
 * Shell command (audio): el estado del codec ES8311, sus mandos de I2C
 * (volumen, mute, ganancia del micro), los relojes I2S, la etapa de salida --que
 * monta un patron de tono o un anillo de reproduccion-- y las tres cosas que se
 * hacen con el device por read() y write(): reproducir un fichero, grabar del
 * microfono y volver a escuchar lo grabado.  Todo a traves de /dev/audio0; los
 * valores que se muestran estan leidos del propio codec o de los registros del
 * periferico, no de un cache del driver, que es lo que hace de esto una prueba
 * del bus y del DMA.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <device.h>
#include <audio.h>
#include <memory.h>
#include <fsutil.h>

/* Duracion del tono cuando `audio out 440' no la pide.  La shell se queda
 * bloqueada ese tiempo y luego para la salida: sin ese tope el altavoz no
 * calla hasta que alguien recuerde apagarlo. */
#define AUDIO_TONE_DEFAULT_MS   1000U
#define AUDIO_TONE_MAX_MS       30000U

/* Ventana con la que se mide el flujo del I2S en el estado: dos lecturas del
 * contador separadas este tiempo.  Los suenos del kernel van 9/8 largos (ver
 * P4_TIMER_FREQ), asi que la cifra real es algo mayor que la que se imprime. */
#define AUDIO_FLOW_WINDOW_MS    200U

/**
 * @ingroup shell
 *
 * Decimas de dB leidas del codec, con su signo.  Se imprimen aparte porque el
 * porcentaje del mando `vol' es lineal sobre el registro y el registro es lineal
 * en decibelios: sin esta cifra el numero de arriba mentiria sobre lo que se oye.
 */
static void print_db10(int db10)
{
    int mag;

    if (SYSERR == db10)
    {
        printf("(el codec no responde)");
        return;
    }

    mag = db10 < 0 ? -db10 : db10;
    printf("(%s%d,%d dB)", db10 < 0 ? "-" : "+", mag / 10, mag % 10);
}

static void usage(char *prog)
{
    printf("Usage: %s [vol N | mute | unmute | gain N | pga N | clocks [off] |"
           " out HZ [ms] | out off |\n\t       play FICHERO | rec MS | loop |"
           " in [on|off]]\n\n", prog);
    printf("Description:\n");
    printf("\tCodec ES8311 status, with the values read back over I2C.\n");
    printf("\t  vol N     salida DAC al 0..100 %%\n");
    printf("\t  mute      silenciar la salida\n");
    printf("\t  unmute    quitar el silencio\n");
    printf("\t  gain N    escala digital del ADC: 0=0 dB .. 7=42 dB\n");
    printf("\t  pga N     ganancia analogica del micro: 0=0 .. 10=+30 dB\n");
    printf("\t  clocks    arrancar MCLK/BCLK/LRCK y medir el BCLK real\n");
    printf("\t  clocks off parar los relojes\n");
    printf("\t  out HZ    tono de HZ hercios (50..8000) durante %u ms\n",
           AUDIO_TONE_DEFAULT_MS);
    printf("\t  out HZ MS el mismo tono durante MS milisegundos\n");
    printf("\t  out off   parar la salida ahora\n");
    printf("\t  play F    reproducir un WAV (o PCM crudo) por el altavoz\n");
    printf("\t  rec MS    grabar MS milisegundos del micro a PSRAM\n");
    printf("\t  loop      reproducir lo ultimo grabado\n");
    printf("\t  in on     montar la captura y dejarla andando\n");
    printf("\t  in off    parar la captura\n");
    printf("\t  in        estado de la captura (motor, I2S y codec)\n");
    printf("\n");
    printf("Sin argumentos se muestra el estado actual.\n");
    printf("La frecuencia pedida se cuantifica al valor mas cercano que cierra\n");
    printf("el bucle del DMA sin costura; se imprime el que realmente suena.\n");
    printf("El %% de vol es lineal sobre el registro del DAC, y el registro es\n");
    printf("lineal en decibelios (0,5 dB/paso): 75 %% = 0 dB, 100 %% = +32 dB.\n");
    printf("El audio por fichero va a 48 kHz y 16 bits; el mono se duplica a los\n");
    printf("dos slots y los formatos con perdida (MP3, AAC, MP4) no se pueden:\n");
    printf("hace falta un decodificador y aqui no entra ni un byte comprimido.\n");
}

/**
 * @ingroup shell
 *
 * Estado del camino de salida, traducido de los bits que devolvieron los
 * registros del DMA y del I2S.  Con la salida montada se mide ademas el
 * incremento del contador de sincronizacion, que es la unica prueba de flujo
 * que da el periferico: el FIFO del P4 no tiene registro de llenado porque lo
 * vacia el DMA, no la CPU.
 */
static void show_out_status(int audiofd)
{
    uint32_t st = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_STAT, 0, 0);
    uint32_t desc = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_DESC, 0, 0);
    uint32_t frames = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_RING, 0, 0);
    uint32_t before, after;

    if ((int)st == SYSERR)
    {
        printf("salida:   el device no responde\n");
        return;
    }

    printf("salida:   %s\n", (st & AUDIO_OUT_ST_ON) ? "montada" : "parada");
    printf("dma:      %s, descriptor actual 0x%08x\n",
           (st & AUDIO_OUT_ST_LINK) ? "enlazado" : "aparcado", desc);
    if (st & AUDIO_OUT_ST_DSCR_ERR)
    {
        printf("dma:      ERROR de descriptor rechazado\n");
    }
    printf("i2s tx:   %s, fsm %s\n",
           (st & AUDIO_OUT_ST_TX_START) ? "arrancado" : "parado",
           (st & AUDIO_OUT_ST_TX_IDLE) ? "idle" : "dando reloj");
    printf("amp:      GPIO53 %s\n", (st & AUDIO_OUT_ST_AMP) ? "activo" : "apagado");

    if (frames != 0U)
    {
        printf("anillo:   %u frames, una vuelta cada %u,%02u ms\n",
               frames, frames / 48U, (frames % 48U) * 100U / 48U);
    }

    if (st & AUDIO_OUT_ST_ON)
    {
        uint32_t win_ms;
        uint32_t rate;

        before = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CNT, 0, 0);
        sleep(AUDIO_FLOW_WINDOW_MS);
        after = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CNT, 0, 0);

        /* El ritmo solo dice algo si la ventana es la de verdad, y aqui no lo
         * es: P4_TIMER_FREQ da un sueno 9/8 largo.  Se corrige en la medida, no
         * en el sueno; cuando se arregle lo primero, sobra el factor.  La
         * multiplicacion va en milisegundos para no pasarse de 2^32. */
        win_ms = AUDIO_FLOW_WINDOW_MS * 9U / 8U;
        rate = (after - before) * 1000U / win_ms;
        printf("i2s tx:   %u muestras de 16 bits, %u/s (se esperan 96.000)\n",
               after, rate);
    }

    /* El numero de descriptores leidos por el motor es la prueba de que la
     * lista de PSRAM existe: 0 significa que aun no ha mirado. */
    if (0 == desc)
    {
        printf("dma:      no ha leido ningun descriptor\n");
    }
}

/* --- reproduccion y captura por el device ------------------------------ */

/* El formato del cable lo fija la etapa de salida: 48 kHz con dos slots de 16
 * bits, o sea 192 bytes por milisegundo.  Un WAV en otra tasa se rechaza en
 * lugar de remuestrearse: aqui no hay sitio para un conversor, y un fichero a
 * 44,1 kHz sonando a 48 no es una reproduccion, es una nota larga. */
#define AUDIO_BYTES_PER_MS      192U
#define AUDIO_PLAY_OUT_BYTES    4096U
#define AUDIO_PLAY_IN_BYTES     (AUDIO_PLAY_OUT_BYTES / 2U)
#define AUDIO_REC_CHUNK_BYTES   4096U
#define AUDIO_REC_MAX_MS        30000U
#define AUDIO_SEC_MS            1000U

/* Lo que cabe en el anillo de captura del driver (32 trozos de 512 tramas) son
 * 341 ms; se dan por perdidos nueve intentos de ese tamaño, o sea tres segundos
 * sin que el motor entregue una sola trama. */
#define AUDIO_REC_SILENT_LIMIT  300U

/* Decimas en que se parte una toma para pintar su envolvente (ver audio_rec). */
#define REC_ENVELOPE            10U

static uint8_t playIn[AUDIO_PLAY_IN_BYTES];
static uint8_t playOut[AUDIO_PLAY_OUT_BYTES];

static void show_in_status(int audiofd);

/* Lo que capta el micro se queda en PSRAM entre el rec y el loop: 30 s son
 * 5,76 MB, y el heap de PSRAM tiene los 31 MB del chip menos lo que ocupan los
 * anillos de las dos etapas.
 *
 * El buffer se maneja por el acceso directo del CPU (0x88......), no por la
 * via con cache (0x48......): es la misma norma que siguen las paginas de los
 * anillos en ld.script y en device/sdmmc.  Mover el PCM por la via con cache
 * es lo que coincide con el corte de la grabacion, y el precedente ya esta
 * escrito en el script de enlaces -- una barrida grande de PSRAM por esa via
 * deja lineas de L2MEM leidas a cero, que es como murio la tabla de handlers
 * del CLIC.  A 192 kB/s el acceso directo sobra. */
#define REC_PCM_DIRECT      0x40000000U

static uint8_t *recPcm;
static uint32_t recLen;
static uint32_t recCap;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Segundos con dos decimales, impresos sin coma flotante. */
static void print_seconds(uint32_t bytes)
{
    uint32_t ms = bytes / AUDIO_BYTES_PER_MS;

    printf("%u,%02u s", ms / AUDIO_SEC_MS, (ms % AUDIO_SEC_MS) / 10U);
}

/**
 * @ingroup shell
 *
 * Mirar si el fichero es RIFF/WAVE y si su audio cabe en el camino de salida.
 * Deja el puntero al empezar los datos y dice cuantos bytes hay.
 *
 * @return 1 con un WAV util, 0 si no es RIFF (se toma como PCM crudo en el
 *         formato del cable) o -1 si es WAV pero en un formato que la etapa no
 *         admite.
 */
static int wav_probe(void *f, uint32_t *channelsOut, uint32_t *bytesOut)
{
    uint8_t hdr[12];
    uint8_t ch[8];
    uint8_t fmt[16];
    uint32_t size, dataBytes = 0U, seen = 0U;
    uint16_t format, channels, bits;
    uint32_t rate;

    if (12 != fl_fread(hdr, 1, 12, f))
    {
        return 0;
    }
    if (0 != memcmp(hdr, "RIFF", 4) || 0 != memcmp(hdr + 8, "WAVE", 4))
    {
        return 0;
    }

    /* Bloque detras de bloque hasta dar con "data": los demas (LIST, fact,
     * cue, los metadatos que escriben las herramientas) se saltan con su
     * relleno a par, que es la regla del RIFF. */
    for (;;)
    {
        if (8 != fl_fread(ch, 1, 8, f))
        {
            break;
        }
        size = le32(ch + 4);

        if (0 == memcmp(ch, "fmt ", 4))
        {
            if (16 != fl_fread(fmt, 1, 16, f))
            {
                return -1;
            }
            format = le16(fmt);
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (size > 16U)
            {
                (void)fl_fseek(f, (long)(size - 16U), SEEK_CUR);
            }
            if ((size & 1U) != 0U)
            {
                (void)fl_fseek(f, 1, SEEK_CUR);
            }
            seen |= 1U;
            continue;
        }

        if (0 == memcmp(ch, "data", 4))
        {
            dataBytes = size;
            seen |= 2U;
            break;
        }

        (void)fl_fseek(f, (long)(size + (size & 1U)), SEEK_CUR);
    }

    if ((seen & 3U) != 3U)
    {
        fprintf(stderr, "audio play: WAV sin bloque fmt o sin bloque data\n");
        return -1;
    }
    if (1U != format)
    {
        fprintf(stderr, "audio play: formato %u; solo se admite PCM entero "
                        "(el MP3/AAC/MP4 llevan dentro un codo que aqui no "
                        "se decodifica)\n", format);
        return -1;
    }
    if (16U != bits)
    {
        fprintf(stderr, "audio play: %u bits por muestra; la etapa va a 16\n",
                bits);
        return -1;
    }
    if (ES8311_SAMPLE_RATE_HZ != rate)
    {
        fprintf(stderr, "audio play: %u Hz; los relojes I2S van fijos a %u Hz "
                        "y aqui no se remuestrea\n",
                rate, ES8311_SAMPLE_RATE_HZ);
        return -1;
    }
    if (1U != channels && 2U != channels)
    {
        fprintf(stderr, "audio play: %u canales; la placa tiene un altavoz y "
                        "dos slots\n", channels);
        return -1;
    }
    if (0U == dataBytes)
    {
        fprintf(stderr, "audio play: el bloque data esta vacio\n");
        return -1;
    }

    *channelsOut = channels;
    *bytesOut = dataBytes;
    return 1;
}

/**
 * @ingroup shell
 *
 * Pasar el fichero por el device.  Cada lectura entra en el anillo de PSRAM de
 * la salida y el write() se queda dormido si el anillo esta a tope, que es lo
 * que marca el ritmo: no hay temporizador propio en este comando.
 */
static shellcmd audio_play(int audiofd, const char *arg)
{
    char path[FS_PATH_MAX];
    void *f;
    uint32_t channels = 2U;
    uint32_t left = 0U;
    uint32_t total = 0U;
    bool raw = false;
    int kind;

    if (!fs_mounted())
    {
        fprintf(stderr, "audio play: no hay tarjeta montada\n");
        return SYSERR;
    }
    if (!fs_resolve(arg, path, sizeof(path)))
    {
        fprintf(stderr, "audio play: %s no cabe en una ruta\n", arg);
        return SYSERR;
    }

    f = fl_fopen(path, "rb");
    if (NULL == f)
    {
        fprintf(stderr, "audio play: %s no existe\n", arg);
        return SYSERR;
    }

    kind = wav_probe(f, &channels, &left);
    if (kind < 0)
    {
        fl_fclose(f);
        return SYSERR;
    }
    if (0 == kind)
    {
        raw = true;
        printf("play:     sin cabecera RIFF: se toma como PCM crudo de 16 "
               "bits estereo a %u Hz\n", ES8311_SAMPLE_RATE_HZ);
    }
    else
    {
        printf("play:     %s, %u canal%s, %u Hz, ", path, channels,
               (channels == 1U) ? "" : "es", ES8311_SAMPLE_RATE_HZ);
        print_seconds(left);
        printf(" de datos\n");
    }

    if (SYSERR == control(audiofd, AUDIO_CTRL_STREAM, 1, 0))
    {
        fprintf(stderr, "audio play: el anillo de reproduccion no se ha "
                        "podido montar\n");
        fl_fclose(f);
        return SYSERR;
    }
    printf("anillo:   reproduccion en 0x%08x, %u tramas\n",
           (uint32_t)control(audiofd, AUDIO_CTRL_STREAM_BASE, 0, 0),
           (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CAP, 0, 0));

    while (raw || left > 0U)
    {
        uint32_t want = (2U == channels) ? AUDIO_PLAY_OUT_BYTES
                                         : AUDIO_PLAY_IN_BYTES;
        uint32_t out;
        int got, sent;

        if (!raw && want > left)
        {
            want = left;
        }

        if (2U == channels)
        {
            got = fl_fread(playOut, 1, (int)want, f);
            if (got <= 0)
            {
                break;
            }
            /* Tramas enteras: los restos de una trama incompleta no salen. */
            out = (uint32_t)got & ~3U;
        }
        else
        {
            const int16_t *m = (const int16_t *)playIn;
            int16_t *s = (int16_t *)playOut;
            uint32_t i, samples;

            got = fl_fread(playIn, 1, (int)want, f);
            if (got <= 0)
            {
                break;
            }
            samples = (uint32_t)got / 2U;
            for (i = 0U; i < samples; i++)
            {
                /* Un altavoz, dos slots: la misma muestra en los dos. */
                s[2U * i] = m[i];
                s[2U * i + 1U] = m[i];
            }
            out = samples * 4U;
        }

        if (!raw)
        {
            left -= (uint32_t)got;
        }
        if (0U == out)
        {
            continue;
        }

        sent = (int)write(audiofd, playOut, out);
        if (sent <= 0)
        {
            fprintf(stderr, "audio play: el device ha rechazado %u bytes\n",
                    out);
            break;
        }
        total += (uint32_t)sent;
    }

    fl_fclose(f);

    printf("play:     ");
    print_seconds(total);
    printf(" enviados; esperando a que suene lo ultimo\n");
    (void)control(audiofd, AUDIO_CTRL_STREAM_WAIT, 0, 0);
    /* El contador de huecos se limpia al desmontar la salida, asi que se mira
     * antes de apagar. */
    printf("play:     %u muestras por el I2S, %u huecos de reproduccion\n",
           (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CNT, 0, 0),
           (uint32_t)control(audiofd, AUDIO_CTRL_STREAM_GAPS, 0, 0));
    (void)control(audiofd, AUDIO_CTRL_OUT_OFF, 0, 0);
    return OK;
}

/** Dejar en @p cur el maximo en valor absoluto entre @p cur y la muestra. */
static uint32_t mag_of(uint32_t cur, int32_t v)
{
    uint32_t mag = (uint32_t)((v < 0) ? -v : v);

    return (mag > cur) ? mag : cur;
}

/** Maximo valor absoluto de las muestras recien llegadas.
 *
 * El buffer de la toma esta en la ventana directa de PSRAM, donde cada acceso es
 * una transaccion por el AHB sin burst, asi que se leen dos muestras por
 * lectura.  Si la punta no cae en borde de palabra se recorre de dos en dos con
 * lecturas de byte: es el unico modo de no provocar un acceso desalineado, que
 * este core no resuelve. */
static uint32_t peak_of(uint32_t cur, const uint8_t *p, uint32_t nbytes)
{
    const uint8_t *end = p + nbytes;

    if (0U != ((uintptr_t)p & 3U))
    {
        while (end - p >= 2)
        {
            cur = mag_of(cur, (int32_t)le16(p));
            p += 2U;
        }
        return cur;
    }

    while (end - p >= 4)
    {
        uint32_t w = *(const uint32_t *)p;

        cur = mag_of(cur, (int32_t)(int16_t)(w & 0xFFFFU));
        cur = mag_of(cur, (int32_t)(int16_t)(w >> 16));
        p += 4U;
    }

    while (end - p >= 2)
    {
        cur = mag_of(cur, (int32_t)le16(p));
        p += 2U;
    }
    return cur;
}

/**
 * @ingroup shell
 *
 * Grabar del microfono a PSRAM.  El read() del device no espera: entrega lo
 * que el motor ya ha llenado y 0 si va por mitad de un trozo, asi que el bucle
 * duerme esas esperas cortas.  El pico se mira sobre las tramas recien
 * copiadas, que es lo que dice si por el micro entro señal o solo el ruido de
 * fondo del ADC.
 *
 * Ademas del pico total se guarda el de cada decima parte de la toma: el pico
 * solo no distingue "el micro no capta" de "el sesgo analogico del PGA todavia
 * estaba subiendo cuando se abrio la captura", y esa diferencia es la que decide
 * donde hay que mirar.
 */
static shellcmd audio_rec(int audiofd, uint32_t ms)
{
    uint32_t wanted = ms * AUDIO_BYTES_PER_MS;
    uint32_t peak = 0U;
    uint32_t slicePeak[REC_ENVELOPE];
    uint32_t sliceBytes;
    uint32_t silent = 0U;
    uint32_t i;
    long dropped;
    long st;

    if (NULL == recPcm)
    {
        uint8_t *cached;

        recCap = (uint32_t)AUDIO_REC_MAX_MS * AUDIO_BYTES_PER_MS;
        cached = (uint8_t *)psram_malloc(recCap);
        if (NULL == cached)
        {
            fprintf(stderr, "audio rec: sin hueco de %u bytes en PSRAM\n",
                    recCap);
            return SYSERR;
        }
        recPcm = (uint8_t *)((uintptr_t)cached + REC_PCM_DIRECT);
        printf("rec:      buffer de captura en 0x%08x (%u bytes)\n",
               (uint32_t)(uintptr_t)recPcm, recCap);
    }
    if (wanted > recCap)
    {
        wanted = recCap;
    }

    sliceBytes = wanted / REC_ENVELOPE;
    if (0U == sliceBytes)
    {
        sliceBytes = 1U;
    }
    for (i = 0U; i < REC_ENVELOPE; i++)
    {
        slicePeak[i] = 0U;
    }

    st = control(audiofd, AUDIO_CTRL_IN, 1, 0);
    if (SYSERR == st)
    {
        fprintf(stderr, "audio rec: la captura no ha arrancado\n");
        show_in_status(audiofd);
        return SYSERR;
    }

    recLen = 0U;
    while (recLen < wanted)
    {
        uint32_t chunk = wanted - recLen;
        int got;

        if (chunk > AUDIO_REC_CHUNK_BYTES)
        {
            chunk = AUDIO_REC_CHUNK_BYTES;
        }

        got = (int)read(audiofd, recPcm + recLen, chunk);
        if (got < 0)
        {
            fprintf(stderr, "audio rec: el read ha fallado\n");
            (void)control(audiofd, AUDIO_CTRL_IN, 0, 0);
            return SYSERR;
        }
        if (0 == got)
        {
            /* El motor solo entrega cuando tiene un trozo entero llenado; si
             * pasan tres segundos sin entregar nada es que el reloj o el
             * enlace no van, y no tiene sentido seguir esperando. */
            if (++silent > AUDIO_REC_SILENT_LIMIT)
            {
                fprintf(stderr, "audio rec: la captura no entrega tramas\n");
                show_in_status(audiofd);
                (void)control(audiofd, AUDIO_CTRL_IN, 0, 0);
                return SYSERR;
            }
            sleep(2);
            continue;
        }

        /* El ES8311 es mono: vuelca la muestra solo en el slot izquierdo y deja
         * el derecho a 0, y en esta placa el DAC reproduce el slot contrario del
         * que saca el ADC.  Sin duplicar, `loop` y un WAV guardado salen mudos
         * aunque la toma tenga senal (medido: L al 100 %, R todo ceros).  Se copia
         * slot0 sobre slot1 trama a trama, igual que el patron del tono con los
         * dos slots. */
        {
            uint8_t *p = recPcm + recLen;
            uint32_t k;

            for (k = 0U; k + 3U < (uint32_t)got; k += 4U)
            {
                p[k + 2U] = p[k];
                p[k + 3U] = p[k + 1U];
            }
        }

        silent = 0U;
        i = recLen / sliceBytes;
        if (i >= REC_ENVELOPE)
        {
            i = REC_ENVELOPE - 1U;
        }

        /* Una sola barrida para el pico y para la envolvente: el trozo cae
         * dentro de una decima y las dos cifras son el maximo del mismo rango. */
        {
            uint32_t piece = peak_of(0U, recPcm + recLen, (uint32_t)got);

            if (piece > peak)
            {
                peak = piece;
            }
            if (piece > slicePeak[i])
            {
                slicePeak[i] = piece;
            }
        }
        recLen += (uint32_t)got;
    }

    dropped = control(audiofd, AUDIO_CTRL_IN_DROP, 0, 0);
    (void)control(audiofd, AUDIO_CTRL_IN, 0, 0);

    /* Los relojes los subio la propia captura (el RX es esclavo del TX).  Si
     * no hay nada sonando no hace falta dejarlos: se apagan. */
    st = control(audiofd, AUDIO_CTRL_OUT_STAT, 0, 0);
    if (SYSERR != st && (st & AUDIO_OUT_ST_ON) == 0L)
    {
        (void)control(audiofd, AUDIO_CTRL_CLOCKS, 0, 0);
    }

    printf("rec:      ");
    print_seconds(recLen);
    printf(" grabados, pico %u / 32767 (%u %%)\n", peak,
           (unsigned)(peak * 100U / 32767U));
    printf("rec:      envolvente, %u decimas de %u ms:",
           (unsigned)REC_ENVELOPE,
           (unsigned)(sliceBytes / AUDIO_BYTES_PER_MS));
    for (i = 0U; i < REC_ENVELOPE; i++)
    {
        printf(" %u", (unsigned)(slicePeak[i] * 100U / 32767U));
    }
    printf(" %%\n");
    if (0U == peak)
    {
        printf("rec:      todo a cero: el micro no ha llegado al ADC\n");
    }
    if (dropped > 0)
    {
        printf("rec:      %d tramas perdidas por no consumir el anillo a "
               "tiempo\n", (int)dropped);
    }
    printf("rec:      `audio loop` para escucharlo\n");
    return OK;
}

/**
 * @ingroup shell
 *
 * Reproducir lo grabado, tal cual y sin tocar el fichero de la SD.
 */
static shellcmd audio_loop(int audiofd)
{
    uint32_t off = 0U;
    uint32_t emitted;
    uint32_t gaps;

    if (0U == recLen)
    {
        fprintf(stderr, "audio loop: no hay nada grabado (usa `audio rec "
                        "MILISEGUNDOS`)\n");
        return SYSERR;
    }

    if (SYSERR == control(audiofd, AUDIO_CTRL_STREAM, 1, 0))
    {
        fprintf(stderr, "audio loop: el anillo de reproduccion no se ha "
                        "podido montar\n");
        return SYSERR;
    }

    printf("loop:     %u tramas desde 0x%08x, anillo en 0x%08x\n",
           recLen / 4U, (uint32_t)(uintptr_t)recPcm,
           (uint32_t)control(audiofd, AUDIO_CTRL_STREAM_BASE, 0, 0));

    while (off < recLen)
    {
        uint32_t n = recLen - off;
        int sent;

        if (n > AUDIO_PLAY_OUT_BYTES)
        {
            n = AUDIO_PLAY_OUT_BYTES;
        }
        n &= ~3U;
        if (0U == n)
        {
            break;
        }

        sent = (int)write(audiofd, recPcm + off, n);
        if (sent <= 0)
        {
            fprintf(stderr, "loop:      el device ha rechazado la escritura\n");
            break;
        }
        off += (uint32_t)sent;
    }

    /* Huecos: veces que el motor dio la vuelta por delante de quien escribia.
     * Mientras se escribe es cuando dicen algo, porque el contador se limpia al
     * desmontar la salida y en el vaciado ya no hay escritor al que alcanzar. */
    gaps = (uint32_t)control(audiofd, AUDIO_CTRL_STREAM_GAPS, 0, 0);
    printf("loop:     %u tramas escritas, %u muestras sacadas ya, %u huecos\n",
           off / 4U,
           (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CNT, 0, 0), gaps);

    (void)control(audiofd, AUDIO_CTRL_STREAM_WAIT, 0, 0);

    /* Lo que dice si de verdad ha sonado: el contador de sincronizacion del I2S
     * cuenta muestras de 16 bits y se pone a cero en cada arranque del stream,
     * asi que aqui vale 2 x tramas si el DMA ha sacado lo escrito.  Contar solo
     * lo aceptado por write() no prueba nada: eso se queda en el anillo aunque
     * el periferico no lo lea. */
    emitted = (uint32_t)control(audiofd, AUDIO_CTRL_OUT_CNT, 0, 0);
    (void)control(audiofd, AUDIO_CTRL_OUT_OFF, 0, 0);
    printf("loop:     %u tramas reproducidas, %u muestras por el I2S "
           "(esperadas %u), %u huecos\n", off / 4U, emitted, off / 2U, gaps);
    return OK;
}

/**
 * @ingroup shell
 *
 * Estado de la captura, todo leido del motor, del I2S y del codec.
 */
static void show_in_status(int audiofd)
{
    uint32_t st = (uint32_t)control(audiofd, AUDIO_CTRL_IN_STAT, 0, 0);
    long dropped;

    if ((int)st == SYSERR)
    {
        printf("captura:  el device no responde\n");
        return;
    }

    printf("captura:  %s\n", (st & AUDIO_IN_ST_ON) ? "montada" : "parada");
    if ((st & AUDIO_IN_ST_ON) == 0U)
    {
        return;
    }

    dropped = control(audiofd, AUDIO_CTRL_IN_DROP, 0, 0);
    printf("dma in:   %s, descriptor 0x%08x\n",
           (st & AUDIO_IN_ST_LINK) ? "enlazado" : "APARCADO",
           (uint32_t)control(audiofd, AUDIO_CTRL_IN_DESC, 0, 0));
    printf("dma eof:  descriptor 0x%08x, %u bytes en cola\n",
           (uint32_t)control(audiofd, AUDIO_CTRL_IN_EOF, 0, 0),
           (uint32_t)control(audiofd, AUDIO_CTRL_IN_QUEUE, 0, 0));
    printf("anillo:   %u bytes en 0x%08x, %d tramas perdidas\n",
           (uint32_t)control(audiofd, AUDIO_CTRL_IN_RING, 0, 0),
           (uint32_t)control(audiofd, AUDIO_CTRL_IN_BASE, 0, 0),
           (int)dropped);
    printf("i2s rx:   %s, micro: %s\n",
           (st & AUDIO_IN_ST_RX) ? "arrancado" : "parado",
           (st & AUDIO_IN_ST_MIC) ? "abierto" : "cerrado");
    if (st & AUDIO_IN_ST_DSCR_ERR)
    {
        printf("dma in:   ERROR de descriptor rechazado\n");
    }
    if (st & AUDIO_IN_ST_ERR_EOF)
    {
        printf("dma in:   fin de recepcion con error\n");
    }

    {
        int pga = (int)control(audiofd, AUDIO_CTRL_MIC_GET, 0, 0);
        int esc = (int)control(audiofd, AUDIO_CTRL_MIC_SCALE, 0, 0);
        int adc = (int)control(audiofd, AUDIO_CTRL_MIC_ADCSV, 0, 0);

        if (pga == SYSERR || esc == SYSERR || adc == SYSERR)
        {
            printf("codec:    la ruta del micro no responde\n");
        }
        else
        {
            /* El volumen digital del ADC es lineal en decibelios con paso de
             * 0,5 y el 0 dB cae en el mismo registro que el del DAC. */
            printf("codec:    pga %d = +%d dB analogicos, escala %d = +%d dB, "
                   "digital del adc %d ",
                   pga, pga * 3, esc, esc * 6, adc);
            print_db10((adc - ES8311_VOL_0DB_REG) * ES8311_VOL_STEP_DB10);
            printf("\n");
        }
    }
}

/**
 * @ingroup shell
 *
 * Shell command (audio)
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_audio(int nargs, char *args[])
{
    int audiofd;
    int volume;
    int mute;
    long value;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        usage(args[0]);
        return OK;
    }

    audiofd = getdev("/dev/audio0");
    if (SYSERR == audiofd)
    {
        fprintf(stderr, "%s: /dev/audio0 is not mounted\n", args[0]);
        return SYSERR;
    }

    if (nargs > 4)
    {
        fprintf(stderr, "%s: too many arguments\n", args[0]);
        close(audiofd);
        return SYSERR;
    }

    /* ---- etapa de salida: el unico sitio que hace sonido -------------- */
    if (nargs >= 2 && 0 == strcmp(args[1], "out"))
    {
        uint32_t hz, ms;
        long applied;

        if (2 == nargs)
        {
            /* `audio out' sin frecuencia: se cae antes de tocar el DMA. */
            fprintf(stderr, "%s: falta la frecuencia\n", args[0]);
            close(audiofd);
            return SYSERR;
        }

        if (0 == strcmp(args[2], "off"))
        {
            value = control(audiofd, AUDIO_CTRL_OUT_OFF, 0, 0);
            close(audiofd);
            if (SYSERR == value)
            {
                fprintf(stderr, "%s: no se ha podido parar\n", args[0]);
                return SYSERR;
            }
            printf("salida parada\n");
            return OK;
        }

        hz = (uint32_t)atoi(args[2]);
        ms = (4 == nargs) ? (uint32_t)atoi(args[3]) : AUDIO_TONE_DEFAULT_MS;
        if (0 == hz || 0 == ms || ms > AUDIO_TONE_MAX_MS)
        {
            fprintf(stderr, "%s: frecuencia fuera de 50..8000 o"
                            " duracion fuera de 1..%u ms\n",
                    args[0], AUDIO_TONE_MAX_MS);
            close(audiofd);
            return SYSERR;
        }

        applied = control(audiofd, AUDIO_CTRL_OUT, hz, 0);
        if (SYSERR == applied)
        {
            fprintf(stderr, "%s: la salida no ha arrancado\n", args[0]);
            show_out_status(audiofd);
            close(audiofd);
            return SYSERR;
        }

        printf("tono aplicado: %d Hz (pedido %u Hz)\n", (int)applied, hz);

        /* El nivel de lo que se va a oir lo manda el registro del DAC, asi que
         * se imprime aqui: es el numero que explica que suene o no. */
        {
            int db10 = (int)control(audiofd, AUDIO_CTRL_GET_VOLUME_DB, 0, 0);

            printf("dac:      %d %% ",
                   (int)control(audiofd, AUDIO_CTRL_GET_VOLUME, 0, 0));
            print_db10(db10);
            if (db10 != SYSERR && db10 < -60)
            {
                printf("  (el registro esta por debajo de -6 dB:"
                       " sube con `audio vol 75', que es el 0 dB)");
            }
            printf("\n");
        }

        sleep(ms);
        show_out_status(audiofd);
        value = control(audiofd, AUDIO_CTRL_OUT_OFF, 0, 0);
        if (SYSERR == value)
        {
            fprintf(stderr, "%s: el amplificador sigue conectado\n", args[0]);
            close(audiofd);
            return SYSERR;
        }
        printf("salida parada\n");
        close(audiofd);
        return OK;
    }

    /* ---- ficheros y microfono: el device por read() y write() --------- */
    if (nargs == 3 && 0 == strcmp(args[1], "play"))
    {
        shellcmd rc = audio_play(audiofd, args[2]);

        close(audiofd);
        return rc;
    }

    if (nargs == 3 && 0 == strcmp(args[1], "rec"))
    {
        uint32_t ms = (uint32_t)atoi(args[2]);
        shellcmd rc;

        if (0U == ms || ms > AUDIO_REC_MAX_MS)
        {
            fprintf(stderr, "%s: duracion fuera de 1..%u ms\n", args[0],
                    AUDIO_REC_MAX_MS);
            close(audiofd);
            return SYSERR;
        }

        rc = audio_rec(audiofd, ms);
        close(audiofd);
        return rc;
    }

    if (nargs == 2 && 0 == strcmp(args[1], "loop"))
    {
        shellcmd rc = audio_loop(audiofd);

        close(audiofd);
        return rc;
    }

    if (nargs >= 2 && nargs <= 3 && 0 == strcmp(args[1], "in"))
    {
        int rc = OK;

        if (3 == nargs && 0 == strcmp(args[2], "on"))
        {
            rc = (int)control(audiofd, AUDIO_CTRL_IN, 1, 0);
        }
        else if (3 == nargs && 0 == strcmp(args[2], "off"))
        {
            rc = (int)control(audiofd, AUDIO_CTRL_IN, 0, 0);
        }
        else
        {
            show_in_status(audiofd);
        }

        close(audiofd);
        return (SYSERR == rc) ? SYSERR : OK;
    }

    if (nargs == 3 && 0 == strcmp(args[1], "pga"))
    {
        int requested = atoi(args[2]);

        if (SYSERR == control(audiofd, AUDIO_CTRL_MIC_PGA, requested, 0))
        {
            fprintf(stderr, "%s: el codec no ha respondido\n", args[0]);
            close(audiofd);
            return SYSERR;
        }
        printf("pga leido del codec: %d = +%d dB\n",
               (int)control(audiofd, AUDIO_CTRL_MIC_GET, 0, 0), requested * 3);
        close(audiofd);
        return OK;
    }

    if (2 == nargs && 0 == strcmp(args[1], "clocks"))
    {
        uint32_t measured;

        if (SYSERR == control(audiofd, AUDIO_CTRL_CLOCKS, 1, 0))
        {
            fprintf(stderr, "%s: los relojes no han arrancado\n", args[0]);
            close(audiofd);
            return SYSERR;
        }

        measured = (uint32_t)control(audiofd, AUDIO_CTRL_BCLK, 50000, 0);
        if (0 == measured || SYSERR == (int)measured)
        {
            fprintf(stderr, "%s: el contador de BCLK no ha medido nada\n",
                    args[0]);
            close(audiofd);
            return SYSERR;
        }

        printf("MCLK:   %u Hz esperado\n", ES8311_MCLK_HZ);
        printf("BCLK:   %u Hz esperado, %u Hz medido\n", I2S_BCLK_HZ, measured);
        printf("LRCK:   %u Hz esperada\n", ES8311_SAMPLE_RATE_HZ);
        close(audiofd);
        return OK;
    }

    if (2 == nargs)
    {
        if (0 == strcmp(args[1], "mute"))
        {
            value = 1;
        }
        else if (0 == strcmp(args[1], "unmute"))
        {
            value = 0;
        }
        else if (0 == strcmp(args[1], "vol") || 0 == strcmp(args[1], "gain"))
        {
            /* `audio vol 60' y `audio gain 4' llegan como dos tokens. */
            fprintf(stderr, "%s: falta el valor\n", args[0]);
            close(audiofd);
            return SYSERR;
        }
        else
        {
            fprintf(stderr, "%s: comando desconocido\n", args[0]);
            close(audiofd);
            return SYSERR;
        }

        if (SYSERR == control(audiofd, AUDIO_CTRL_MUTE, value, 0))
        {
            fprintf(stderr, "%s: el codec no ha respondido\n", args[0]);
            close(audiofd);
            return SYSERR;
        }
        close(audiofd);
        return OK;
    }

    if (3 == nargs && 0 == strcmp(args[1], "clocks") &&
        0 == strcmp(args[2], "off"))
    {
        if (SYSERR == control(audiofd, AUDIO_CTRL_CLOCKS, 0, 0))
        {
            fprintf(stderr, "%s: no se han podido parar\n", args[0]);
            close(audiofd);
            return SYSERR;
        }
        printf("relojes I2S parados\n");
        close(audiofd);
        return OK;
    }

    if (3 == nargs)
    {
        int cmd;
        int requested;

        requested = atoi(args[2]);
        if (0 == strcmp(args[1], "vol"))
        {
            cmd = AUDIO_CTRL_VOLUME;
        }
        else if (0 == strcmp(args[1], "gain"))
        {
            cmd = AUDIO_CTRL_MIC_GAIN;
        }
        else
        {
            fprintf(stderr, "%s: comando desconocido\n", args[0]);
            close(audiofd);
            return SYSERR;
        }

        value = control(audiofd, cmd, requested, 0);
        if (SYSERR == value)
        {
            fprintf(stderr, "%s: el codec no ha respondido\n", args[0]);
            close(audiofd);
            return SYSERR;
        }

        if (AUDIO_CTRL_VOLUME == cmd)
        {
            printf("volumen leido del codec: %d %% ", (int)value);
            print_db10((int)control(audiofd, AUDIO_CTRL_GET_VOLUME_DB, 0, 0));
            printf("\n");
        }
        else
        {
            int scale = (int)control(audiofd, AUDIO_CTRL_MIC_SCALE, 0, 0);

            printf("escala digital del adc leida del codec: %d = +%d dB\n",
                   scale, scale * 6);
        }
        close(audiofd);
        return OK;
    }

    /* Sin argumentos: estado, con los valores leidos del codec. */
    volume = control(audiofd, AUDIO_CTRL_GET_VOLUME, 0, 0);
    mute = control(audiofd, AUDIO_CTRL_GET_MUTE, 0, 0);

    if (SYSERR == volume || SYSERR == mute)
    {
        fprintf(stderr, "%s: el codec no responde\n", args[0]);
        close(audiofd);
        return SYSERR;
    }

    printf("codec:    ES8311 en 0x18\n");
    printf("volumen:  %d %% ", volume);
    print_db10((int)control(audiofd, AUDIO_CTRL_GET_VOLUME_DB, 0, 0));
    printf("\n");
    printf("salida i2c: %s\n", mute ? "muteada" : "activa");
    printf("relojes:  %s\n", i2s_clocks_running() ? "corriendo" : "parados");
    show_out_status(audiofd);
    show_in_status(audiofd);
    close(audiofd);

    return OK;
}
