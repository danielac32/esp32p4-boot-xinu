/**
 * @file xsh_udpserver.c
 *
 * Servidor de comandos por UDP y sesion de subida de firmware, portado del
 * `udpserver' de xinu-s3-amp sobre la pila UDP nativa del P4 (device/udp,
 * NUDP=2).
 *
 * El bucle es el del S3, y la razon de cada paso no ha cambiado:
 *   udpAlloc -> open(ip local, NULL, puerto) -> UDP_CTRL_SETFLAG con
 *   UDP_FLAG_PASSIVE (en modo pasivo read() entrega pseudo-header + cabecera
 *   + payload, y ahi es donde esta la IP de origen de la consulta) ->
 *   UDP_CTRL_BIND con el puerto y la IP remotos para poder responder ->
 *   UDP_CTRL_BIND 0/NULL al terminar.
 *
 * Que se ha dejado fuera del del S3, y por que:
 *  - GPIO: el S3 usa xt_gpio_* (Xtensa). En el P4 el controlador es otro y no
 *    hay ningun pin que merezca la pena mover por la red.
 *  - TFT / RGB / BUTTON: no existen como devices en este arbol. El LCD
 *    (device/lcd) lo posee el dash; pintar por UDP encima del dash es una
 *    carrera segura, y la placa no tiene LED RGB.
 *
 * Y lo que en el P4 no puede ser igual:
 *  - La libreria FAT del P4 **no tiene fl_rename()**. El paso final se hace
 *    con fs_copy() + fl_remove() encajados en fatLibLock()/fatLibUnlock(): el
 *    monitor de la libreria es reentrante para el hilo dueno (system/lock.c),
 *    asi que la secuencia va completa sin que `ls' o el httpd se metan en
 *    medio. No es atomico como un renombre de entrada de directorio: si el
 *    corte cae en mitad de la copia queda un /xinu.img incompleto, y por eso
 *    se vuelve a verificar la talla por nombre antes de borrar el temporal.
 *  - El **apply real sigue bloqueado**: en este arbol no hay driver de
 *    escritura ni de borrado de la flash y el SBL no mira la SD (documento 05
 *    §6). FW_END deja la imagen en /xinu.img y lo dice en la respuesta; no
 *    reinicia la placa, porque reiniciar no cambiaria nada y costaria la
 *    sesion.
 *  - `psram_size' del S3 era una variable del driver; aqui el heap PSRAM es la
 *    lista psramlist, y eso es exactamente lo que responden MEM y PSRAM.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <conf.h>

#if NUDP

#include <clock.h>
#include <device.h>
#include <fat.h>
#include <fsutil.h>
#include <ipv4.h>
#include <memory.h>
#include <network.h>
#include <platform.h>
#include <sdblock.h>
#include <shell.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <udp.h>
#include <version.h>

#define UDPSERVER_PORT      5000
/* 12 KB, igual que en el S3: por debajo del bucle estan la libreria FAT y el
 * driver de la tarjeta, y la respuesta se construye con 1 KB mas. */
#define UDPSERVER_STK       12288
#define UDPSERVER_REPLY     1024
#define UDPSERVER_RX        (sizeof(struct udpPseudoHdr) + UDP_HDR_LEN + \
                             UDP_MAX_DATALEN)
#define UDPSERVER_MAXARGS   8

/* Nombre en el devtab (system/conf.c), no la macro ETH0: se resuelve con
 * getdev() como hacen `telnetserver -d', `netup' y `httpd -d'. */
#define UDPSERVER_DEVNAME   "ETH0"

/* Subida de firmware.  El tamanno de bloque deja sitio para la cabecera ASCII
 * mas larga dentro de un payload UDP (UDP_MAX_DATALEN = 1024). */
#define FW_BLOCK_SIZE       960
#define FW_MAX_IMAGE        0x200000UL
#define FW_TEMP_PATH        "/xinu.tmp"
#define FW_FINAL_PATH       "/xinu.img"

/* El unico servidor en marcha: una hebra, una consulta atendida.  Como en el
 * httpd, los buffers grandes van a ambito de fichero y no a la pila, para que
 * el pico de pila no dependa de la talla del payload. */
static bool udpServerStarted = FALSE;
static uchar udpRx[UDPSERVER_RX];
static char udpLine[UDPSERVER_REPLY];
static char udpRep[UDPSERVER_REPLY];

