/**
 * @file xsh_wifi_scan.c
 *
 * Escaneo de redes WiFi desde la consola (`wifi scan`) y barrido del esquema
 * RPC de este NCP (`wifi sweep`).
 *
 * Por que hace falta escribirlo y no viene del SDK de referencia:
 * /home/daniel/esp32-riscv-bare-metal-sdk-main/drivers/esp_hosted implementa
 * solo init/set_mode/set_config/start/connect/disconnect/get_mac/heartbeat.
 * No hay ninguna llamada de escaneo, y el driver se porta verbatim, asi que el
 * escaneo se escribe aqui, en la capa de shell, usando las primitivas que el
 * propio transporte ya exporta (esp_hosted_ctrl_tx + esp_hosted_rx_pop).
 *
 * Consecuencia: el numero de RPC del escaneo no se conoce de antemano.  Este
 * fichero lo mide.  Lo que si esta verificado en placa es como el propio C6
 * etiqueta sus RpcPacket (visto con `wifi up'/'wifi join' y los hexdumps de
 * espHostedSdio.c):
 *
 *   sobre la trama del esclavo:  01 len_lo len_hi "RPCRsp"/"RPCEvt"
 *                                02 len_lo len_hi <protobuf>
 *   dentro del protobuf:         08 <type>        campo 1 varint: 1=Req 2=Resp 3=Event
 *                                10 <id>          campo 2 varint: id del RPC
 *                                18 <uid>         campo 3 varint
 *                                <clave> <len> <payload>
 *                                clave = (id << 3) | 2  -> el numero de campo del
 *                                oneof es el PROPIO id del RPC (medido: el
 *                                StaDisconnected 776 llega con clave 6210, y
 *                                (776<<3)|2 = 6210).
 *
 * `wifi sweep <desde> <hasta>' manda peticiones vacias id a id y reporta la
 * respuesta de cada uno, campo a campo; `wifi scan' usa lo medido con ese
 * barrido (286 arranca el escaneo, 289 da el recuento, 294 el registro i) para
 * listar las redes que ve el C6.  `wifi scan <id>' se queda como la sonda en
 * bruto, por si el firmware del esclavo cambia y hay que volver a medir.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <esp_hosted.h>
#include <esp_hosted_ctrl.h>
#include <esp_hosted_sdio.h>
#include <shell.h>
#include <timer.h>

/* Tipos del sobre TLV del canal de control (los mismos que usa
 * espHostedCtrl.c al envolver sus peticiones). */
#define HOSTED_TLV_EPNAME   0x01U
#define HOSTED_TLV_DATA     0x02U

#define RPC_TYPE_REQ        1U
#define RPC_TYPE_RESP       2U
#define RPC_TYPE_EVENT      3U

/* Nombre del endpoint H2S: el del original, verificado en placa en W2 (con
 * "RPCReq" este NCP no contestaba). */
#define SWEEP_EP_NAME       "RPCRsp"

/* uid de las peticiones que genera este fichero; alto para no pisar el de la
 * capa de control (que empieza en 1). */
static uint32 scan_uid = 1000U;

/**
 * Varint protobuf (hasta 32 bits) desde buf[*off].  Devuelve false si se acaba
 * el buffer o si el varint pasa de 5 bytes.
 */
static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *off, uint32 *out)
{
    uint32 value = 0U;
    uint32 shift = 0U;
    uint32 bytes = 0U;
    uint8  byte;

    /* Hasta 10 bytes: protobuf codifica un int32 NEGATIVO como su
     * complemento a dos de 64 bits, o sea un varint de 10 bytes.  El RSSI
     * (campo 5 del registro de barrido y del AP asociado) viene asi, y con el
     * corte de antes se perdian el registro entero y el parseo.  Aqui se
     * consumen los 10 bytes y se quedan los 32 bajos, que es justo el int32
     * con signo que se busca. */
    while (bytes < 10U)
    {
        if (*off >= len)
        {
            return false;
        }
        byte = buf[(*off)++];
        if (shift < 32U)
        {
            value |= (uint32)(byte & 0x7FU) << shift;
        }
        if ((byte & 0x80U) == 0U)
        {
            *out = value;
            return true;
        }
        shift += 7U;
        bytes++;
    }

    return false;
}

/** Codificar un varint en out; devuelve cuantos bytes escribio (0 si no cabe). */
static size_t pb_write_varint(uint8_t *out, uint32 value)
{
    size_t n = 0U;
    uint8 byte;

    do
    {
        byte = (uint8)(value & 0x7FU);
        value >>= 7U;
        if (value != 0U)
        {
            byte |= 0x80U;
        }
        out[n++] = byte;
    } while (value != 0U);

    return n;
}

/**
 * Montar la peticion RPC completa (TLV + RpcPacket) para el id dado.
 *
 * Con extra == NULL el payload del oneof va vacio: es como el C6 manda sus
 * propios mensajes vacios (evento de arranque medido: 08 03 | 10 81 06 |
 * 8a 30 00).  Con extra se mete el protobuf ya montado del parametro, que es
 * lo que necesitan peticiones como "dame el registro i del escaneo".
 */
