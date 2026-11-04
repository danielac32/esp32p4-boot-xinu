/**
 * @file     xsh_wifi.c
 *
 * Shell command (wifi): W1 trae la sonda de enumeración SDIO del ESP32-C6
 * sobre el slot 1 del controlador SDMMC compartido; W2.c añade `wifi up`, que
 * levanta el transporte esp_hosted portado verbatim desde
 * /home/daniel/esp32-riscv-bare-metal-sdk-main/drivers/esp_hosted.  W2.d añade
 * `wifi ver`, la primera RPC de control (versión del firmware del C6) con
 * esp_hosted_ctrl.c del mismo SDK, y `wifi dump` para ver las tramas tal como
 * suben del esclavo.  Todo lo que se imprime se lee del propio periférico
 * (registros del bloque SDHOST y de la función 1 del slave vía CMD53), no de un
 * caché del driver: eso es lo que convierte el comando en una prueba del bus.
 * Los estados de red (IP, SSID) llegan en etapas posteriores (W4) y aún no
 * existen.
 *
 * Espera: los bucles de sondeo de este fichero usan sleep(), que es la espera
 * de hebra de Xinu (cede la CPU y despierta al tick); delay_ms() es mdelay(),
 * un giro sobre el contador, y solo queda justificada en el driver, donde hay
 * tiempos de registro o interrupciones enmascaradas.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <device.h>
#include <interrupt.h>
#include <kernel.h>
#include <semaphore.h>
#include <shell.h>
#include <timer.h>
#include <sdio.h>
#include <espeth.h>
#include <esp_hosted.h>
#include <esp_hosted_ctrl.h>
#include <esp_hosted_sdio.h>

/* Escaneo (`wifi scan') y barrido del esquema RPC (`wifi sweep') viven en
 * shell/xsh_wifi_scan.c: el SDK de referencia no implementa escaneo, asi que
 * hay que medir el id en placa y decodificar el resultado a mano. */
shellcmd wifiSweep(uint32 from, uint32 to);
shellcmd wifiScanList(void);
shellcmd wifiScanRaw(uint32 req_id, const char *hex);

/* Señal del enlace activo (`wifi signal'), leída del RPC 294 en
 * shell/xsh_wifi_scan.c.  wifiSignalQuality() pasa los dBm a 0..100 para que
 * el dashboard no tenga que hacer la conversión ni divisiones con coma. */
int wifiSignalRead(int32 *rssi, uint32 *channel);
int wifiSignalQuality(int32 rssi);

/* Nombre del endpoint H2S con el que espHostedCtrl.c etiqueta la peticion; vive
 * en el driver como variable (no como macro) justamente para poder medir los dos
 * candidatos desde el shell sin recompilar. */
extern const char *esp_hosted_ctrl_ep_name;

/* Escalón de reintento del ejemplo de referencia
 * (examples/esp_hosted/source/main.c): arranque con el config por defecto,
 * luego con pull-ups internos, y al final 1 bit a 5 MHz. */
#define WIFI_TRIES  3

static void wifi_print_frame(const esp_hosted_frame_info_t *info,
                             const uint8_t *payload, size_t len);
static void wifi_priv_rx(const esp_hosted_frame_info_t *info,
                         const uint8_t *payload, size_t len, void *ctx);

static void wifi_usage(const char *cmd)
{
    printf("Usage: %s up | down | diag | ver | dump | probe | jam | poke | init | mac | stats | signal | join | leave | scan [id] | sweep <de> <a> | sdio | fn1 | dma | pio | slow | bus | dat | wires | hunt | hold | pads\n\n",
           cmd);
    printf("Description:\n");
    printf("\t'up'    attaches the esp_hosted SDIO transport on SDMMC slot 1\n");
    printf("\t        (ESP32-C6) and reports the registers read from the slave.\n");
    printf("\t'diag'  dumps the shared SDHOST clocks, the DMA pages and the fn1\n");
    printf("\t        registers, all read from the peripheral itself.\n");
    printf("\t'ver'    first control RPC: ask the C6 for its firmware version\n");
    printf("\t         (TLV+protobuf over the transport, 3 s timeout).\n");
    printf("\t'dump'  print every esp_hosted frame the slave already pushed into\n");
    printf("\t        the RX queue: decoded header + first 32 payload bytes.\n");
    printf("\t'probe' blast PRIV frames with the RX-throttle threshold at 1%% and\n");
    printf("\t        read fn1 INT_RAW: bit 7 proves our host->slave bytes are\n");
    printf("\t        parsed by the slave's esp_hosted layer, not just by SDIO.\n");
    printf("\t'jam'   24 SERIAL RPC requests (> the slave's 20-deep RX queue) and\n");
    printf("\t        then the 'probe' oracle: if the oracle still fires, the\n");
    printf("\t        slave's SERIAL consumer is alive and our request content is\n");
    printf("\t        what is refused; if credits freeze, pserial_task is wedged.\n");
    printf("\t'poke'  four hand-built RPC requests (ids 257/259/277/350) sent raw\n");
    printf("\t        on the SERIAL channel, listening half a second after each:\n");
    printf("\t        which ones make the C6 write bytes back, and with what\n");
    printf("\t        protobuf field numbers (the dump prints the answer).\n");
    printf("\t'init'  Req_WifiInit (id 278): brings up the wifi stack inside the\n");
    printf("\t        C6; without it every other wifi RPC answers 0x3001.\n");
    printf("\t'mac'   Req_GetMacAddress (id 257): the MAC the C6 reports, which is\n");
    printf("\t        the control-path proof this NCP can actually answer.\n");
    printf("\t'down'  detaches the transport (slot 1 idle again).\n");
    printf("\t'join'  associates the STA: 278 init + 260 mode STA + 284 config\n");
    printf("\t        + 280 start + 282 connect, waiting for StaConnected.\n");
    printf("\t'scan'  lists the APs the C6 sees: 286 starts the sweep, 289 is the\n");
    printf("\t        record count and 294 fetches record i (bssid/ssid/channel).\n");
    printf("\t'scan <id>'  raw probe of one RPC id, dumping every frame for 60 s\n");
    printf("\t        -- the tool the ids above were measured with.\n");
    printf("\t'sweep' send an empty request per id in a range and print each\n");
    printf("\t        answer field by field, to measure this NCP's RPC scheme.\n");
    printf("\t'stats' attach/association flags, transport queue and ETH0 counters.\n");
    printf("\t'signal' RSSI (dBm) y porcentaje de senal del AP asociado (RPC 294).\n");
    printf("\t'sdio'  enumerates the ESP32-C6 on SDMMC slot 1 and prints the\n");
    printf("\t        SDIO registers read from the device (W1 probe).\n");
    printf("\t'fn1'   brings up function 1 and probes the CMD53/CMD52 data\n");
    printf("\t        path against the esp_hosted slave registers (W2.1).\n");
    printf("\t'dma'   CMD53 data-phase diagnostic over the internal DMA (IDMAC),\n");
    printf("\t        4-bit and 1-bit, register by register.\n");
    printf("\t'pio'   same CMD53 transfers with the IDMAC OFF: the words are\n");
    printf("\t        moved by hand through BUFFIFO, which separates the DMA\n");
    printf("\t        engine from the physical layer.\n");
    printf("\t'slow'  PIO transfers swept over the slot clock (400 kHz..20 MHz):\n");
    printf("\t        the data phase was only ever tried at 20 MHz, where the\n");
    printf("\t        schematic's 51k pull-ups on DAT0..DAT3 and the GPIO matrix\n");
    printf("\t        routing of slot 1 bite hardest.\n");
    printf("\t'bus'   who drives the data lines: each slot-1 pad left tri-stated\n");
    printf("\t        with an internal pull-up then pull-down, with the C6 alive,\n");
    printf("\t        held in reset and just released (GPIO54 verified first).\n");
    printf("\t'dat'   cause and effect on DAT0: disarm CCCR_INT_ENABLE and ack\n");
    printf("\t        INT_RAW into INT_CLR over CMD52, re-read the tri-stated pad,\n");
    printf("\t        retry CMD53 at rest, then drive D0/D1 push-pull to rule out\n");
    printf("\t        a shorted net (D0/gpio14 y D1/gpio15 no obedecieron).\n");
    printf("\t'wires' ask the wire instead of the pin map: snapshot all 48 P4\n");
    printf("\t        receivers with the slave interrupt pending, after the ack\n");
    printf("\t        and after ringing the H2S doorbell; the pad that flips is\n");
    printf("\t        the real DAT0.\n");
    printf("\t'hunt'  sweep the CIU's DAT0 INPUT pad by pad (FUNC_IN_SEL_CFG[2])\n");
    printf("\t        and read 8 CCCR bytes by CMD53 over each one: the pad whose\n");
    printf("\t        bytes match the CMD52 read is the real DAT0 wire.  Then it\n");
    printf("\t        moves the DAT0 OUTPUT there too and proves the reverse\n");
    printf("\t        direction by acking INT_RAW with a CMD53 write.\n");
    printf("\t'hold'  pads 0..15 have a second owner on the P4: LP_IOMUX with a\n");
    printf("\t        MUX_SEL bit, and every pad has a hold bit (LP_PAD_HOLD\n");
    printf("\t        0x5012B04C for 0..15, HP O_HOLD_CTRL0/1 0x500E5074/78 for\n");
    printf("\t        16..56) that freezes the pad even though the HP register\n");
    printf("\t        still takes the write.  Dump both, drive/pull-test pads 14\n");
    printf("\t        and 15, release hold and MUX_SEL for 14..19 only, re-test\n");
    printf("\t        and retry the CMD53 read against the CMD52 truth.\n");
    printf("\t--help  display this help and exit\n");
}