/** Estado de la subida en curso, si la hay. */
struct fwstate
{
    bool active;
    void *file;                 /**< handle de fl_fopen (es void* aqui)   */
    uint32_t expected;
    uint32_t received;
    uint32_t next_seq;
    struct netaddr client;
    ushort client_port;
};

static struct fwstate fw;

/** Buffer de salida acotado para udpAppend(). */
struct udpout
{
    char *buf;
    uint cap;
    uint len;
};

/** _doprnt() que nunca escribe mas alla del final del buffer. */
static int udpOutPutc(int c, uintptr_t arg)
{
    struct udpout *out = (struct udpout *)arg;

    if (out->len + 1 < out->cap)
    {
        out->buf[out->len++] = (char)c;
        out->buf[out->len] = '\0';
    }
    return (unsigned char)c;
}

/** Anade un fragmento formateado a un buffer ya terminados en NUL. */
static void udpAppend(char *buf, uint cap, const char *fmt, ...)
{
    struct udpout out;
    va_list ap;

    out.buf = buf;
    out.cap = cap;
    out.len = strlen(buf);

    va_start(ap, fmt);
    _doprnt(fmt, ap, udpOutPutc, (uintptr_t)&out);
    va_end(ap);
}

/** Parte una linea de comando en sitio. */
static int udpSplit(char *line, char **argv, int maxargs)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < maxargs)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t'
               && *p != '\r' && *p != '\n')
        {
            p++;
        }
        if (*p != '\0')
        {
            *p++ = '\0';
        }
    }

    return argc;
}

/**
 * Envfa una respuesta de texto.  La recepcion esta en modo pasivo para tener
 * el origen; para enviar se sale un momento del modo pasivo con el endpoint
 * ya enlazado por udpHandle().
 */
static void udpReply(int dev, const char *text)
{
    int len = strlen(text);

    if (len <= 0)
    {
        return;
    }

    control(dev, UDP_CTRL_CLRFLAG, UDP_FLAG_PASSIVE, 0);
    write(dev, text, len);
    control(dev, UDP_CTRL_SETFLAG, UDP_FLAG_PASSIVE, 0);
}

/** Cierra la subida actual, borrando el temporal si se pide. */
static void fwReset(bool removeTemp)
{
    if (NULL != fw.file)
    {
        fl_fclose(fw.file);
        fw.file = NULL;
    }
    if (removeTemp && fs_mounted())
    {
        fl_remove(FW_TEMP_PATH);
    }

    fw.active = FALSE;
    fw.expected = 0;
    fw.received = 0;
    fw.next_seq = 0;
    bzero(&fw.client, sizeof(fw.client));
    fw.client_port = 0;
}

/** TRUE si la peticion viene del cliente que abrio la sesion. */
static bool fwClientMatches(const struct netaddr *src, ushort srcport)
{
    return (NULL != src) && (fw.client_port == srcport)
        && netaddrequal(&fw.client, src);
}

/** Comprueba la talla que dice la entrada de directorio de @p path. */
static bool fwFileSizeIs(const char *path, uint32_t expected)
{
    void *file;
    bool ok;

    file = fl_fopen(path, "r");
    if (NULL == file)
    {
        return FALSE;
    }

    ok = (((FL_FILE *)file)->filelength == expected);
    fl_fclose(file);
    return ok;
}

/** Abre una subida nueva, descartando cualquier sesion anterior. */
static void fwBegin(char *reply, uint cap, unsigned long size,
                    const struct netaddr *src, ushort srcport)
{
    void *file;

    if (0 == size || size > FW_MAX_IMAGE)
    {
        udpAppend(reply, cap, "ERR FW_SIZE max=%lu\r\n",
                  (unsigned long)FW_MAX_IMAGE);
        return;
    }

    /* Un BEGIN nuevo tira el temporal viejo y cualquier sesion abierta. */
    fwReset(TRUE);

    if (!fs_mounted())
    {
        udpAppend(reply, cap, "ERR FW_NO_SD\r\n");
        return;
    }

    file = fl_fopen(FW_TEMP_PATH, "w");
    if (NULL == file)
    {
        udpAppend(reply, cap, "ERR FW_OPEN temp=%s\r\n", FW_TEMP_PATH);
        return;
    }

    fw.active = TRUE;
    fw.file = file;
    fw.expected = (uint32_t)size;
    fw.received = 0;
    fw.next_seq = 0;
    bzero(&fw.client, sizeof(fw.client));
    if (NULL != src)
    {
        netaddrcpy(&fw.client, src);
    }
    fw.client_port = srcport;

    udpAppend(reply, cap,
              "OK FW_BEGIN size=%lu block=%u temp=%s\r\n",
              (unsigned long)fw.expected, FW_BLOCK_SIZE, FW_TEMP_PATH);
}