static size_t rpc_build_request_ex(uint8_t *out, size_t cap, uint32 id,
                                   uint32 uid, const uint8_t *extra, size_t extra_len)
{
    uint8_t pb[32];
    uint8_t key[4];
    size_t pb_len = 0U;
    size_t key_len;
    size_t ep_len = strlen(SWEEP_EP_NAME);
    size_t n = 0U;

    pb[pb_len++] = 0x08U;                        /* campo 1, varint: type   */
    pb[pb_len++] = RPC_TYPE_REQ;
    pb[pb_len++] = 0x10U;                        /* campo 2, varint: msg_id */
    pb_len += pb_write_varint(&pb[pb_len], id);
    pb[pb_len++] = 0x18U;                        /* campo 3, varint: uid    */
    pb_len += pb_write_varint(&pb[pb_len], uid);

    key_len = pb_write_varint(key, (id << 3) | 2U);   /* clave del oneof     */
    if (pb_len + key_len + extra_len + 1U > sizeof(pb))
    {
        return 0U;
    }
    memcpy(&pb[pb_len], key, key_len);
    pb_len += key_len;
    pb[pb_len++] = (uint8)extra_len;             /* longitud del payload    */
    if (extra_len != 0U)
    {
        memcpy(&pb[pb_len], extra, extra_len);
        pb_len += extra_len;
    }

    if (cap < (7U + ep_len + pb_len))
    {
        return 0U;
    }

    out[n++] = HOSTED_TLV_EPNAME;
    out[n++] = (uint8)(ep_len & 0xFFU);
    out[n++] = (uint8)((ep_len >> 8) & 0xFFU);
    memcpy(&out[n], SWEEP_EP_NAME, ep_len);
    n += ep_len;
    out[n++] = HOSTED_TLV_DATA;
    out[n++] = (uint8)(pb_len & 0xFFU);
    out[n++] = (uint8)((pb_len >> 8) & 0xFFU);
    memcpy(&out[n], pb, pb_len);
    n += pb_len;

    return n;
}

static size_t rpc_build_request(uint8_t *out, size_t cap, uint32 id, uint32 uid)
{
    return rpc_build_request_ex(out, cap, id, uid, NULL, 0U);
}

/**
 * Sacar el bloque protobuf del sobre TLV de una trama del esclavo.
 */
static const uint8_t *tlv_find_data(const uint8_t *buf, size_t len, size_t *out_len)
{
    size_t off = 0U;

    while (off + 3U <= len)
    {
        uint8  type = buf[off];
        size_t blen = (size_t)buf[off + 1U] | ((size_t)buf[off + 2U] << 8);

        off += 3U;
        if (off + blen > len)
        {
            return NULL;
        }
        if (type == HOSTED_TLV_DATA)
        {
            *out_len = blen;
            return &buf[off];
        }
        off += blen;
    }

    return NULL;
}

/**
 * Cabecera del RpcPacket: type, msg_id y uid (campos 1..3).  El payload del
 * oneof se deja para quien lo quiera: aqui interesa el id.
 */
static bool rpc_read_header(const uint8_t *pb, size_t len,
                            uint32 *type, uint32 *msg_id, uint32 *uid)
{
    size_t off = 0U;
    uint32 key;
    uint32 val;

    *type = 0U;
    *msg_id = 0U;
    *uid = 0U;

    while (off < len)
    {
        if (!pb_read_varint(pb, len, &off, &key))
        {
            return false;
        }
        switch (key >> 3)
        {
            case 1U:
                if ((key & 7U) != 0U || !pb_read_varint(pb, len, &off, &val))
                {
                    return false;
                }
                *type = val;
                break;

            case 2U:
                if ((key & 7U) != 0U || !pb_read_varint(pb, len, &off, &val))
                {
                    return false;
                }
                *msg_id = val;
                break;

            case 3U:
                if ((key & 7U) != 0U || !pb_read_varint(pb, len, &off, &val))
                {
                    return false;
                }
                *uid = val;
                break;

            default:
                /* campo del oneof u otro: se para, ya se leyo lo que habia */
                return true;
        }
    }

    return true;
}

/** Sangrado proporcional a la profundidad, para que un registro de escaneo
 *  anidado se lea como una tabla. */
static void pb_indent(uint32 depth)
{
    uint32 i;

    printf("    ");
    for (i = 0U; i < depth; i++)
    {
        printf("  ");
    }
}

/**
 * Volcado de un mensaje protobuf campo a campo, con una linea por campo y
 * sangrado por nivel.  No sabe los esquemas: imprime numero de campo, tipo de
 * cable y, segun corresponda, el varint, el texto (si es imprimible) o el
 * sub-mensaje recorrido en nivel +1.  Suficiente para leer los SSID del
 * escaneo y para deducir que campo es el canal y cual el RSSI.
 *
 * Si un campo no se puede interpretar (la recursion se acaba o el sub-mensaje
 * no es un mensaje), se vuelcan en hex los bytes que quedan: un registro de
 * escaneo con el bssid (6 bytes crudos) se lee asi igualmente.
 */