/**
 * @ingroup shell
 *
 * Shell command (wifi up).  Adjunta el transporte esp_hosted con la escalera
 * de reintentos de la referencia.  Comparte el bloque SDHOST con la microSD,
 * así que el driver sólo escribe los campos del slot 1 y toma el monitor del
 * bus alrededor de cada transacción; tras esto hay que poder seguir leyendo
 * el FAT sin despeinarse (`ls` de toda la vida).
 */
static shellcmd wifi_up(void)
{
    esp_hosted_sdio_config_t cfg;
    static const char *label[WIFI_TRIES] = { "default", "pullups", "safe-1bit" };
    uint32 try;
    uint32 i;
    shellcmd rc = SYSERR;
    /* El attach reescribe el estado global del transporte (s_ctx) y solo hay
     * un C6: medido en placa, si la hebra de arranque y un `wifi up' a mano
     * lo lanzan a la vez, el segundo memset deja al primero sin bus y los dos
     * se van por "host init failed attempt=1..3".  Un semaforo de a 1
     * serializa la subida. */
    static bool upSemMade = FALSE;
    static semaphore upSem;

    if (!upSemMade)
    {
        irqmask im = disable();

        if (!upSemMade)
        {
            upSem = semcreate(1);
            upSemMade = TRUE;
        }
        restore(im);
    }
    wait(upSem);

    esp_hosted_sdio_get_default_config(&cfg);

    /* 1.6 KB: fuera de la pila del shell */
    {
        static esp_hosted_packet_t pkt;

        for (try = 0; try < WIFI_TRIES; try++)
        {
            printf("wifi: intentando %s (slot=%u width=%u clk=%u kHz pullups=%u)...\n",
                   label[try], (unsigned)cfg.slot, (unsigned)cfg.bus_width,
                   (unsigned)cfg.clock_khz, (unsigned)cfg.use_internal_pullups);

            if (esp_hosted_sdio_attach(&cfg))
            {
                printf("wifi: C6 adjunto en el slot %u a %u kHz (%u bits)\n",
                       (unsigned)cfg.slot, (unsigned)cfg.clock_khz,
                       (unsigned)cfg.bus_width);
                esp_hosted_sdio_diag_print();

                /* Intento de leer la primera trama que suba el slave.  La PRIV de
                 * inicio no es solo diagnostico: es el evento que abre el handshake,
                 * asi que ademas de volcarse se entrega a wifi_priv_rx(), que manda
                 * las capacidades del host.  Sin eso el C6 ignora cualquier RPC. */
                for (i = 0; i < 20U; i++)
                {
                    esp_hosted_poll();
                    if (esp_hosted_rx_pop(&pkt))
                    {
                        printf("wifi: primera trama del slave:\n");
                        wifi_print_frame(&pkt.info, pkt.payload, pkt.len);
                        if (pkt.info.if_type == ESP_HOSTED_IF_PRIV)
                        {
                            wifi_priv_rx(&pkt.info, pkt.payload, pkt.len, NULL);
                        }
                        rc = OK;
                        break;
                    }
                    sleep(2);
                }

                if (SYSERR == rc)
                {
                    printf("wifi: sin tramas del slave (aun no hay RPC en vuelo)\n");
                    rc = OK;
                }
                break;
            }

            if (try == 0U)
            {
                cfg.use_internal_pullups = true;
            }
            else if (try == 1U)
            {
                cfg.bus_width = 1U;
                cfg.clock_khz = 5000U;
            }
        }

        if (SYSERR == rc)
        {
            fprintf(stderr, "wifi: el C6 no adjunto\n");

            /* El slot 1 y la microSD comparten el bloque SDHOST; un attach que aborta
             * puede dejar el motor aparca-do (CMD_START clavado).  Se devuelve el bus
             * al estado de arranque y se comprueba leyendo el LBA 0 de verdad, para que
             * `ls` siga vivo despues de un intento fallido. */
            sdioRestoreSlot0();
        }
    }

    signal(upSem);
    return rc;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi down).  Suelta el transporte; deja el slot 1 apagado.
 */
static shellcmd wifi_down(void)
{
    if (!esp_hosted_sdio_is_attached())
    {
        printf("wifi: no habia nada adjunto\n");
        return OK;
    }
    esp_hosted_sdio_detach();
    printf("wifi: transporte esp_hosted desadjuntado\n");
    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi diag).  Volteo de registros leidos del periférico.
 */