/**
 * Atiende un bloque binario de firmware.
 *
 * Formato por el cable: "FW_DATA <seq> <len>\n" seguido de exactamente <len>
 * bytes.  Las secuencias empiezan en 0 y tienen que ser consecutivas; un
 * bloque retransmitido con seq anterior se confirma sin volver a escribirlo.
 */
static void fwHandleData(int dev, const char *data, int total,
                         const struct netaddr *src, ushort srcport)
{
    char header[64];
    char reply[256];
    const char *body;
    int headerLen = -1;
    int bodyLen;
    int i;
    unsigned long seq;
    unsigned int blockLen;

    reply[0] = '\0';

    if (!fw.active || NULL == fw.file)
    {
        udpAppend(reply, sizeof(reply), "ERR FW_IDLE\r\n");
        udpReply(dev, reply);
        return;
    }

    if (!fwClientMatches(src, srcport))
    {
        udpAppend(reply, sizeof(reply), "ERR FW_CLIENT\r\n");
        udpReply(dev, reply);
        return;
    }

    for (i = 0; i < total && i < (int)sizeof(header) - 1; i++)
    {
        if (data[i] == '\n')
        {
            headerLen = i;
            break;
        }
    }

    if (headerLen < 0)
    {
        udpAppend(reply, sizeof(reply), "ERR FW_HEADER\r\n");
        udpReply(dev, reply);
        return;
    }

    memcpy(header, data, headerLen);
    header[headerLen] = '\0';
    if (headerLen > 0 && header[headerLen - 1] == '\r')
    {
        header[headerLen - 1] = '\0';
    }

    if (2 != sscanf(header, "FW_DATA %lu %u", &seq, &blockLen))
    {
        udpAppend(reply, sizeof(reply), "ERR FW_USAGE\r\n");
        udpReply(dev, reply);
        return;
    }

    body = data + headerLen + 1;
    bodyLen = total - headerLen - 1;

    if (bodyLen != (int)blockLen)
    {
        udpAppend(reply, sizeof(reply),
                  "ERR FW_LEN header=%u actual=%d\r\n",
                  blockLen, bodyLen);
        udpReply(dev, reply);
        return;
    }

    if (0 == blockLen || blockLen > FW_BLOCK_SIZE)
    {
        udpAppend(reply, sizeof(reply),
                  "ERR FW_BLOCK max=%u\r\n", FW_BLOCK_SIZE);
        udpReply(dev, reply);
        return;
    }

    if (seq > fw.next_seq)
    {
        udpAppend(reply, sizeof(reply),
                  "ERR FW_SEQ expected=%lu got=%lu\r\n",
                  (unsigned long)fw.next_seq, seq);
        udpReply(dev, reply);
        return;
    }

    if (seq < fw.next_seq)
    {
        udpAppend(reply, sizeof(reply),
                  "OK FW_DATA %lu duplicate %lu/%lu\r\n",
                  seq, (unsigned long)fw.received,
                  (unsigned long)fw.expected);
        udpReply(dev, reply);
        return;
    }

    if (fw.received > fw.expected
        || blockLen > (fw.expected - fw.received))
    {
        udpAppend(reply, sizeof(reply),
                  "ERR FW_OVERFLOW received=%lu len=%u size=%lu\r\n",
                  (unsigned long)fw.received, blockLen,
                  (unsigned long)fw.expected);
        udpReply(dev, reply);
        return;
    }

    if (fl_fwrite(body, 1, (int)blockLen, fw.file) != (int)blockLen
        || fl_ftell(fw.file) != (long)(fw.received + blockLen))
    {
        fwReset(TRUE);
        udpAppend(reply, sizeof(reply), "ERR FW_WRITE\r\n");
        udpReply(dev, reply);
        return;
    }

    fw.received += blockLen;
    fw.next_seq++;

    udpAppend(reply, sizeof(reply),
              "OK FW_DATA %lu %lu/%lu%s\r\n",
              seq, (unsigned long)fw.received, (unsigned long)fw.expected,
              (fw.received == fw.expected) ? " complete" : "");
    udpReply(dev, reply);
}