static void pb_dump(const uint8_t *buf, size_t len, uint32 depth)
{
    static const uint32 pb_max_depth = 6U;
    size_t off = 0U;
    uint32 key;
    uint32 num;
    uint32 wire;
    uint32 val;
    size_t slen;
    size_t i;

    if (depth > pb_max_depth)
    {
        pb_indent(depth);
        printf("(demasiada profundidad)\n");
        return;
    }

    while (off < len)
    {
        if (!pb_read_varint(buf, len, &off, &key))
        {
            pb_indent(depth);
            printf("(hex de reserva, %u bytes):", (unsigned)(len - off));
            for (i = off; i < len && i < off + 48U; i++)
            {
                printf(" %02x", buf[i]);
            }
            printf("\n");
            return;
        }
        num = key >> 3;
        wire = key & 7U;

        if (wire == 0U)
        {
            if (!pb_read_varint(buf, len, &off, &val))
            {
                return;
            }
            pb_indent(depth);
            printf("f%u varint = %u (0x%x)\n", (unsigned)num,
                   (unsigned)val, (unsigned)val);
            continue;
        }

        if (wire == 2U)
        {
            if (!pb_read_varint(buf, len, &off, &val))
            {
                return;
            }
            slen = (size_t)val;
            if (off + slen > len)
            {
                return;
            }

            /* ?es texto imprimible? si lo es, se imprime entre comillas; si
             * no, es un sub-mensaje y se recursa.  Los campos de red vienen
             * rellenos hasta 32/64 bytes con ceros, asi que primero se quitan
             * los NUL del final. */
            {
                size_t text_len = slen;
                uint32 printable = 1U;

                while (text_len > 0U && buf[off + text_len - 1U] == 0U)
                {
                    text_len--;
                }

                for (i = 0U; i < text_len; i++)
                {
                    uint8 c = buf[off + i];

                    if (c < 0x20U || c > 0x7EU)
                    {
                        printable = 0U;
                        break;
                    }
                }

                if (printable && text_len > 0U)
                {
                    pb_indent(depth);
                    printf("f%u texto  = \"%.*s\"\n", (unsigned)num,
                           (int)text_len, (const char *)&buf[off]);
                }
                else if (slen == 6U)
                {
                    /* 6 bytes crudos = un MAC.  Se imprime tal cual: es el
                     * bssid de los registros de escaneo. */
                    pb_indent(depth);
                    printf("f%u 6bytes = %02x:%02x:%02x:%02x:%02x:%02x\n",
                           (unsigned)num, buf[off], buf[off + 1U], buf[off + 2U],
                           buf[off + 3U], buf[off + 4U], buf[off + 5U]);
                }
                else
                {
                    pb_indent(depth);
                    printf("f%u mensaje(%u bytes):\n", (unsigned)num,
                           (unsigned)slen);
                    pb_dump(&buf[off], slen, depth + 1U);
                }
            }
            off += slen;
            continue;
        }

        if (wire == 1U)
        {
            if (off + 8U > len)
            {
                return;
            }
            pb_indent(depth);
            printf("f%u fijo64\n", (unsigned)num);
            off += 8U;
            continue;
        }

        if (wire == 5U)
        {
            if (off + 4U > len)
            {
                return;
            }
            pb_indent(depth);
            printf("f%u fijo32   = 0x%08x\n", (unsigned)num,
                   (unsigned)(buf[off] | (buf[off + 1U] << 8) |
                              (buf[off + 2U] << 16) | (buf[off + 3U] << 24)));
            off += 4U;
            continue;
        }

        pb_indent(depth);
        printf("(tipo de cable %u no soportado)\n", (unsigned)wire);
        return;
    }
}

/** Vaciar la cola de tramas del esclavo sin enrutar a nadie. */
static void rx_drain(void)
{
    static esp_hosted_packet_t pkt;

    while (esp_hosted_rx_pop(&pkt))
    {
        /* descartada a proposito: es estado de una peticion anterior */
    }
}

/**
 * @ingroup shell
 *
 * `wifi sweep <desde> <hasta>': manda una peticion RPC vacia por cada id del
 * rango y dice cuales responden.  Es la forma de medir el esquema de este NCP
 * sin copiar numeros de ninguna parte: los ids que callan no existen en este
 * firmware (así se descubrio que Req_GetCoprocessorFwVersion=350 no esta), y
 * los que contestan dicen en su respuesta el id espejo (Resp = Req + 256).
 *
 * Ojo: se esta pidiendo ids a ciegas.  Con payload vacio lo maximo que puede
 * pasar es que alguno ejecute algo (stop/deinit); el C6 se recupera con
 * `wifi join'.
 */