static shellcmd wifi_diag(void)
{
    if (!esp_hosted_sdio_is_attached())
    {
        printf("wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }
    esp_hosted_sdio_diag_print();
    return OK;
}

/**
 * @ingroup shell
 *
 * `wifi stats': estado del enlace y contadores planos del camino de datos.
 *
 * Existe porque la pila puede mandar y no recibir, y desde fuera esas dos
 * cosas se parecen: un `netup' por DHCP agotado puede ser que la DHCPDISCOVER
 * no salio, o que del C6 no bajo ninguna trama.  tx_frames los cuenta
 * espethWrite cuando el transporte escribio la trama entera, rx_frames los
 * cuenta espethInput cuando el dispatch baja una trama STA/AP/ETH, y las
 * caídas dicen si el anillo de 4 ranuras se lleno.  `pendientes' es lo que el
 * C6 tiene preparado para subir y aun no hemos leido.
 */
static shellcmd wifi_stats(void)
{
    struct espeth_stats st;

    printf("wifi: adjunto=%u sta_connected=%u ctrl_ready=%u\n",
           (unsigned)esp_hosted_sdio_is_attached(),
           (unsigned)esp_hosted_wifi_sta_is_connected(),
           (unsigned)esp_hosted_ctrl_is_ready());
    printf("wifi: cola del transporte: pendientes=%u descartadas=%u\n",
           (unsigned)esp_hosted_rx_pending(), (unsigned)esp_hosted_rx_dropped());

#if NETHER
    /* El enlace se busca por nombre en el devtab, no por la macro: control()
     * rechaza un numero malo (isbaddev) y aqui se imprime el nombre resuelto,
     * que es lo que el driver realmente atiende. */
    int descrp = getdev("ETH0");

    if (OK == control(descrp, ESPETH_CTRL_STATS, (long)&st, 0))
    {
        printf("wifi: %s tx=%u caidas_tx=%u rx=%u caidas_rx=%u\n",
               devtab[descrp].name,
               (unsigned)st.tx_frames, (unsigned)st.tx_drops,
               (unsigned)st.rx_frames, (unsigned)st.rx_drops);
    }
    else
    {
        printf("wifi: no hay ETH0 arriba o el driver no responde por STATS\n");
    }
#endif

    return OK;
}

/**
 * @ingroup shell
 *
 * `wifi signal': cuanta señal hay del AP con el que estamos asociados.
 *
 * Pide el registro del enlace activo (RPC 294) e imprime los dBm y el porcentaje
 * que usara la pantalla.  Es una RPC con su espera, de unos pocos milisegundos
 * cuando el C6 responde y de hasta 3 s cuando no hay enlace: por eso el dash no
 * debe llamarla en cada refresco, sino una vez cada varios segundos.
 */
static shellcmd wifi_signal(void)
{
    int32 rssi = 0;
    uint32 channel = 0U;

    if (OK != wifiSignalRead(&rssi, &channel))
    {
        fprintf(stderr, "wifi: signal: el C6 no da el AP asociado"
                        " (¿hay enlace? prueba `wifi join')\n");
        return SYSERR;
    }

    printf("wifi: signal: rssi=%d dBm  senal=%d%%  canal=%u\n",
           (int)rssi, wifiSignalQuality(rssi), (unsigned)channel);

    return OK;
}

/**
 * @ingroup shell
 *
 * Volcado de una trama esp_hosted ya desencabezada.  Se imprime SIEMPRE la
 * cabecera decodificada (if_type, len, offset, seq) y los primeros 32 bytes del
 * payload en hexadecimal y en ASCII: es la unica forma de saber en placa si lo
 * que sube el C6 es el TLV/protobuf que espera la capa de control, o si trae un
 * encabezado priv extra que hay que saltar antes del TLV.
 */
static void wifi_print_frame(const esp_hosted_frame_info_t *info,
                             const uint8_t *payload, size_t len)
{
    static const char *ifname[] = {
        "invalid", "sta", "ap", "serial", "hci", "priv", "test", "eth"
    };
    unsigned i;
    unsigned shown = (len < 32U) ? (unsigned)len : 32U;
    const char *nm = ((unsigned)info->if_type < 8U) ? ifname[(unsigned)info->if_type]
                                                    : "?";

    printf("trama if=%u(%s) ifnum=%u flags=0x%02x len=%u off=%u seq=%u "
           "chk=%u privtype=%u hcitype=%u\n",
           (unsigned)info->if_type, nm, (unsigned)info->if_num,
           (unsigned)info->flags, (unsigned)info->payload_len,
           (unsigned)info->payload_offset, (unsigned)info->seq_num,
           (unsigned)info->checksum, (unsigned)info->priv_pkt_type,
           (unsigned)info->hci_pkt_type);

    if (payload == NULL || len == 0U)
    {
        printf("  (vacia)\n");
        return;
    }

    for (i = 0U; i < shown; i++)
    {
        printf((i % 16U == 0U) ? "  %02x" : " %02x", payload[i]);
    }
    printf("\n  ascii: ");
    for (i = 0U; i < shown; i++)
    {
        printf("%c", (payload[i] >= 0x20U && payload[i] < 0x7FU) ?
                 (char)payload[i] : '.');
    }
    printf("%s\n", len > shown ? " ..." : "");
}

/* ---------------------------------------------------------------------------
 * Handshake PRIV de arranque (copiado del ejemplo de referencia
 * examples/esp_hosted/source/main.c).  El C6 sube por el canal PRIV (if_type 5,
 * priv_pkt_type 0x33) una trama que empieza con 0x22 = EVENT_INIT y sigue con
 * 0x0c = longitud (12) de una lista de TLV de 1+1+n:
 *      12 01 0d   FIRMWARE_CHIP_ID = 0x0d  (ESP32-C6)
 *      11 01 0d   CAPABILITY       = 0x0d
 *      13 01 00   TEST_RAW_TP      = 0
 *      14 01 14   RX_Q_SIZE        = 20    <- los 20 creditos TX que medimos
 * El host DEVE contestar con sus propias capacidades por el mismo PRIV; la
 * referencia no lanza ningun RPC hasta tener esa respuesta enviada Y
 * esp_hosted_ctrl_is_ready().  Nosotros no lo haciamos: por eso el RPC de
 * version moria en silencio.
 * ------------------------------------------------------------------------- */
#define HOSTED_PRIV_PKT_EVENT        0x33U  /* valor del aux/priv_pkt_type H2S */
#define HOSTED_PRIV_EVENT_INIT       0x22U  /* evento de inicio del esclavo */
#define HOSTED_PRIV_CAPABILITY       0x11U
#define HOSTED_PRIV_FIRMWARE_CHIP_ID 0x12U
#define HOSTED_PRIV_CAP_EXT          0x16U
#define HOSTED_PRIV_TRANS_SDIO_MODE  0x18U
#define HOSTED_HOST_CAPABILITIES     0x44U  /* tag H2S: capacidades del host */
#define HOSTED_HOST_SLAVE_CHIP_ID    0x45U
#define HOSTED_HOST_RAW_TP           0x46U
#define HOSTED_HOST_THROTTLE_HIGH    0x47U
#define HOSTED_HOST_THROTTLE_LOW     0x48U

static bool s_hosted_init_seen;
static bool s_hosted_init_replied;
static uint8_t s_hosted_slave_cap;
static uint8_t s_hosted_slave_chip_id;
static uint8_t s_hosted_slave_sdio_mode;
static uint32 s_hosted_slave_ext_cap;

/**
 * @ingroup shell
 *
 * ARBOL: wifi_priv_rx() -> aqui.  Arma la trama de capacidades del host y la
 * sube por PRIV (0x44/0x45/0x46/0x47/0x48, todas de longitud 1; el total del
 * valor es 15, de ahi buf[1] = 15 y buf = 2 + 15 = 17 bytes).
 */
static bool hosted_send_init_response(uint8_t chip_id)
{
    uint8_t buf[17];
    size_t written;

    memset(buf, 0, sizeof(buf));
    buf[0]  = HOSTED_PRIV_EVENT_INIT;
    buf[1]  = 15U;
    buf[2]  = HOSTED_HOST_CAPABILITIES;
    buf[3]  = 1U;
    buf[4]  = 0U;
    buf[5]  = HOSTED_HOST_SLAVE_CHIP_ID;
    buf[6]  = 1U;
    buf[7]  = chip_id;
    buf[8]  = HOSTED_HOST_RAW_TP;
    buf[9]  = 1U;
    buf[10] = 0U;
    buf[11] = HOSTED_HOST_THROTTLE_HIGH;
    buf[12] = 1U;
    buf[13] = 0U;
    buf[14] = HOSTED_HOST_THROTTLE_LOW;
    buf[15] = 1U;
    buf[16] = 0U;

    written = esp_hosted_priv_tx(HOSTED_PRIV_PKT_EVENT, buf, sizeof(buf));
    if (written != sizeof(buf))
    {
        printf("hosted: la respuesta de inicio no salio entera (wrote=%u "
               "need=%u)\n", (unsigned)written, (unsigned)sizeof(buf));
        return false;
    }

    printf("hosted: respuesta de inicio enviada, chip=0x%02x\n", chip_id);
    return true;
}

/**
 * @ingroup shell
 *
 * Callback del canal PRIV: decodifica la lista de TLV del EVENT_INIT y manda
 * las capacidades del host una sola vez.
 */
static void wifi_priv_rx(const esp_hosted_frame_info_t *info,
                         const uint8_t *payload, size_t len, void *ctx)
{
    const uint8_t *pos;
    size_t remaining;

    (void)info;
    (void)ctx;

    if (payload == NULL || len < 2U)
    {
        return;
    }

    if (payload[0] != HOSTED_PRIV_EVENT_INIT)
    {
        printf("hosted: PRIV con tipo 0x%02x len=%u\n", (unsigned)payload[0],
               (unsigned)len);
        return;
    }

    if (len < (size_t)(2U + payload[1]))
    {
        printf("hosted: EVENT_INIT cortado len=%u hace falta %u\n",
               (unsigned)len, (unsigned)(2U + payload[1]));
        return;
    }

    s_hosted_init_seen = true;
    pos = payload + 2U;
    remaining = payload[1];

    while (remaining >= 2U)
    {
        uint8_t tag = pos[0];
        uint8_t tag_len = pos[1];

        if ((size_t)(tag_len + 2U) > remaining)
        {
            break;
        }

        if (tag == HOSTED_PRIV_CAPABILITY && tag_len >= 1U)
        {
            s_hosted_slave_cap = pos[2];
        }
        else if (tag == HOSTED_PRIV_FIRMWARE_CHIP_ID && tag_len >= 1U)
        {
            s_hosted_slave_chip_id = pos[2];
        }
        else if (tag == HOSTED_PRIV_CAP_EXT && tag_len >= 4U)
        {
            s_hosted_slave_ext_cap = ((uint32)pos[2]) |
                                     (((uint32)pos[3]) << 8) |
                                     (((uint32)pos[4]) << 16) |
                                     (((uint32)pos[5]) << 24);
        }
        else if (tag == HOSTED_PRIV_TRANS_SDIO_MODE && tag_len >= 1U)
        {
            s_hosted_slave_sdio_mode = pos[2];
        }

        pos += (size_t)tag_len + 2U;
        remaining -= (size_t)tag_len + 2U;
    }

    printf("hosted: inicio cap=0x%02x ext=0x%08x chip=0x%02x sdio=%s\n",
           (unsigned)s_hosted_slave_cap, (unsigned)s_hosted_slave_ext_cap,
           (unsigned)s_hosted_slave_chip_id,
           s_hosted_slave_sdio_mode ? "streaming" : "packet");

    if (!s_hosted_init_replied)
    {
        s_hosted_init_replied = hosted_send_init_response(
                                      s_hosted_slave_chip_id);
    }
}

/**
 * @ingroup shell
 *
 * Shell command (wifi dump).  Vuelca lo que el slave ya subio a la cola RX,
 * sin tocar la capa de control.  Las tramas que aqui se consumen se pierden
 * para un eventual `wifi ver`, asi que es un diagnostico del bus, no del RPC.
 */
static shellcmd wifi_dump(void)
{
    static esp_hosted_packet_t pkt;   /* 1.6 KB: fuera de la pila del shell */
    uint32 rounds;
    uint32 n = 0U;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    for (rounds = 0U; rounds < 50U; rounds++)
    {
        esp_hosted_poll();
        while (esp_hosted_rx_pop(&pkt))
        {
            wifi_print_frame(&pkt.info, pkt.payload, pkt.len);
            n++;
        }
        sleep(2);
    }

    printf("wifi: %u tramas subidas por el C6 en ~100 ms de sondeo\n", (unsigned)n);
    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi probe).  Prueba de que los bytes que escribimos hacia el
 * C6 de verdad los esta procesando su capa esp_hosted, y no solo el periferico
 * SDIO.  No usa el RPC: usa el UNICO efecto del camino H2S que el host puede
 * ver en registros por si solo, el aviso de estrangulamiento (throttle).
 *
 * El esclavo guarda dos umbrales de ocupacion de su cola RX (PRIO_Q_OTHERS,
 * 20 mensajes) que se le ponen por TLV PRIV: 0x47 alto y 0x48 bajo  (son los
 * mismos tags de la respuesta de inicio; con 0 quedan desactivados, que es como
 * la estamos mandando).  Con alto=1, en cuanto la cola tenga 1 mensaje pendiente
 * (1*100/20 = 5% > 1%) el esclavo levanta el bit 7 de su interrupcion en banda
 * --START_THROTTLE en fn1 INT_RAW (0x50)-- y al vaciarse levanta el bit 6.
 *
 * Para crear el atasco se manda la rafaga entera sin sondear: cada trama PRIV
 * la procesa el esclavo mucho mas despacio (registra "Slave init_config
 * received" por su consola) de lo que nosotros la escribimos en el bus, asi que
 * la cola crece.  Si INT_RAW sale con bit 7, el transporte H2S es correcto byte
 * a byte --cabecera, suma de verificacion y TLVs-- y el silencio del RPC es un
 * problema de contenido; si no sale nunca, lo que llega al C6 esta corrupto.
 * Al final se vuelven a poner los umbrales en 0 para dejar el esclavo igual.
 */
static shellcmd wifi_probe(void)
{
    uint8_t buf[17];
    uint32 i;
    uint32 written = 0U;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    memset(buf, 0, sizeof(buf));
    buf[0]  = HOSTED_PRIV_EVENT_INIT;
    buf[1]  = 15U;
    buf[2]  = HOSTED_HOST_CAPABILITIES;
    buf[3]  = 1U;
    buf[4]  = 0U;
    buf[5]  = HOSTED_HOST_SLAVE_CHIP_ID;
    buf[6]  = 1U;
    buf[7]  = s_hosted_slave_chip_id;
    buf[8]  = HOSTED_HOST_RAW_TP;
    buf[9]  = 1U;
    buf[10] = 0U;
    buf[11] = HOSTED_HOST_THROTTLE_HIGH;
    buf[12] = 1U;
    buf[13] = 1U;                      /* umbral alto al 1%: salta con 1 trama */
    buf[14] = HOSTED_HOST_THROTTLE_LOW;
    buf[15] = 1U;
    buf[16] = 1U;                      /* umbral bajo 1%: 0 pendientes lo quita */

    printf("wifi: rafaga de 24 tramas PRIV con umbral alto al 1%%...\n");
    for (i = 0U; i < 24U; i++)
    {
        if (esp_hosted_priv_tx(HOSTED_PRIV_PKT_EVENT, buf, sizeof(buf)) ==
            sizeof(buf))
        {
            written++;
        }
    }
    printf("wifi: %u de 24 tramas escritas (el resto sin creditos del C6)\n",
           (unsigned)written);

    /* Sin sondeo antes de leer: esp_hosted_poll() limpia INT_RAW (fn1 0xD4). */
    esp_hosted_sdio_diag_print();

    /* Drenado: la cola del esclavo se vacia y el bit 6 (STOP_THROTTLE) deberia
     * aparecer en la segunda lectura. */
    sleep(300);
    esp_hosted_sdio_diag_print();

    /* Umbrales de nuevo a 0: el esclavo vuelve a no estrangular nunca. */
    buf[13] = 0U;
    buf[16] = 0U;
    (void)esp_hosted_priv_tx(HOSTED_PRIV_PKT_EVENT, buf, sizeof(buf));
    printf("wifi: umbrales de estrangulamiento devueltos a 0\n");
    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi jam).  Decide si la tarea que consume el canal SERIAL
 * dentro del C6 esta viva o atascada, y con eso en que mitad del esclavo esta
 * el silencio del RPC.
 *
 * Prueba (esp-hosted-mcu 1.4.7, sdio_slave_api.c): UNA sola tarea, sdio_rx_task,
 * lee todas las tramas que suben del bus y las encola por if_type en colas
 * separadas --SERIAL a PRIO_Q_SERIAL (CONFIG_ESP_SDIO_RX_Q_SIZE = 20, medido en
 * el fingerprint del NCP), HCI a la de BT y el resto (PRIV incluida) a
 * PRIO_Q_OTHERS-- con xQueueSend(..., portMAX_DELAY).  El consumidor de
 * PRIO_Q_SERIAL es pserial_task.  Por tanto:
 *   - si pserial_task esta viva, las tramas SERIAL se drenan y despues de la
 *     rafaga el oraculo de `wifi probe` sigue respondiendo igual (bit de
 *     estrangulamiento y creditos avanzando) => el rechazo es del CONTENIDO de
 *     nuestra peticion (TLV/protobuf) y no del transporte;
 *   - si esta atascada, PRIO_Q_SERIAL se llena a las 20 tramas y sdio_rx_task se
 *     queda bloqueada en el encolado: ya no lee nada mas, ni PRIV.  Entonces el
 *     umbral de estrangulamiento no se ejecuta (sin bit 7/6 en INT_RAW), los
 *     creditos TX dejan de avanzar (el esclavo no libera bufers) y las escrituras
 *     devuelven menos tramas de las que intentamos.
 *
 * La rafaga usa la misma peticion de siempre (get_coprocessor_fw_version) con
 * espera de 1 ms para que cada una se escriba y se abandone; no se cambia nada
 * del driver ni del esclavo y el probe final devuelve los umbrales a 0.
 */
static shellcmd wifi_jam(void)
{
    uint32_t major = 0U;
    uint32_t minor = 0U;
    uint32_t patch = 0U;
    uint32 written = 0U;
    uint32 i;
    /* 24 > PRIO_Q_SERIAL (20) y > REQ_Q_MAX (10): si el consumidor esta muerto,
     * con esto se satura seguro. */
    const uint32 n = 24U;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    if (!esp_hosted_ctrl_init())
    {
        fprintf(stderr, "wifi: la capa de control no se inicializa\n");
        return SYSERR;
    }

    /* Un solo endpoint, el verificado por el esclavo; aqui no se prueba
     * nomenclatura, se busca volumen. */
    esp_hosted_ctrl_ep_name = "RPCRsp";

    printf("wifi: rafaga de %u peticiones SERIAL (1 ms de espera cada una)...\n",
           (unsigned)n);
    for (i = 0U; i < n; i++)
    {
        int rc = esp_hosted_ctrl_get_coprocessor_fw_version(1U, &major, &minor,
                                                          &patch);
        if (rc != ESP_HOSTED_CTRL_ERR_TX)
        {
            written++;        /* la trama entro en el FIFO del esclavo */
        }
    }
    printf("wifi: %u de %u peticiones escritas (sin escribir = rc=%d, sin "
           "creditos del C6)\n", (unsigned)written, (unsigned)n,
           ESP_HOSTED_CTRL_ERR_TX);

    printf("wifi: oraculo justo despues de la rafaga:\n");
    return wifi_probe();
}

/**
 * @ingroup shell
 *
 * Varint de protobuf, escrita a mano: los numeros que usamos (ids de RPC y uid)
 * caben en uno o dos bytes.
 */
static size_t wifi_pb_varint(uint8_t *out, uint32_t v)
{
    if (v < 0x80U)
    {
        out[0] = (uint8_t)v;
        return 1U;
    }
    out[0] = (uint8_t)((v & 0x7FU) | 0x80U);
    out[1] = (uint8_t)(v >> 7);
    return 2U;
}

/**
 * @ingroup shell
 *
 * Monta una peticion RPC cruda, sin pasar por espHostedCtrl.c, para poder variar
 * el id y el endpoint de una prueba a otra y ver cual contesta el NCP.
 *
 * Sobre Rpc (esp_hosted_rpc.proto, esquema 1.x verificado en 1.0.5/1.3.5/1.4.7):
 *   campo 1 = msg_type (Req = 1)
 *   campo 2 = msg_id   (257 GetMacAddress, 259 GetMode, 277 ConfigHeartbeat,
 *                       350 GetCoprocessorFwVersion)
 *   campo 3 = uid
 *   y el miembro del oneof lleva NUMERO DE CAMPO = msg_id, aqui vacio porque
 *   Rpc_Req_GetCoprocessorFwVersion y las otras cuatro peticiones de prueba no
 *   tienen campos obligatorios.
 * Encima va el TLV de pserial: [1][len2]["RPCRsp"][2][len2][protobuf], que es el
 * endpoint con el que el esclavo registro su handler de peticiones.
 */
static size_t wifi_poke_build(uint8_t *out, size_t out_max, const char *ep,
                              uint32_t msg_id, uint32_t uid)
{
    uint8_t pb[16];
    size_t pb_len = 0U;
    size_t ep_len = strlen(ep);
    size_t n = 0U;
    size_t tag_len;
    uint8_t tag_buf[2];

    pb[pb_len++] = 0x08U;                     /* campo 1, varint: msg_type   */
    pb[pb_len++] = 0x01U;                     /*   = Req                     */
    pb[pb_len++] = 0x10U;                     /* campo 2, varint: msg_id     */
    pb_len += wifi_pb_varint(&pb[pb_len], msg_id);
    pb[pb_len++] = 0x18U;                     /* campo 3, varint: uid        */
    pb_len += wifi_pb_varint(&pb[pb_len], uid);
    tag_len = wifi_pb_varint(tag_buf, (msg_id << 3) | 2U);   /* oneof, LEN key */
    pb[pb_len++] = tag_buf[0];
    if (tag_len > 1U)
    {
        pb[pb_len++] = tag_buf[1];
    }
    pb[pb_len++] = 0x00U;                     /* payload del oneof: vacio    */

    if (out_max < (7U + ep_len + pb_len))
    {
        return 0U;
    }

    out[n++] = 0x01U;                         /* TLV EPNAME                  */
    out[n++] = (uint8_t)(ep_len & 0xFFU);
    out[n++] = (uint8_t)((ep_len >> 8) & 0xFFU);
    memcpy(&out[n], ep, ep_len);
    n += ep_len;
    out[n++] = 0x02U;                         /* TLV DATA                    */
    out[n++] = (uint8_t)(pb_len & 0xFFU);
    out[n++] = (uint8_t)((pb_len >> 8) & 0xFFU);
    memcpy(&out[n], pb, pb_len);
    n += pb_len;

    return n;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi poke).  Mide CUALES de las peticiones de control responde
 * el NCP de la placa, una por una, con el sobre Rpc construido a mano en el shell
 * (sin la capa de control).  Es la forma de separar dos diagnosticos que hasta
 * ahora van juntos:
 *   - "el camino SERIAL esta muerto" (ningun id produce bytes C2H), frente a
 *   - "ese RPC concreto no existe en este firmware" (unos ids responden y otros
 *     no), que es lo que pasaria si el NCP llevara un esquema con ids distintos
 *     de los 1.x que hemos verificado (350 = GetCoprocessorFwVersion, 606 = su
 *     respuesta, 769 = Event_ESPInit).
 *
 * Despues de cada escritura se sondea medio segundo: si el C6 escribe algo, el
 * contador acumulado de bytes C2H (fn1 PACKET_LEN) sube y el volcado de la
 * capa SDIO imprime los bytes exactos de la respuesta, con lo que ademas queda
 * medido el numero de campos del sobre que usa ese firmware.
 */
static shellcmd wifi_poke(void)
{
    static const struct {
        uint32_t msg_id;
        const char *name;
    } reqs[] = {
        { 257U, "Req_GetMacAddress"           },
        { 259U, "Req_GetWifiMode"             },
        { 277U, "Req_ConfigHeartbeat"         },
        { 350U, "Req_GetCoprocessorFwVersion" },
    };
    uint8_t buf[64];
    uint32 k;
    uint32 i;
    uint32 uid = 1U;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    for (k = 0U; k < (sizeof(reqs) / sizeof(reqs[0])); k++)
    {
        size_t len = wifi_poke_build(buf, sizeof(buf), "RPCRsp",
                                     reqs[k].msg_id, uid);

        if (len == 0U)
        {
            fprintf(stderr, "wifi: no cabe la peticion %u\n",
                    (unsigned)reqs[k].msg_id);
            return SYSERR;
        }

        printf("wifi: poke id=%u (%s) %u bytes...\n",
               (unsigned)reqs[k].msg_id, reqs[k].name, (unsigned)len);
        if (esp_hosted_ctrl_tx(buf, len) != len)
        {
            printf("wifi: poke id=%u no se pudo escribir\n",
                   (unsigned)reqs[k].msg_id);
            continue;
        }
        uid++;

        /* Escucha media segunda: cualquier byte del C6 se ve en el contador. */
        for (i = 0U; i < 500U; i++)
        {
            esp_hosted_poll();
            esp_hosted_dispatch_rx();
            sleep(1);
        }
        printf("wifi: poke id=%u -> colaquejado=%u descartados=%u\n",
               (unsigned)reqs[k].msg_id, (unsigned)esp_hosted_rx_pending(),
               (unsigned)esp_hosted_rx_dropped());
        esp_hosted_sdio_diag_print();
    }

    return OK;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi mac).  Segunda RPC de control, y la primera que este NCP
 * SI implementa: Req_GetMacAddress (id 257 -> respuesta 513).  Medido con
 * `wifi poke`: el C6 responde a 257/259/277 y calla ante 350
 * (Req_GetCoprocessorFwVersion), asi que la version del firmware del coprocesador
 * no sirve como prueba de extremo a extremo en esta placa y el MAC si.
 *
 * Mientras la pila wifi del C6 no esta inicializada la respuesta trae
 * resp = 0x3001 (ESP_ERR_WIFI_NOT_INIT) en el campo 2 de Rpc_Resp_GetMacAddress
 * (campo 1 = mac), que el controlador devuelve como rc.  Despues de `wifi init`
 * tiene que salir un MAC real de 6 bytes.
 */
static shellcmd wifi_mac(void)
{
    uint8_t mac[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    int rc;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    (void)esp_hosted_ctrl_init();

    printf("wifi: RPC Req_GetMacAddress(STA) id=257 (espera 3 s)...\n");
    rc = esp_hosted_wifi_get_mac(ESP_HOSTED_WIFI_IF_STA, mac, 3000U);
    if (rc == ESP_HOSTED_CTRL_OK)
    {
        printf("wifi: mac del C6 = %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return OK;
    }

    printf("wifi: rc=%d", rc);
    if (rc == 0x3001)
    {
        printf(" (ESP_ERR_WIFI_NOT_INIT: usa antes `wifi init`)");
    }
    printf("\n");
    esp_hosted_sdio_diag_print();
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi init).  Levanta la pila wifi DENTRO del C6 con
 * Req_WifiInit (id 278 -> respuesta 534, Rpc_Resp_WifiInit con resp en el campo 1),
 * que es el paso que falta para que las demas peticiones dejen de devolver
 * ESP_ERR_WIFI_NOT_INIT.  El cfg se manda con los valores por defecto del
 * WIFI_INIT_CONFIG_DEFAULT del SDK, incluido el magic 0x1234 del campo 20.
 *
 * Es el preludio obligatorio de W4 (set_config/start/connect).
 */
static shellcmd wifi_init(void)
{
    int rc;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    (void)esp_hosted_ctrl_init();

    printf("wifi: RPC Req_WifiInit id=278 (espera 3 s)...\n");
    rc = esp_hosted_wifi_init_default(3000U);
    printf("wifi: Req_WifiInit -> rc=%d\n", rc);

    if (rc != ESP_HOSTED_CTRL_OK)
    {
        esp_hosted_sdio_diag_print();
        return SYSERR;
    }

    /* Despues de inicializar, la peticion del MAC ya deberia responder con
     * datos: es la comprobacion de que el estado del C6 cambio de verdad. */
    return wifi_mac();
}

/* Segundos de espera por el evento StaConnected despues de Req_WifiConnect;
 * el ejemplo de referencia no lo mide, pero la asociacion (barrido de canal
 * incluido) puede tardar mas que la respuesta a la peticion. */
#define WIFI_JOIN_EVENT_TRIES   24U
#define WIFI_JOIN_EVENT_MS      500U

/* Intentos de asociacion por `wifi join'.  Ver wifi_join_attempt(): la AP de
 * casa rechaza a veces con motivo 39 y acepta en el siguiente intento, con el
 * mismo RSSI y la misma configuracion. */
#define WIFI_JOIN_ATTEMPTS      3U

/* Lo que el esclavo conto por el ultimo evento wifi: -1 ninguno, 1
 * StaConnected, 2 StaDisconnected.  Con StaDisconnected guardamos tambien el
 * motivo que trajo el evento, que es lo unico que distingue "el SSID no se
 * ve" de "la clave esta mal". */
static int wifi_evt_seen;
static esp_hosted_wifi_sta_disconnected_t wifi_evt_disc;

static void wifi_event_print(esp_hosted_wifi_event_t event,
                             const void *event_data, void *ctx)
{
    (void)ctx;

    if (event == ESP_HOSTED_WIFI_EVENT_STA_CONNECTED && event_data != NULL)
    {
        const esp_hosted_wifi_sta_connected_t *e = event_data;

        wifi_evt_seen = 1;
        printf("wifi: evento StaConnected ssid=\"%.*s\" ch=%u auth=%d\n",
               (int)e->ssid_len, (const char *)e->ssid,
               (unsigned)e->channel, (int)e->authmode);
        return;
    }

    if (event == ESP_HOSTED_WIFI_EVENT_STA_DISCONNECTED && event_data != NULL)
    {
        const esp_hosted_wifi_sta_disconnected_t *e = event_data;

        wifi_evt_seen = 2;
        wifi_evt_disc = *e;
        printf("wifi: evento StaDisconnected ssid=\"%.*s\" reason=%u rssi=%d\n",
               (int)e->ssid_len, (const char *)e->ssid,
               (unsigned)e->reason, (int)e->rssi);
        return;
    }
}

/**
 * @ingroup shell
 *
 * Un intento completo de asociacion: configuracion -> start -> connect y espera
 * por el evento.  Devuelve OK si vino StaConnected (775); SYSERR si vino
 * StaDisconnected (776) o si no vino nada en WIFI_JOIN_EVENT_TRIES sondeos.
 *
 * El motivo por el que `wifi join' llama a esto mas de una vez: en placa se ha
 * medido que la misma AP rechaza la asociacion con motivo 39 algunas veces y
 * la acepta otras, sin cambiar nada el host (el RSSI en el rechazo es tan bueno
 * como en la aceptacion, -35 dBm, o sea que no es señal).  La AP de casa
 * tambien tarda mas de un sondeo en tener el grupo PMK listo despues de un
 * arranque de radio.  Reintentar es lo que hace el ejemplo de referencia por
 * dentro del propio esp_wifi (auto-reconnect); aqui, sin ese mecanismo, lo hace
 * el comando.
 */
static shellcmd wifi_join_attempt(const char *ssid, const char *pass)
{
    uint tries;
    int rc;

    wifi_evt_seen = 0;

    rc = esp_hosted_wifi_set_sta_config(ssid, pass, 5000U);
    printf("wifi: Req_WifiSetConfig(284) ssid=\"%s\" rc=%d\n", ssid, rc);
    if (ESP_HOSTED_CTRL_OK != rc)
    {
        fprintf(stderr, "wifi: el C6 rechazo la configuracion STA (rc=%d)\n", rc);
        return SYSERR;
    }

    rc = esp_hosted_wifi_start(5000U);
    printf("wifi: Req_WifiStart(280) rc=%d\n", rc);
    if (ESP_HOSTED_CTRL_OK != rc)
    {
        fprintf(stderr, "wifi: Req_WifiStart fallo (rc=%d)\n", rc);
        return SYSERR;
    }

    rc = esp_hosted_wifi_connect(10000U);
    printf("wifi: Req_WifiConnect(282) rc=%d\n", rc);
    if (ESP_HOSTED_CTRL_OK != rc)
    {
        fprintf(stderr, "wifi: Req_WifiConnect fallo (rc=%d)\n", rc);
        return SYSERR;
    }

    /* La peticion paso; ahora el evento.  sta_connected lo pone true el rx
     * del controlador al ver id 775, y ese rx solo corre cuando alguien baja
     * tramas del esclavo: con la interfaz de red aun abajo no hay hilo
     * netRecv que sondee, asi que el propio join tiene que bombear el SDIO
     * (poll + dispatch) mientras espera. */
    for (tries = 0U; tries < WIFI_JOIN_EVENT_TRIES; tries++)
    {
        esp_hosted_poll();
        esp_hosted_dispatch_rx();

        if (esp_hosted_wifi_sta_is_connected() || wifi_evt_seen == 1)
        {
            printf("wifi: asociado a \"%s\" (evento StaConnected 775)\n", ssid);
            printf("wifi: siguiente paso: `netup ETH0' (sube la interfaz por DHCP)\n");
            return OK;
        }

        /* Desasociacion con motivo terminal: el C6 reintenta solo, pero si en
         * 5 s ya dijo que la red no esta, no tiene espera dejarlo corriendo. */
        if (wifi_evt_seen == 2 && tries >= 10U)
        {
            break;
        }
        sleep(WIFI_JOIN_EVENT_MS);
    }

    fprintf(stderr, "wifi: no llego el evento StaConnected (%u s de sondeo)\n",
            (WIFI_JOIN_EVENT_TRIES * WIFI_JOIN_EVENT_MS) / 1000U);
    if (wifi_evt_seen == 2)
    {
        fprintf(stderr, "wifi: motivo del C6 = %u", (unsigned)wifi_evt_disc.reason);
        if (wifi_evt_disc.reason == 201U)
        {
            fprintf(stderr, " (NO_AP_FOUND: ese SSID no esta en el aire; revisa "
                            "el nombre, el `_` de mas/menos, o el bando 2,4 GHz)");
        }
        else if (wifi_evt_disc.reason == 202U)
        {
            fprintf(stderr, " (AUTH_EXPIRE: el SSID existe pero la clave no "
                            "cuadra con el handshake)");
        }
        else if (wifi_evt_disc.reason == 203U)
        {
            fprintf(stderr, " (CONN_FAIL: asociacion rechazada por la AP)");
        }
        fprintf(stderr, "  rssi=%d\n", (int)wifi_evt_disc.rssi);
    }
    else
    {
        fprintf(stderr, "wifi: ni siquiera llego evento de desasociacion\n");
    }
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi join <ssid> [pass]).  W4: asociar la STA del C6 a un
 * punto de acceso.  Repite el orden del ejemplo de referencia
 * (examples/esp_hosted/source/main.c:470-516):
 *
 *   Req_WifiInit 278 -> Req_SetWifiMode 260 (STA) -> Req_WifiSetConfig 284
 *   (ssid/pass) -> Req_WifiStart 280 -> Req_WifiConnect 282
 *
 * y despues ESPERA el evento StaConnected (id 775): la respuesta a
 * Req_WifiConnect solo dice que el C6 acepto la peticion, la asociacion real
 * llega por el evento y es la que pone sta_connected a true en el controlador
 * (lo lee espeth por ESPETH_CTRL_LINK, y por lo tanto `netup' por DHCP).
 *
 * Las credenciales entran por la consola y no se guardan en ningun fichero.
 */
static shellcmd wifi_join(int nargs, char *args[])
{
    const char *ssid;
    const char *pass;
    uint attempt;
    int rc;

    if (nargs < 3)
    {
        fprintf(stderr, "wifi: uso: `wifi join <ssid> [pass]'\n");
        return SYSERR;
    }

    ssid = args[2];
    pass = (nargs >= 4) ? args[3] : "";

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up'\n");
        return SYSERR;
    }

    if (!esp_hosted_ctrl_init())
    {
        fprintf(stderr, "wifi: la capa de control no se inicializa\n");
        return SYSERR;
    }

    /* El evento de desasociacion es lo unico que dice POR QUE no se conecto
     * (motivo 201 = el SSID no se ve, 202 = clave/autenticacion mal); sin cb
     * registrado el controlador lo parsea y lo tira. */
    esp_hosted_wifi_set_event_cb(wifi_event_print, NULL);

    /* init y modo son tolerantemente reentrantes: si ya se hizo `wifi init'
     * antes, el C6 devuelve error y se continua igual. */
    rc = esp_hosted_wifi_init_default(3000U);
    printf("wifi: Req_WifiInit(278) rc=%d\n", rc);

    rc = esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000U);
    printf("wifi: Req_SetWifiMode(260,STA) rc=%d\n", rc);

    for (attempt = 1U; attempt <= WIFI_JOIN_ATTEMPTS; attempt++)
    {
        if (attempt > 1U)
        {
            printf("wifi: intento %u de %u\n", attempt, WIFI_JOIN_ATTEMPTS);
            /* Desasociar antes de volver a pedir: deja el supplicant del C6 en
             * limpio en lugar de reintentar sobre el estado del rechazo. */
            esp_hosted_wifi_disconnect(3000U);
            sleep(1000U);
        }

        if (OK == wifi_join_attempt(ssid, pass))
        {
            return OK;
        }
    }

    printf("wifi: tramas del esclavo en cola=%u descartadas=%u\n",
           (unsigned)esp_hosted_rx_pending(), (unsigned)esp_hosted_rx_dropped());
    esp_hosted_sdio_diag_print();
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi leave).  Desasociar la STA con Req_WifiDisconnect
 * (id 283 -> respuesta 539).  Deja la interfaz de red montada pero sin
 * enlace; para bajar la pila usa `netdown ETH0'.
 */
static shellcmd wifi_leave(void)
{
    int rc;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up'\n");
        return SYSERR;
    }

    if (!esp_hosted_ctrl_init())
    {
        fprintf(stderr, "wifi: la capa de control no se inicializa\n");
        return SYSERR;
    }

    rc = esp_hosted_wifi_disconnect(5000U);
    printf("wifi: Req_WifiDisconnect(283) rc=%d -> sta_connected=%u\n",
           rc, (unsigned)esp_hosted_wifi_sta_is_connected());

    return (ESP_HOSTED_CTRL_OK == rc) ? OK : SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi ver).  Primera RPC de control: pedir la version del
 * firmware del coprocessador (esp_hosted_ctrl.c, ID 350/606).  Es la prueba de
 * extremo a extremo del camino de datos --CMD53 de escritura, TLV+protobuf,
 * esclavo, y CMD53 de lectura con la respuesta-- antes de tocar escaneo,
 * SSID o DHCP.
 */
static shellcmd wifi_ver(void)
{
    uint32_t major = 0U;
    uint32_t minor = 0U;
    uint32_t patch = 0U;
    int rc;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up`\n");
        return SYSERR;
    }

    if (!esp_hosted_ctrl_init())
    {
        fprintf(stderr, "wifi: la capa de control no se inicializa "
                        "(esp_hosted sin init?)\n");
        return SYSERR;
    }

    /* La referencia escucha el SERIAL (if_type 3) para el RPC y el PRIV
     * (if_type 5) para el handshake de inicio; ambos van enganchados mientras
     * dura la espera. */
    esp_hosted_priv_set_rx_cb(wifi_priv_rx, NULL);

    /* Antes de la primera RPC tienen que darse las dos condiciones de la
     * referencia: las capacidades del host ya fueron subidas (replied) Y la capa
     * de control se dio por lista (esp_hosted_ctrl_is_ready, que se enciende al
     * procesar el evento ESP_INIT del SERIAL).  Se sondea hasta 1 s. */
    {
        uint32 wait_ms = 1000U;
        while (wait_ms-- > 0U &&
               (!s_hosted_init_replied || !esp_hosted_ctrl_is_ready()))
        {
            esp_hosted_poll();
            esp_hosted_dispatch_rx();
            sleep(1);
        }
        printf("wifi: handshake visto=%u respondido=%u ctrl_listo=%u\n",
               (unsigned)s_hosted_init_seen, (unsigned)s_hosted_init_replied,
               (unsigned)esp_hosted_ctrl_is_ready());
    }

    /* Se prueban los dos nombres de endpoint en el mismo arranque: "RPCRsp" es
     * el que usa la referencia para la peticion H2S y "RPCReq" es el que sugiere
     * la nomenclatura del propio C6 (hermano de "RPCEvt", con el que subio su
     * evento).  Cada intento espera 3 s; si uno contesta, ese nombre se queda. */
    {
        static const char *ep_names[2] = { "RPCRsp", "RPCReq" };
        uint32 k;

        rc = ESP_HOSTED_CTRL_ERR_TIMEOUT;
        for (k = 0U; k < 2U && rc != ESP_HOSTED_CTRL_OK; k++)
        {
            esp_hosted_ctrl_ep_name = ep_names[k];
            printf("wifi: RPC get_coprocessor_fw_version por endpoint \"%s\" "
                   "(espera 3 s)...\n", ep_names[k]);
            rc = esp_hosted_ctrl_get_coprocessor_fw_version(3000U, &major,
                                                           &minor, &patch);
            printf("wifi: endpoint \"%s\" -> rc=%d\n", ep_names[k], rc);
        }
    }

    if (rc == ESP_HOSTED_CTRL_OK)
    {
        printf("wifi: firmware del C6 = %u.%u.%u\n", (unsigned)major,
               (unsigned)minor, (unsigned)patch);
    }
    else
    {
        printf("wifi: RPC sin respuesta valida -> rc=%d (cola: pendientes=%u "
               "descartados=%u)\n", rc, (unsigned)esp_hosted_rx_pending(),
               (unsigned)esp_hosted_rx_dropped());
        /* Si el esclavo tiene una trama sin subir, PACKET_LEN/INT_RAW lo dicen
         * en registers: es la diferencia entre "el C6 no contesto" y " contesto
         * y aqui no la supimos leer". */
        esp_hosted_sdio_diag_print();
    }

    esp_hosted_priv_set_rx_cb(NULL, NULL);
    return (rc == ESP_HOSTED_CTRL_OK) ? OK : SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (wifi up | down | diag | ver | dump | sdio | fn1).
 * @param nargs number of arguments in args array
 * @param args  array of arguments
 * @return non-zero value on error
 */
shellcmd xsh_wifi(int nargs, char *args[])
{
    struct sdioinfo info;

    if (nargs == 2 && strcmp(args[1], "--help") == 0)
    {
        wifi_usage(args[0]);
        return OK;
    }

    if (nargs == 2 && strcmp(args[1], "up") == 0)
    {
        return wifi_up();
    }

    if (nargs == 2 && strcmp(args[1], "down") == 0)
    {
        return wifi_down();
    }

    if (nargs == 2 && strcmp(args[1], "diag") == 0)
    {
        return wifi_diag();
    }

    if (nargs == 2 && strcmp(args[1], "stats") == 0)
    {
        return wifi_stats();
    }

    if (nargs == 2 && strcmp(args[1], "signal") == 0)
    {
        return wifi_signal();
    }

    if (nargs == 2 && strcmp(args[1], "dump") == 0)
    {
        return wifi_dump();
    }

    if (nargs == 2 && strcmp(args[1], "probe") == 0)
    {
        return wifi_probe();
    }

    if (nargs == 2 && strcmp(args[1], "jam") == 0)
    {
        return wifi_jam();
    }

    if (nargs == 2 && strcmp(args[1], "poke") == 0)
    {
        return wifi_poke();
    }

    if (nargs == 2 && strcmp(args[1], "init") == 0)
    {
        return wifi_init();
    }

    if (nargs >= 3 && strcmp(args[1], "join") == 0)
    {
        return wifi_join(nargs, args);
    }

    if (nargs == 2 && strcmp(args[1], "leave") == 0)
    {
        return wifi_leave();
    }

    if (nargs >= 2 && strcmp(args[1], "scan") == 0)
    {
        /* Sin argumento: el listado medido (286 -> 289 -> 294).  Con un numero
         * de id: la sonda en bruto de ese RPC, para volver a medir el esquema
         * si el firmware del NCP cambia. */
        if (nargs >= 3)
        {
            return wifiScanRaw((uint32)atoi(args[2]),
                               (nargs >= 4) ? args[3] : NULL);
        }
        return wifiScanList();
    }

    if (nargs >= 3 && strcmp(args[1], "sweep") == 0)
    {
        uint32 from = (uint32)atoi(args[2]);
        uint32 to = (nargs >= 4) ? (uint32)atoi(args[3]) : from;

        return wifiSweep(from, to);
    }

    if (nargs == 2 && strcmp(args[1], "mac") == 0)
    {
        return wifi_mac();
    }

    if (nargs == 2 && strcmp(args[1], "ver") == 0)
    {
        return wifi_ver();
    }

    if (nargs == 2 && strcmp(args[1], "fn1") == 0)
    {
        printf("wifi: sonda del transporte de la funcion 1 del C6 (W2.1)...\n");
        return (sdioFn1TransportProbe() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "dma") == 0)
    {
        printf("wifi: diagnostico del CMD53 por la IDMAC, 4-bit y 1-bit "
               "(tarda unos segundos)...\n");
        return (sdioDmaDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "pio") == 0)
    {
        printf("wifi: diagnostico del CMD53 en PIO (IDMAC apagada, BUFFIFO a "
               "mano), 4-bit y 1-bit (tarda unos segundos)...\n");
        return (sdioPioDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "slow") == 0)
    {
        printf("wifi: CMD53 en PIO barre relojes de 400 kHz a 20 MHz y "
               "compara CMD52 vs PIO en el mismo registro...\n");
        return (sdioPioClkDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "bus") == 0)
    {
        printf("wifi: quien conduce DAT0 (pads del P4 tri-estado, pull-up y "
               "pull-down, y reset del C6 validado)...\n");
        return (sdioBusOwnershipDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "dat") == 0)
    {
        printf("wifi: causa-efecto sobre DAT0: desarmar/ack/rearmar la "
               "interrupcion del esclavo, CMD53 en reposo y prueba de "
               "cortocircuito conduciendo D0/D1...\n");
        return (sdioDatDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "wires") == 0)
    {
        printf("wifi: pregunta por el alambre: fotos de los 48 receptores del "
               "P4 con la IRQ del esclavo pendiente, tras el ack y tras el "
               "doorbell -- el pad que cambia es el DAT0 verdadero...\n");
        return (sdioWiresDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "fifo") == 0)
    {
        printf("wifi: ventana FIFO del esclavo: leerla byte a byte por CMD52 y "
               "pedirla por CMD53 en byte-mode y en block-mode, mas el control "
               "sobre fn0 (que ya responde)...\n");
        return (sdioFifoWindowDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "hold") == 0)
    {
        printf("wifi: dona y congelacion de los pads 14/15: lectura del hold "
               "(LP_PAD_HOLD 0x5012B04C y HP O_HOLD_CTRL0/1 0x500E5074/78) y del "
               "MUX_SEL LP (0x5012B040/44), conduccion y pull antes y despues de "
               "soltarlos, y CMD53 contra la verdad del CMD52...\n");
        return (sdioPadHoldDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "hunt") == 0)
    {
        printf("wifi: recorre la ENTRADA DAT0 del CIU pad por pad y pide 8 "
               "bytes por CMD53: el pad que devuelve lo mismo que el CMD52 es "
               "el cable de DAT0...\n");
        return (sdioDatHuntDiag() == OK) ? OK : SYSERR;
    }

    if (nargs == 2 && strcmp(args[1], "pads") == 0)
    {
        printf("wifi: quien conduce las patas de datos del C6 "
               "(fotos en reposo / C6 en reset / C6 liberado)...\n");
        sdioPadProbe();
        return OK;
    }

    if (nargs != 2 || strcmp(args[1], "sdio") != 0)
    {
        fprintf(stderr, "%s: invalid arguments\n", args[0]);
        wifi_usage(args[0]);
        return SYSERR;
    }

    printf("wifi: enumerando el ESP32-C6 en el slot 1 (SDIO)...\n");
    if (sdioEnumerate(&info) == OK)
    {
        sdioProbeReport(&info);
        printf("wifi: enlace SDIO con el C6 OK (funcion 1 habilitada)\n");
        return OK;
    }

    sdioProbeReport(&info);
    printf("wifi: el C6 no respondio (hwerr=0x%08x)\n", sd_hwerr);
    return SYSERR;
}