/**
 * Cierra la subida y deja la imagen en /xinu.img.
 *
 * Sin fl_rename() en la libreria FAT del P4, la fase final es
 * comprobar-talla -> fs_copy -> volver-a-comprobar-por-nombre -> borrar
 * temporal, todo con el candado de la libreria tomado una sola vez (el monitor
 * es reentrante para el dueno, y cada fl_* lo reentra sin bloquearse).
 */
static bool fwEnd(char *reply, uint cap, const struct netaddr *src,
                  ushort srcport)
{
    uint32_t size;
    int copyRc;

    if (!fw.active || NULL == fw.file)
    {
        udpAppend(reply, cap, "ERR FW_IDLE\r\n");
        return FALSE;
    }

    if (!fwClientMatches(src, srcport))
    {
        udpAppend(reply, cap, "ERR FW_CLIENT\r\n");
        return FALSE;
    }

    if (fw.received != fw.expected)
    {
        udpAppend(reply, cap,
                  "ERR FW_SIZE expected=%lu received=%lu\r\n",
                  (unsigned long)fw.expected,
                  (unsigned long)fw.received);
        return FALSE;
    }

    size = fw.expected;

    fl_fclose(fw.file);
    fw.file = NULL;

    fatLibLock();

    if (!fwFileSizeIs(FW_TEMP_PATH, size))
    {
        fatLibUnlock();
        fwReset(TRUE);
        udpAppend(reply, cap, "ERR FW_VERIFY\r\n");
        return FALSE;
    }

    /* La libreria FAT no trunca: fl_fopen(...,"w") sobre un fichero que ya
     * existe abre el existente CON SU TALLA VIEJA (FILE_ERASE se marca en
     * fat_filelib.c:735 y nadie lo lee; la rama "Write Existing" de las lineas
     * 791-795 devuelve el fichero con su filelength y su cadena de clusters),
     * asi que una imagen nueva mas pequena dejaria la cola de la anterior y el
     * filelength del directorio viejo.  Se borra el destino primero: la copia
     * nace en un nombre libre. */
    fl_remove(FW_FINAL_PATH);

    copyRc = fs_copy(FW_TEMP_PATH, FW_FINAL_PATH);

    /* La comprobacion es sobre el nombre final: si el corte deja la copia a
     * medias, aqui se ve y se borra.  Nunca queda una imagen parcial con el
     * nombre que leeria un futuro cargador. */
    if (OK != copyRc || !fwFileSizeIs(FW_FINAL_PATH, size))
    {
        fl_remove(FW_FINAL_PATH);
        fatLibUnlock();
        fwReset(TRUE);
        udpAppend(reply, cap, "ERR FW_FINALIZE\r\n");
        return FALSE;
    }

    fl_remove(FW_TEMP_PATH);

    fatLibUnlock();

    fw.active = FALSE;
    fw.expected = 0;
    fw.received = 0;
    fw.next_seq = 0;

    /* apply=blocked: no hay driver de flash en este arbol y el SBL no lee la
     * SD.  Documentado en docs/port-esp32p4/05-roadmap-y-conf.md §6. */
    udpAppend(reply, cap, "OK FW_END size=%lu staged=%s apply=blocked\r\n",
              (unsigned long)size, FW_FINAL_PATH);
    return TRUE;
}

/** Aborta la subida en curso. */
static void fwAbort(char *reply, uint cap, const struct netaddr *src,
                    ushort srcport)
{
    if (fw.active && !fwClientMatches(src, srcport))
    {
        udpAppend(reply, cap, "ERR FW_CLIENT\r\n");
        return;
    }

    fwReset(TRUE);
    udpAppend(reply, cap, "OK FW_ABORT\r\n");
}

/** Informa del estado de la subida. */
static void fwStatus(char *reply, uint cap)
{
    if (!fw.active)
    {
        udpAppend(reply, cap, "FW_STATUS IDLE");

        /* Si hay imagen ya apilada en la tarjeta se reporta: el cliente tiene
         * que poder distinguir "subida a medias" de "imagen lista, apply sin
         * mecanismo". */
        if (fs_mounted())
        {
            void *f = fl_fopen(FW_FINAL_PATH, "r");

            if (NULL != f)
            {
                udpAppend(reply, cap, " staged=%s size=%lu",
                          FW_FINAL_PATH,
                          (unsigned long)((FL_FILE *)f)->filelength);
                fl_fclose(f);
            }
        }
        udpAppend(reply, cap, "\r\n");
        return;
    }

    udpAppend(reply, cap,
              "FW_STATUS ACTIVE size=%lu received=%lu next=%lu block=%u "
              "temp=%s\r\n",
              (unsigned long)fw.expected, (unsigned long)fw.received,
              (unsigned long)fw.next_seq, FW_BLOCK_SIZE, FW_TEMP_PATH);
}