shellcmd wifiSweep(uint32 from, uint32 to)
{
    static esp_hosted_packet_t pkt;
    uint8_t buf[64];
    uint32 answered = 0U;
    uint32 id;
    uint32 i;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up'\n");
        return SYSERR;
    }

    if (to < from || to - from > 64U)
    {
        fprintf(stderr, "wifi: sweep: rango demasiado grande (max 64 ids)\n");
        return SYSERR;
    }

    for (id = from; id <= to; id++)
    {
        size_t len = rpc_build_request(buf, sizeof(buf), id, scan_uid++);
        uint32 hits = 0U;

        if (len == 0U)
        {
            continue;
        }

        rx_drain();
        if (esp_hosted_ctrl_tx(buf, len) != len)
        {
            printf("sweep: id=%u no se pudo escribir\n", (unsigned)id);
            continue;
        }

        for (i = 0U; i < 100U; i++)
        {
            esp_hosted_poll();
            while (esp_hosted_rx_pop(&pkt))
            {
                size_t pb_len = 0U;
                const uint8_t *pb;
                uint32 type, msg_id, uid;

                if (pkt.info.if_type != ESP_HOSTED_IF_SERIAL)
                {
                    continue;
                }
                pb = tlv_find_data(pkt.payload, pkt.len, &pb_len);
                if (pb == NULL || !rpc_read_header(pb, pb_len, &type, &msg_id, &uid))
                {
                    continue;
                }
                hits++;
                printf("sweep: id=%u -> %s msg_id=%u uid=%u (%u bytes)\n",
                       (unsigned)id,
                       (type == RPC_TYPE_RESP) ? "Resp" :
                       (type == RPC_TYPE_EVENT) ? "Event" : "otro",
                       (unsigned)msg_id, (unsigned)uid, (unsigned)pb_len);
                /* Volcado campo a campo de las primeras respuestas: es lo que
                 * permite saber QUÉ es cada id (una consulta de config trae
                 * texto; un escaneo trae registros repetidos con bssid/canal/
                 * rssi).  Se limita para no anegar la consola. */
                if (hits <= 3U)
                {
                    pb_dump(pb, pb_len, 0U);
                }
            }
            sleep(2U);
        }

        if (hits == 0U)
        {
            printf("sweep: id=%u sin respuesta\n", (unsigned)id);
        }
        else
        {
            answered++;
        }
    }

    printf("wifi: sweep de %u a %u: %u ids con respuesta\n",
           (unsigned)from, (unsigned)to, (unsigned)answered);
    return OK;
}

/**
 * Devolver los bytes del campo `msg_id` del RpcPacket (el del oneof, que lleva
 * el cuerpo de la respuesta).  rpc_read_header se para justo ahi; esto, en
 * cambio, lo busca por numero de campo y lo entrega entero.
 */
static const uint8_t *rpc_payload(const uint8_t *pb, size_t len, uint32 msg_id,
                                  size_t *out_len)
{
    size_t off = 0U;
    uint32 key;
    uint32 val;

    *out_len = 0U;

    while (off < len)
    {
        if (!pb_read_varint(pb, len, &off, &key))
        {
            return NULL;
        }
        if ((key & 7U) == 2U)
        {
            if (!pb_read_varint(pb, len, &off, &val))
            {
                return NULL;
            }
            if ((key >> 3U) == msg_id)
            {
                if (off + (size_t)val > len)
                {
                    return NULL;
                }
                *out_len = (size_t)val;
                return &pb[off];
            }
            off += (size_t)val;
            continue;
        }
        if ((key & 7U) == 0U)
        {
            if (!pb_read_varint(pb, len, &off, &val))
            {
                return NULL;
            }
            continue;
        }
        if ((key & 7U) == 5U)
        {
            off += 4U;
            continue;
        }
        if ((key & 7U) == 1U)
        {
            off += 8U;
            continue;
        }
        return NULL;
    }

    return NULL;
}

/** Primer varint del cuerpo de la respuesta con el numero de campo dado. */
static uint32 payload_varint(const uint8_t *pb, size_t len, uint32 field)
{
    size_t off = 0U;
    uint32 key;
    uint32 val;

    while (off < len)
    {
        if (!pb_read_varint(pb, len, &off, &key))
        {
            return 0U;
        }
        if ((key & 7U) != 0U)
        {
            return 0U;                     /* el primer campo no es varint   */
        }
        if (!pb_read_varint(pb, len, &off, &val))
        {
            return 0U;
        }
        if ((key >> 3U) == field)
        {
            return val;
        }
    }

    return 0U;
}

/**
 * Mandar una peticion y esperar su respuesta.
 *
 * Desde W6-R1 la espera la hace la capa de control con el uid reservado
 * (esp_hosted_ctrl_raw_request, en device/sdio-esp32p4/espHostedCtrl.c): los
 * ids que el SDK no trae emparejados (286/288/289/294) se atienden igual que
 * los propios, y la respuesta se copia tal cual al buffer que trae el llamante.
 *
 * Antes se poppeaba la cola del transporte aqui mismo.  Eso era una carrera con
 * la hebra que baja las tramas (espethRead llama a esp_hosted_dispatch_rx) y,
 * peor aun, descartaba todo lo que no fuera SERIAL: medido, un `wifi signal'
 * con la red en marcha costaba un 25 % de ping perdidos.
 *
 * @param id        numero de RPC a pedir
 * @param extra     protobuf del parametro, o NULL para pedir sin payload
 * @param resp      destino del protobuf de la respuesta completo
 * @param resp_cap  capacidad de resp
 * @param wait_ms   cuantos milisegundos se espera antes de dar la peticion por
 *                  perdida
 * @return bytes copiados en resp, 0 si no llego la respuesta
 */
static size_t rpc_call(uint32 id, const uint8_t *extra, size_t extra_len,
                       uint8_t *resp, size_t resp_cap, uint32 wait_ms)
{
    size_t pb_len = 0U;

    if (esp_hosted_ctrl_raw_request(id, extra, extra_len, resp, resp_cap,
                                    &pb_len, wait_ms) != ESP_HOSTED_CTRL_OK)
    {
        return 0U;
    }

    return pb_len;
}

/**
 * Resumen de una linea de un registro de escaneo.
 *
 * Medido en placa, el registro trae: campo 1 = bssid (6 bytes crudos), campo 2
 * = SSID (relleno con ceros hasta 33), campo 3 = canal, campo 4 = authmode y
 * campo 5 = RSSI en dBm.  Lo que no sea de esa lista se imprime como
 * `f<n>=<valor>` para que un cambio de firmware del C6 se vea en la salida en
 * vez de parsearse a la fuerza.
 */
static void scan_record_print(uint32 idx, const uint8_t *rec, size_t len)
{
    size_t off = 0U;
    uint32 key;
    uint32 val;
    uint8_t bssid[6];
    char ssid[33];
    uint32 channel = 0U;
    uint32 authmode = 0U;
    int32 rssi = 0;
    uint32 have_rssi = 0U;
    uint32 have_bssid = 0U;
    uint32 have_ssid = 0U;
    size_t n;
    size_t field_len;
    size_t i;

    bssid[0] = 0U;
    ssid[0] = '\0';

    printf("  [%u]", (unsigned)idx);

    while (off < len)
    {
        if (!pb_read_varint(rec, len, &off, &key))
        {
            break;
        }
        if ((key & 7U) == 0U)
        {
            if (!pb_read_varint(rec, len, &off, &val))
            {
                break;
            }
            if ((key >> 3U) == 3U)
            {
                channel = val;
            }
            else if ((key >> 3U) == 4U)
            {
                /* authmode: el enum de Opts (0=OPEN, 2=WPA_PSK, 3=WPA2_PSK...) */
                authmode = val;
            }
            else if ((key >> 3U) == 5U)
            {
                /* RSSI en dBm: int32 con signo, y por eso el varint de 10
                 * bytes que la lectura tolera desde hoy. */
                rssi = (int32)val;
                have_rssi = 1U;
            }
            else
            {
                printf(" f%u=%u", (unsigned)(key >> 3U), (unsigned)val);
            }
            continue;
        }
        if ((key & 7U) != 2U)
        {
            break;
        }
        if (!pb_read_varint(rec, len, &off, &val))
        {
            break;
        }
        n = (size_t)val;
        if (off + n > len)
        {
            break;
        }
        /* `field_len` es lo que realmente hay que saltar al final del bucle:
         * el SSID viene relleno con ceros hasta 33 y recortar `n` para
         * imprimirlo desincronizaba el parseo (de ahi salian los `f0=24'
         * espurios en las lineas). */
        field_len = n;
        if (n == 6U && !have_bssid)
        {
            memcpy(bssid, &rec[off], 6U);
            have_bssid = 1U;
        }
        else if (n != 0U && (key >> 3U) == 2U && !have_ssid)
        {
            uint32 printable = 1U;
            size_t slen = n;

            while (slen > 0U && rec[off + slen - 1U] == 0U)
            {
                slen--;
            }
            for (i = 0U; i < slen; i++)
            {
                if (rec[off + i] < 0x20U || rec[off + i] > 0x7EU)
                {
                    printable = 0U;
                    break;
                }
            }
            if (printable && slen > 0U)
            {
                size_t copy = (slen < sizeof(ssid) - 1U) ? slen : sizeof(ssid) - 1U;

                memcpy(ssid, &rec[off], copy);
                ssid[copy] = '\0';
                have_ssid = 1U;
            }
        }
        off += field_len;
    }

    if (have_ssid)
    {
        printf(" \"%s\"", ssid);
    }
    if (have_bssid)
    {
        printf(" bssid=%02x:%02x:%02x:%02x:%02x:%02x",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    }
    if (channel != 0U)
    {
        printf(" ch=%u", (unsigned)channel);
    }
    if (authmode != 0U)
    {
        printf(" auth=%u", (unsigned)authmode);
    }
    if (have_rssi)
    {
        printf(" rssi=%d dBm", (int)rssi);
    }
    printf("\n");
}

/**
 * @ingroup shell
 *
 * `wifi scan': listar las redes que ve el C6.
 *
 * Los ids de esta secuencia NO vienen del SDK de referencia, que no implementa
 * escaneo: se midieron en placa con `wifi sweep' y `wifi scan <id> [hex]'.  Lo
 * que responde cada uno, verificado:
 *
 *   286 -> Resp 542 vacia            arranca el barrido.  Es asincrono: al
 *                                     terminar manda el evento 774 con
 *                                     {f2 = numero de APs hallados}.
 *   288 -> Resp 544 {f2 = n}         cuantos registros hay cacheados (se viio
 *                                     cambiar: 2, 3, 6 segun el ultimo barrido)
 *   289 -> Resp 545 {f2 = m,         DAR HASTA n REGISTROS: el cuerpo del
 *              f3 = registro,        request lleva {f1 = n} y la respuesta
 *              f3 = registro, ...}   trae el numero devuelto en f2 y cada
 *                                     registro repetido en f3.  Medido pidiendo
 *                                     6: devolvio "Tenda_CA66E0" ch=6 y
 *                                     "MERCUSYS_1EEE" ch=7 - los mismos que ve
 *                                     la wifi del PC.
 *   294 -> Resp 550 {f2 = registro}  NO es el registro i: es la info del AP al
 *                                     que se esta asociado (ignora el indice y
 *                                     fuera de asociacion contesta 0x300f).
 *
 * Cada registro trae campo 1 = bssid (6 bytes), campo 2 = SSID y campo 3 =
 * canal.  Antes de escanear hay que encender la WiFi del NCP (278 init + 260
 * modo STA + 280 start); si ya esta encendida porque se uso `wifi join', esas
 * peticiones contestan error y se ignoran.
 */
shellcmd wifiScanList(void)
{
    /* static: la respuesta de 289 puede traer decenas de registros de ~60
     * bytes y eso no va en la pila del hilo de la shell. */
    static uint8_t resp[1700];
    uint8_t arg[8];
    size_t pb_len;
    size_t body_len = 0U;
    const uint8_t *body;
    uint32 count = 0U;
    uint32 batch = 6U;
    uint32 i;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up'\n");
        return SYSERR;
    }

    (void)esp_hosted_ctrl_init();

    printf("wifi: scan: encendiendo la WiFi del NCP (278/260/280)...\n");
    (void)esp_hosted_wifi_init_default(3000U);
    (void)esp_hosted_wifi_set_mode(ESP_HOSTED_WIFI_MODE_STA, 3000U);
    (void)esp_hosted_wifi_start(3000U);

    printf("wifi: scan: pidiendo barrido (id=286)...\n");
    if (rpc_call(286U, NULL, 0U, resp, sizeof(resp), 3000U) == 0U)
    {
        fprintf(stderr, "wifi: scan: el NCP no contesto al arranque del barrido\n");
        return SYSERR;
    }

    /* El barrido es asincrono: el evento 774 llega cuando TERMINA y trae el
     * numero de APs, asi que se bombea hasta 15 s y se sale 3 s despues del
     * ultimo.  Salir antes (lo que haciamos al no ver ninguno) deja el cache
     * del NCP con el resultado del barrido anterior. */
    {
        static esp_hosted_packet_t pkt;
        uint32 quiet = 0U;
        uint32 scans = 0U;

        for (i = 0U; i < 1500U; i++)
        {
            uint32 got = 0U;

            esp_hosted_poll();
            while (esp_hosted_rx_pop(&pkt))
            {
                size_t pl = 0U;
                const uint8_t *pb;
                uint32 type, msg_id, uid;

                if (pkt.info.if_type != ESP_HOSTED_IF_SERIAL)
                {
                    continue;
                }
                pb = tlv_find_data(pkt.payload, pkt.len, &pl);
                if (pb == NULL || !rpc_read_header(pb, pl, &type, &msg_id, &uid))
                {
                    continue;
                }
                if (type == RPC_TYPE_EVENT && msg_id == 774U)
                {
                    got = 1U;
                    scans++;
                    /* Medido: el cuerpo es {f2 = numero de APs hallados,
                     * f3 = 128}; el numero ya lo vuelve a dar id=288, asi que
                     * aqui solo se cuenta el evento (el volcado crudo fue
                     * para descifrar el esquema y ya ensuciaba la salida). */
                }
            }
            sleep(10U);
            quiet = got ? 0U : quiet + 10U;
            if (scans != 0U && quiet >= 3000U)
            {
                break;
            }
        }
        printf("wifi: scan: %u eventos 774 del barrido\n", (unsigned)scans);
    }

    /* Cuantos registros hay cacheados: la Resp 544 lleva el numero en el campo
     * 2 (medido: 2, 3 y 6 segun el ultimo barrido). */
    pb_len = rpc_call(288U, NULL, 0U, resp, sizeof(resp), 3000U);
    if (pb_len == 0U)
    {
        fprintf(stderr, "wifi: scan: no llego el recuento (id=288)\n");
        return SYSERR;
    }
    body = rpc_payload(resp, pb_len, 544U, &body_len);
    if (body == NULL)
    {
        fprintf(stderr, "wifi: scan: respuesta de recuento inesperada\n");
        return SYSERR;
    }
    count = payload_varint(body, body_len, 2U);
    printf("wifi: scan: %u redes vistas por el C6\n", (unsigned)count);

    /* Se piden de 6 en 6: cada registro ocupa unos 60 bytes y la trama que sube
     * del esclavo tiene que caber en el buffer del transporte, asi que 6 va
     * holgado y el bucle lista las que haya.  Medido: el NCP va vaciando su
     * cache con cada peticion (no hay desplazamiento que pedir), y a veces
     * contesta vacio habiendo redes por listar (9 vistas, 6 entregadas) -- por
     * eso el lote se reduce y se reintenta antes de rendirse. */
    for (i = 0U; i < count; )
    {
        uint32 ask = (count - i < batch) ? (count - i) : batch;
        size_t arg_len = 0U;
        uint32 got = 0U;
        uint32 before = i;
        size_t off = 0U;
        uint32 key;
        uint32 v;
        size_t rec_len;

        arg[arg_len++] = 0x08U;                  /* campo 1: cuantos se quieren */
        arg_len += pb_write_varint(&arg[arg_len], ask);

        pb_len = rpc_call(289U, arg, arg_len, resp, sizeof(resp), 3000U);
        if (pb_len == 0U)
        {
            printf("  [%u] sin respuesta\n", (unsigned)i);
            break;
        }
        body = rpc_payload(resp, pb_len, 545U, &body_len);
        if (body == NULL)
        {
            printf("wifi: scan: respuesta de registros inesperada (%u bytes)\n",
                   (unsigned)pb_len);
            pb_dump(resp, pb_len, 0U);
            break;
        }
        got = payload_varint(body, body_len, 2U);

        while (off < body_len)
        {
            if (!pb_read_varint(body, body_len, &off, &key))
            {
                break;
            }
            if ((key & 7U) == 0U)
            {
                if (!pb_read_varint(body, body_len, &off, &v))
                {
                    break;
                }
                continue;
            }
            if ((key & 7U) != 2U)
            {
                break;
            }
            if (!pb_read_varint(body, body_len, &off, &v))
            {
                break;
            }
            rec_len = (size_t)v;
            if (off + rec_len > body_len)
            {
                break;
            }
            if ((key >> 3U) == 3U)               /* campo 3 = registro repetido */
            {
                scan_record_print(i, &body[off], rec_len);
                i++;
            }
            off += rec_len;
        }

        if (i == before)
        {
            /* Lote vacio: se prueba con el lote a la mitad (el NCP habia
             * entregado 6 de 9 y devolvio cuerpo vacio pidiendo 3) y solo se
             * rinde cuando ya se bajo a 1 registro por peticion. */
            if (batch > 1U)
            {
                batch /= 2U;
                if (batch < 1U)
                {
                    batch = 1U;
                }
                continue;
            }
            printf("wifi: scan: la peticion de %u registro(s) no trajo cuerpo "
                   "(f2=%u); se listaron %u de %u\n", (unsigned)ask,
                   (unsigned)got, (unsigned)i, (unsigned)count);
            break;
        }
    }

    return OK;
}