/** Anade los campos de la interfaz, compartidos por INFO y NET. */
static void udpAppendNet(char *reply, uint cap)
{
    struct netif *iface;
    char ip[32];
    char mask[32];
    char gateway[32];
    char mac[32];

    iface = netLookup(getdev(UDPSERVER_DEVNAME));
    if (NULL == iface)
    {
        udpAppend(reply, cap, "ERR no network interface\r\n");
        return;
    }

    netaddrsprintf(ip, &iface->ip);
    netaddrsprintf(mask, &iface->mask);
    netaddrsprintf(gateway, &iface->gateway);
    netaddrsprintf(mac, &iface->hwaddr);

    udpAppend(reply, cap, "ip: %s\r\n", ip);
    udpAppend(reply, cap, "netmask: %s\r\n", mask);
    udpAppend(reply, cap, "gateway: %s\r\n", gateway);
    udpAppend(reply, cap, "mac: %s\r\n", mac);
    udpAppend(reply, cap, "mtu: %u\r\n", iface->mtu);
}

/**
 * Familia PSRAM: el segundo heap del P4, la lista psramlist sobre
 * [psminheap, psmaxheap).  Los totales salen de la cabecera de la lista, sin
 * recorrerla: el recorrido con cota y validaciones ya lo hace `psram'.
 */
static void udpAppendPsram(char *reply, uint cap)
{
    unsigned long lo = (unsigned long)(uintptr_t)psminheap;
    unsigned long hi = (unsigned long)(uintptr_t)psmaxheap;
    unsigned long total = (hi > lo) ? (hi - lo) : 0UL;
    unsigned long freeBytes = (unsigned long)psramlist.length;

    udpAppend(reply, cap, "psram_range: 0x%08lx-0x%08lx\r\n", lo, hi);
    udpAppend(reply, cap, "psram_size: %lu\r\n", total);
    udpAppend(reply, cap, "psram_free: %lu\r\n", freeBytes);
    udpAppend(reply, cap, "psram_used: %lu\r\n",
              (total > freeBytes) ? (total - freeBytes) : 0UL);
    if (0UL == total)
    {
        udpAppend(reply, cap, "psram: sin inicializar (el device psram no ha "
                              "montado el heap)\r\n");
    }
}

/** Familia SD: geometria de la tarjeta leida del driver, mas el estado FAT. */
static void udpHandleSd(char *reply, uint cap, int argc, char **argv)
{
    if (argc < 2 || 0 != strcmp(argv[1], "INFO"))
    {
        udpAppend(reply, cap, "ERR usage: SD INFO\r\n");
        return;
    }

    if (!sd_present)
    {
        udpAppend(reply, cap, "SD: no card\r\n");
        return;
    }

    udpAppend(reply, cap, "SD: present\r\n");
    udpAppend(reply, cap, "blocks: %lu\r\n", (unsigned long)sd_blocks);
    udpAppend(reply, cap, "size_mb: %lu\r\n",
              (unsigned long)(sd_blocks / 2048UL));
    udpAppend(reply, cap, "type: %s\r\n", sd_hc ? "SDHC/SDXC" : "SDSC");
    udpAppend(reply, cap, "bus: %u-bit\r\n", (unsigned int)sd_width);
    udpAppend(reply, cap, "rca: 0x%04lx\r\n", (unsigned long)sd_rca);
    udpAppend(reply, cap, "fat: %s\r\n",
              fs_mounted() ? "mounted" : "not mounted");
}