/**
 * Leer un campo varint de un registro de AP, saltando los de tipo longitud.
 *
 * Vale para cualquier registro del esquema (escaneo o enlace activo), y sirve
 * sobre todo para el RSSI: protobuf codifica un int32 NEGATIVO como su
 * complemento a dos de 64 bits -- 10 bytes de varint -- y pb_read_varint() los
 * lee; el valor llega truncado a 32 bits, que es justo lo que se necesita para
 * reinterpretarlo como `int32`.
 */
static bool record_varint(const uint8_t *rec, size_t len, uint32 field,
                          uint32 *out)
{
    size_t off = 0U;
    uint32 key;
    uint32 val;

    while (off < len)
    {
        if (!pb_read_varint(rec, len, &off, &key))
        {
            return false;
        }
        if ((key & 7U) == 0U)
        {
            if (!pb_read_varint(rec, len, &off, &val))
            {
                return false;
            }
            if ((key >> 3U) == field)
            {
                *out = val;
                return true;
            }
            continue;
        }
        if ((key & 7U) != 2U)
        {
            return false;
        }
        if (!pb_read_varint(rec, len, &off, &val))
        {
            return false;
        }
        if (off + (size_t)val > len)
        {
            return false;
        }
        off += (size_t)val;
    }

    return false;
}

/**
 * @ingroup shell
 *
 * Señal del AP al que estamos asociados.
 *
 * El id 294 (Resp 550) es el unico que habla del enlace ACTIVO: medido en placa
 * ignora cualquier indice que se le mande y, sin asociación, contesta con el
 * error 0x300f.  Su campo 2 es un registro con el mismo formato que los del
 * escaneo, asi que aqui se reutiliza el lector de campos.
 *
 * Es la lectura que habra que pintar en pantalla: un -50 dBm es enlace lleno y
 * un -90 dBm esta al borde de caer, y wifiSignalQuality() traduce eso a 0..100
 * para no tener que hacer la conversión (ni dividir) en el dashboard.
 *
 * @param rssi    salida: dBm del enlace, sin tocar si no hay respuesta
 * @param channel salida: canal del AP, 0 si el registro no lo trae
 * @return OK si el C6 contesto con el registro; SYSERR sin enlace o sin respuesta
 */
int wifiSignalRead(int32 *rssi, uint32 *channel)
{
    /* static: 300 bytes de respuesta no van en la pila de la hebra de la shell */
    static uint8_t resp[300];
    size_t pb_len;
    size_t body_len = 0U;
    const uint8_t *body;
    const uint8_t *rec;
    size_t rec_len;
    size_t off = 0U;
    uint32 key;
    uint32 val;
    uint32 got;

    if (!esp_hosted_sdio_is_attached())
    {
        return SYSERR;
    }

    pb_len = rpc_call(294U, NULL, 0U, resp, sizeof(resp), 1000U);
    if (pb_len == 0U)
    {
        return SYSERR;
    }
    body = rpc_payload(resp, pb_len, 550U, &body_len);
    if (body == NULL)
    {
        return SYSERR;
    }

    /* campo 2 = registro del AP asociado */
    rec = NULL;
    rec_len = 0U;
    while (off < body_len)
    {
        if (!pb_read_varint(body, body_len, &off, &key))
        {
            break;
        }
        if ((key & 7U) == 0U)
        {
            if (!pb_read_varint(body, body_len, &off, &val))
            {
                break;
            }
            continue;
        }
        if ((key & 7U) != 2U)
        {
            break;
        }
        if (!pb_read_varint(body, body_len, &off, &val))
        {
            break;
        }
        if (off + (size_t)val > body_len)
        {
            break;
        }
        if ((key >> 3U) == 2U)
        {
            rec = &body[off];
            rec_len = (size_t)val;
        }
        off += (size_t)val;
    }

    if (rec == NULL)
    {
        return SYSERR;
    }
    if (channel != NULL && record_varint(rec, rec_len, 3U, &got))
    {
        *channel = got;
    }
    if (rssi == NULL || !record_varint(rec, rec_len, 5U, &got))
    {
        return SYSERR;
    }
    *rssi = (int32)got;

    return OK;
}