/** Atiende una peticion. */
static void udpHandle(int dev, const char *data, int len,
                      const struct netaddr *src, ushort srcport)
{
    static const char *const pstnams[] = {
        "curr", "free", "ready", "recv", "sleep", "susp", "wait", "rtim",
        "migr"
    };
    char *argv[UDPSERVER_MAXARGS];
    unsigned long size;
    int argc;
    int i;

    if (len <= 0)
    {
        return;
    }

    /* FW_DATA lleva una cabecera ASCII corta seguida de bytes binarios, asi
     * que esquiva el separador de texto y su buffer terminado en NUL. */
    if (len >= 8 && 0 == memcmp(data, "FW_DATA", 7) && data[7] == ' ')
    {
        fwHandleData(dev, data, len, src, srcport);
        return;
    }

    if (len >= (int)sizeof(udpLine))
    {
        len = (int)sizeof(udpLine) - 1;
    }
    memcpy(udpLine, data, len);
    udpLine[len] = '\0';

    /* Se quitan los finales de linea y espacios que deja un cliente de texto. */
    while (len > 0
           && (udpLine[len - 1] == '\r' || udpLine[len - 1] == '\n'
               || udpLine[len - 1] == ' ' || udpLine[len - 1] == '\t'))
    {
        udpLine[--len] = '\0';
    }

    argc = udpSplit(udpLine, argv, UDPSERVER_MAXARGS);
    if (argc < 1)
    {
        return;
    }

    udpRep[0] = '\0';

    if (0 == strcmp(argv[0], "PING"))
    {
        udpAppend(udpRep, sizeof(udpRep), "PONG %lu\r\n",
                  (unsigned long)clktotal);
    }
    else if (0 == strcmp(argv[0], "INFO"))
    {
        udpAppend(udpRep, sizeof(udpRep), "Xinu ESP32-P4\r\n");
        udpAppend(udpRep, sizeof(udpRep), "version: %s\r\n", VERSION);
        udpAppend(udpRep, sizeof(udpRep), "uptime_ms: %lu\r\n",
                  (unsigned long)clktotal);
        udpAppendNet(udpRep, sizeof(udpRep));
        udpAppend(udpRep, sizeof(udpRep), "heap_free: %lu\r\n",
                  (unsigned long)memlist.length);
        udpAppendPsram(udpRep, sizeof(udpRep));
    }
    else if (0 == strcmp(argv[0], "VERSION"))
    {
        udpAppend(udpRep, sizeof(udpRep), "%s\r\n", VERSION);
    }
    else if (0 == strcmp(argv[0], "UPTIME"))
    {
        udpAppend(udpRep, sizeof(udpRep), "uptime_ms: %lu\r\n",
                  (unsigned long)clktotal);
    }
    else if (0 == strcmp(argv[0], "MEM"))
    {
        /* L2MEM: [memheap, platform.maxaddr) con el total libre en la
         * cabecera de memlist.  PSRAM va en su propia familia. */
        unsigned long lo = (unsigned long)(uintptr_t)memheap;
        unsigned long hi = (unsigned long)(uintptr_t)platform.maxaddr;
        unsigned long total = (hi > lo) ? (hi - lo) : 0UL;
        unsigned long freeBytes = (unsigned long)memlist.length;

        udpAppend(udpRep, sizeof(udpRep), "heap_range: 0x%08lx-0x%08lx\r\n",
                  lo, hi);
        udpAppend(udpRep, sizeof(udpRep), "heap_size: %lu\r\n", total);
        udpAppend(udpRep, sizeof(udpRep), "heap_free: %lu\r\n", freeBytes);
        udpAppend(udpRep, sizeof(udpRep), "heap_used: %lu\r\n",
                  (total > freeBytes) ? (total - freeBytes) : 0UL);
        udpAppendPsram(udpRep, sizeof(udpRep));
    }
    else if (0 == strcmp(argv[0], "PSRAM"))
    {
        udpAppendPsram(udpRep, sizeof(udpRep));
    }
    else if (0 == strcmp(argv[0], "PS"))
    {
        uint32_t total0;
        uint32_t total1;
        uint32_t elapsed;
        uint32_t ticks0[NTHREAD];
        uint32_t gen0[NTHREAD];

        /* Uso de CPU en una ventana de 1 s reciente: el promedio desde el
         * arranque hacia que la hebra null (idle) saliera siempre al 99 % y
         * las demas tareas se redondearan a 0 %. */
        total0 = (uint32_t)clktotal;
        for (i = 0; i < NTHREAD; i++)
        {
            ticks0[i] = thrtab[i].cpu_ticks;
            gen0[i] = thrtab[i].gen;
        }

        sleep(1000);

        total1 = (uint32_t)clktotal;
        elapsed = total1 - total0;
        if (elapsed == 0)
        {
            elapsed = 1;
        }

        udpAppend(udpRep, sizeof(udpRep),
                  "TID NAME             STATE PRIO PPID CPU% STACK\r\n");

        for (i = 0; i < NTHREAD; i++)
        {
            struct thrent *thrptr = &thrtab[i];
            uint32_t used;
            uint32_t window;
            uint32_t pct;

            if (thrptr->state == THRFREE)
            {
                continue;
            }

            if (thrptr->gen == gen0[i] && thrptr->born_ticks <= total0)
            {
                used = thrptr->cpu_ticks - ticks0[i];
                window = elapsed;
            }
            else
            {
                used = thrptr->cpu_ticks;
                window = (uint32_t)clktotal - thrptr->born_ticks;
                if (window == 0)
                {
                    window = 1;
                }
            }

            if (used > window)
            {
                used = window;
            }

            if (window >= 100)
            {
                pct = used / (window / 100);
            }
            else
            {
                pct = (used * 100) / window;
            }
            if (pct > 100)
            {
                pct = 100;
            }

            udpAppend(udpRep, sizeof(udpRep),
                      "%3d %-16s %-5s %4d %4d %3lu%% %lu/%lu\r\n",
                      i, thrptr->name,
                      (thrptr->state >= 1 && thrptr->state <= 9)
                          ? pstnams[(int)thrptr->state - 1] : "?????",
                      thrptr->prio, thrptr->parent,
                      (unsigned long)pct,
                      (unsigned long)thrptr->stk_peak,
                      (unsigned long)thrptr->stklen);
        }
    }
    else if (0 == strcmp(argv[0], "NET"))
    {
        udpAppendNet(udpRep, sizeof(udpRep));
    }
    else if (0 == strcmp(argv[0], "SD"))
    {
        udpHandleSd(udpRep, sizeof(udpRep), argc, argv);
    }
    else if (0 == strcmp(argv[0], "FW_BEGIN"))
    {
        if (argc < 2 || 1 != sscanf(argv[1], "%lu", &size))
        {
            udpAppend(udpRep, sizeof(udpRep),
                      "ERR usage: FW_BEGIN <image-size>\r\n");
        }
        else
        {
            fwBegin(udpRep, sizeof(udpRep), size, src, srcport);
        }
    }
    else if (0 == strcmp(argv[0], "FW_END"))
    {
        fwEnd(udpRep, sizeof(udpRep), src, srcport);
    }
    else if (0 == strcmp(argv[0], "FW_ABORT"))
    {
        fwAbort(udpRep, sizeof(udpRep), src, srcport);
    }
    else if (0 == strcmp(argv[0], "FW_STATUS"))
    {
        fwStatus(udpRep, sizeof(udpRep));
    }
    else if (0 == strcmp(argv[0], "REBOOT"))
    {
        udpAppend(udpRep, sizeof(udpRep), "OK rebooting\r\n");
        udpReply(dev, udpRep);
        sleep(200);
        xsh_reset(1, argv);
        return;
    }
    else if (0 == strcmp(argv[0], "HELP"))
    {
        udpAppend(udpRep, sizeof(udpRep),
                  "Commands: PING INFO VERSION UPTIME MEM PSRAM PS NET SD "
                  "FW_BEGIN FW_DATA FW_END FW_ABORT FW_STATUS REBOOT HELP\r\n");
        udpAppend(udpRep, sizeof(udpRep),
                  "Firmware: FW_BEGIN <size> | FW_DATA <seq> <len>\\n<bytes> "
                  "| FW_END | FW_STATUS | FW_ABORT\r\n");
        udpAppend(udpRep, sizeof(udpRep),
                  "Nota: FW_END deja /xinu.img en la SD; el apply esta "
                  "bloqueado (sin driver de flash, el SBL no lee la SD)\r\n");
    }
    else
    {
        udpAppend(udpRep, sizeof(udpRep), "ERR unknown command '%s'\r\n",
                  argv[0]);
    }

    udpReply(dev, udpRep);
}

/**
 * Servidor UDP.  Usa un dispositivo UDP en modo pasivo para que cada peticion
 * traiga la direccion del emisor, enlaza el endpoint remoto para la respuesta
 * y lo vuelve a soltar.
 */
thread udpServer(int ethdev, int port)
{
    struct netif *iface;
    ushort udpdev;
    int n;
    int payloadLen;
    struct udpPseudoHdr *pseudo;
    struct udpPkt *pkt;
    struct netaddr src;

    iface = netLookup(ethdev);
    if (NULL == iface)
    {
        fprintf(stderr, "udpserver: no network interface\n");
        udpServerStarted = FALSE;
        return SYSERR;
    }

    udpdev = udpAlloc();
    if ((ushort)SYSERR == udpdev)
    {
        fprintf(stderr, "udpserver: no UDP device available\n");
        udpServerStarted = FALSE;
        return SYSERR;
    }

    if (SYSERR == open(udpdev, &iface->ip, NULL, port, 0))
    {
        fprintf(stderr, "udpserver: cannot open UDP port %d\n", port);
        close(udpdev);
        udpServerStarted = FALSE;
        return SYSERR;
    }

    if (SYSERR == control(udpdev, UDP_CTRL_SETFLAG, UDP_FLAG_PASSIVE, 0))
    {
        fprintf(stderr, "udpserver: cannot set passive mode\n");
        close(udpdev);
        udpServerStarted = FALSE;
        return SYSERR;
    }

    fprintf(stderr, "udpserver: escuchando en UDP puerto %d\n", port);

    while (TRUE)
    {
        n = read(udpdev, udpRx, sizeof(udpRx));
        if (n < (int)(sizeof(struct udpPseudoHdr) + UDP_HDR_LEN))
        {
            if (SYSERR == n)
            {
                break;
            }
            continue;
        }

        pseudo = (struct udpPseudoHdr *)udpRx;
        pkt = (struct udpPkt *)(pseudo + 1);

        if (pkt->len < UDP_HDR_LEN)
        {
            continue;
        }

        payloadLen = pkt->len - UDP_HDR_LEN;
        if (payloadLen < 0
            || n < (int)(sizeof(struct udpPseudoHdr) + UDP_HDR_LEN
                         + payloadLen))
        {
            continue;
        }

        /* La capa UDP ya paso los campos de la cabecera a orden de maquina. */
        bzero(&src, sizeof(src));
        src.type = NETADDR_IPv4;
        src.len = IPv4_ADDR_LEN;
        memcpy(src.addr, pseudo->srcIp, IPv4_ADDR_LEN);

        if (SYSERR == control(udpdev, UDP_CTRL_BIND, pkt->srcPort,
                              (long)&src))
        {
            continue;
        }

        udpHandle(udpdev, (const char *)pkt->data, payloadLen,
                  &src, pkt->srcPort);

        control(udpdev, UDP_CTRL_BIND, 0, (long)NULL);
    }

    close(udpdev);
    udpServerStarted = FALSE;
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Arranca el servidor de comandos por UDP.
 *
 * @param nargs numero de argumentos en args
 * @param args  args[1] es el puerto UDP; 5000 por defecto
 * @return SHELL_OK si va bien, SHELL_ERROR si no
 */
shellcmd xsh_udpserver(int nargs, char *args[])
{
    int port = UDPSERVER_PORT;
    int ethdev;
    tid_typ tid;

    if (2 == nargs && 0 == strcmp(args[1], "--help"))
    {
        printf("Usage: udpserver [puerto]\n\n");
        printf("Description:\n");
        printf("\tServidor de comandos por UDP y subida de firmware.\n");
        printf("\tComandos por el cable: HELP, INFO, PING, MEM, PSRAM, PS,\n");
        printf("\tNET, SD INFO, VERSION, UPTIME, REBOOT y la familia FW_*.\n");
        printf("\tFW_END deja la imagen en /xinu.img de la SD; el apply a\n");
        printf("\tflash sigue bloqueado en este puerto (documento 05 §6).\n");
        return SHELL_OK;
    }

    if (nargs > 1)
    {
        port = atoi(args[1]);
    }
    if (port <= 0 || port > 65535)
    {
        printf("Usage: udpserver [port]\n");
        return SHELL_ERROR;
    }

    ethdev = getdev(UDPSERVER_DEVNAME);
    if (SYSERR == ethdev)
    {
        fprintf(stderr, "udpserver: no existe %s\n", UDPSERVER_DEVNAME);
        return SHELL_ERROR;
    }

    if (udpServerStarted)
    {
        printf("udpserver: ya esta en marcha\n");
        return SHELL_OK;
    }

    tid = create((void *)udpServer, UDPSERVER_STK, INITPRIO, "UDPSERVER",
                 2, ethdev, port);
    if (SYSERR == tid)
    {
        printf("udpserver: no se pudo crear la hebra\n");
        return SHELL_ERROR;
    }

    udpServerStarted = TRUE;
    ready(tid, RESCHED_YES);
    return SHELL_OK;
}

#endif /* NUDP */