/**
 * Porcentaje de señal a partir del dBm que reporta el C6.
 *
 * Recta entre -100 dBm (0 %) y -40 dBm (100 %), que es el rango en el que un
 * enlace 802.11 pasa de inoperante a lleno; fuera se satura.  Todo en enteros
 * porque el kernel no lleva coma: 100/60 se aplica al final y el truncado
 * pierde como mucho un punto, irrelevante para una barra.
 */
int wifiSignalQuality(int32 rssi)
{
    int q;

    if (rssi >= -40)
    {
        return 100;
    }
    if (rssi <= -100)
    {
        return 0;
    }
    q = (int)((rssi + 100) * 100 / 60);

    return (q < 0) ? 0 : (q > 100 ? 100 : q);
}

/** Leer una cadena de digitos hex ('0a0c54..') y dejar los bytes en out. */
static size_t hex_parse(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0U;

    while (*s != '\0')
    {
        uint32 v = 0U;
        int i;

        for (i = 0; i < 2 && *s != '\0'; i++, s++)
        {
            char c = *s;

            v <<= 4;
            if (c >= '0' && c <= '9')
            {
                v |= (uint32)(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                v |= (uint32)(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                v |= (uint32)(c - 'A' + 10);
            }
            else
            {
                return 0U;
            }
        }
        if (n >= cap)
        {
            return 0U;
        }
        out[n++] = (uint8_t)v;
    }

    return n;
}

/**
 * @ingroup shell
 *
 * `wifi scan <id> [hex]': sonda en bruto de un id - pedir ese RPC (con el
 * cuerpo del oneof que de `hex', si se pasa) y volcar TODO lo que suba.  Se
 * para 1 s despues de llegar la respuesta, o a los 60 s si no llega.
 *
 * Es la herramienta con la que se midieron 286/289/294 de arriba y la que hay
 * que usar para afinar el request del barrido: p.ej. `wifi scan 286
 * 0a0c54656e64615f434136364530' pide el escaneo filtrando por el SSID Tenda.
 */
shellcmd wifiScanRaw(uint32 req_id, const char *hex)
{
    static esp_hosted_packet_t pkt;
    uint8_t buf[64];
    uint8_t body[48];
    uint32 events = 0U;
    uint32 uid = scan_uid++;
    uint32 answered = 0U;
    size_t body_len = 0U;
    size_t len;
    uint32 i;

    if (!esp_hosted_sdio_is_attached())
    {
        fprintf(stderr, "wifi: no adjunto; usa `wifi up'\n");
        return SYSERR;
    }

    (void)esp_hosted_ctrl_init();

    if (hex != NULL && *hex != '\0')
    {
        body_len = hex_parse(hex, body, sizeof(body));
        if (body_len == 0U)
        {
            fprintf(stderr, "wifi: scan: el cuerpo hex no es par de digitos\n");
            return SYSERR;
        }
    }

    len = rpc_build_request_ex(buf, sizeof(buf), req_id, uid,
                               body_len != 0U ? body : NULL, body_len);
    if (len == 0U)
    {
        fprintf(stderr, "wifi: scan: no se pudo montar la peticion\n");
        return SYSERR;
    }

    rx_drain();
    printf("wifi: scan: pidiendo RPC id=%u (%u bytes, cuerpo=%u)...\n",
           (unsigned)req_id, (unsigned)len, (unsigned)body_len);
    if (esp_hosted_ctrl_tx(buf, len) != len)
    {
        fprintf(stderr, "wifi: scan: no se pudo escribir la peticion\n");
        return SYSERR;
    }

    /* Escucha: se vuelcan TODAS las tramas, incluidas las de datos, porque el
     * cuerpo del request es una hipotesis hasta que el NCP la confirma, y hace
     * falta ver por donde contesta.  Se corta 1 s despues de la Resp propia
     * (60 s si no llega). */
    for (i = 0U; i < 3000U; i++)
    {
        esp_hosted_poll();
        while (esp_hosted_rx_pop(&pkt))
        {
            size_t pb_len = 0U;
            const uint8_t *pb;
            uint32 type, msg_id, got_uid;

            if (pkt.info.if_type != ESP_HOSTED_IF_SERIAL)
            {
                printf("wifi: trama de datos if_type=%u ifnum=%u len=%u\n",
                       (unsigned)pkt.info.if_type, (unsigned)pkt.info.if_num,
                       (unsigned)pkt.len);
                events++;
                continue;
            }
            pb = tlv_find_data(pkt.payload, pkt.len, &pb_len);
            if (pb == NULL || !rpc_read_header(pb, pb_len, &type, &msg_id, &got_uid))
            {
                continue;
            }

            printf("wifi: trama type=%u msg_id=%u uid=%u payload=%u bytes:\n",
                   (unsigned)type, (unsigned)msg_id, (unsigned)got_uid,
                   (unsigned)pb_len);
            pb_dump(pb, pb_len, 0U);
            events++;

            if (type == RPC_TYPE_RESP && msg_id == req_id + 256U && got_uid == uid)
            {
                answered = 1U;
            }
        }
        if (answered != 0U && i > 5U)
        {
            break;
        }
        sleep(20U);
    }

    printf("wifi: scan: %u tramas de control recibidas\n", (unsigned)events);
    return (events > 0U) ? OK : SYSERR;
}
