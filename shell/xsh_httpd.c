/**
 * @file xsh_httpd.c
 *
 * Servidor HTTP sobre la pila TCP nativa de Xinu y el FAT de la microSD.
 *
 * Es el `httpd' de xinu-s3-amp portado: escucha en el 80, sirve ficheros
 * estaticos desde la tarjeta (`GET /ruta') y despacha la API REST de ficheros
 * de `shell/httpapi.c' (/api/files, /api/download, /api/file PUT/DELETE,
 * /api/mkdir, /api/rmdir).  Se queda fuera lo que en el P4 aun no tiene donde
 * apoyarse: webterm y sus /api/exec + /api/job.
 *
 * La pagina embebida (httpd_index) sigue vivo como respaldo de `/` y
 * `/index.html` cuando la tarjeta no tiene ese fichero: es la prueba de la W5
 * tal como se validou, y permite arrancar el servidor antes de subir nada a
 * la SD.  En cuanto la tarjeta tiene /index.html, ese manda.
 *
 * Dos cosas que no son ornamentales y vienen medidas en placa:
 *  - el read() del request lleva timeout (TCP_CTRL_RCVTIMEO), porque un
 *    navegador abre conexiones especulativas y manda nada; sin timeout el
 *    servidor se queda colgado en la primera y ya no acepta ninguna;
 *  - el close() de la conexion se delega en una hebra corta, porque close()
 *    de un TCP bloquea hasta que el otro lado confirma el FIN y mientras no
 *    hay socket a la escucha.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <conf.h>

#if NTCP

#include <device.h>
#include <network.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>
#include <thread.h>
#include <fat_filelib.h>
#include <fsutil.h>
#include <xsh_http.h>

#define HTTPD_PORT      80
/* 2 KB: el request completo de un PUT con cabeceras de curl y una ruta larga
 * no cabe en los 512 B del servidor de prueba; el body se lee aparte. */
#define HTTPD_RQMAX     2048
/* 8 KB: servir de la SD mete la libreria FAT y el driver de tarjeta por
 * debajo del bucle, y la API llama a fl_fwrite con su propio camino. */
#define HTTPD_STK       8192
#define HTTPD_RCVTIMEO  2000

/* Nombre en el devtab (system/conf.c), no el ETH0 del header: el comando lo
 * pasa por getdev(), igual que `telnetserver -d'. */
#define HTTPD_DEVNAME   "ETH0"

static const char httpd_index[] =
"<!DOCTYPE html>\n"
"<html lang=\"es\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<title>Xinu en ESP32-P4</title>\n"
"<style>body{font-family:monospace;background:#101418;color:#d8e0e8;"
"margin:3em auto;max-width:36em;padding:0 1em}"
"h1{font-size:1.3em;color:#5ad0a0}code{color:#ffd479}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>Embedded Xinu &mdash; ESP32-P4 + ESP32-C6</h1>\n"
"<p>El C6 esta asociado a la AP, <code>netup ETH0</code> subio la interfaz por "
"DHCP y esta pagina salio por la pila TCP nativa de Xinu "
"(tcpAllocPassive/open(TCP_PASSIVE)/read/write), sin lwIP.</p>\n"
"<p>Esto es el respaldo embebido: la microSD todavia no tiene "
"<code>/index.html</code>.  Subelo con</p>\n"
"<p><code>curl -T pagina.html \"http://IP_TABLERO/api/file?path=/index.html\"</code></p>\n"
"<p>Servidores: <code>telnet puerto 23</code>, <code>http puerto 80</code>, "
"API de ficheros en <code>/api/</code>.</p>\n"
"</body>\n"
"</html>\n";

static volatile int httpdRunning = 0;

/* Un solo servidor a la vez y una conexion cada vez: el buffer del request y
 * el request parseado viven en .bss en vez de en la pila de la hebra. */
static char httpdReq[HTTPD_RQMAX + 1];
static struct http_request httpdRq;

static const char *mime_type(const char *name)
{
    const char *dot = strrchr(name, '.');

    if (!dot)
        return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm"))
        return "text/html";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".log"))
        return "text/plain";
    if (!strcmp(dot, ".css"))
        return "text/css";
    if (!strcmp(dot, ".js"))
        return "application/javascript";
    if (!strcmp(dot, ".json"))
        return "application/json";
    if (!strcmp(dot, ".png"))
        return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"))
        return "image/jpeg";
    if (!strcmp(dot, ".gif"))
        return "image/gif";
    if (!strcmp(dot, ".ico"))
        return "image/x-icon";
    if (!strcmp(dot, ".svg"))
        return "image/svg+xml";
    return "application/octet-stream";
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decodifica %XX del path.  Un '+' literal sigue siendo un plus en la ruta. */
static bool url_decode_path(char *s)
{
    char *r = s;
    char *w = s;

    while (*r)
    {
        if (*r == '%')
        {
            int hi;
            int lo;
            char c;

            if (!r[1] || !r[2])
                return FALSE;
            hi = hexval((unsigned char)r[1]);
            lo = hexval((unsigned char)r[2]);

            if (hi < 0 || lo < 0)
                return FALSE;
            c = (char)((hi << 4) | lo);
            if (c == '\0')
                return FALSE;
            *w++ = c;
            r += 3;
        }
        else
        {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return TRUE;
}

/* Parsea "METHOD /path?query HTTP/1.x".  La query se guarda cruda: la API
 * decodifica cada valor por separado, asi un %26 sigue siendo un valor. */
static bool parse_request(const char *req, struct http_request *r)
{
    const char *p;
    const char *q;
    const char *qm;
    uint len;
    uint plen;
    uint qlen;

    p = strchr(req, ' ');
    if (!p)
        return FALSE;
    len = (uint)(p - req);
    if (len == 0 || len >= sizeof(r->method))
        return FALSE;
    memcpy(r->method, req, len);
    r->method[len] = '\0';

    p++;
    q = strchr(p, ' ');
    if (!q)
        return FALSE;
    len = (uint)(q - p);
    if (len == 0)
        return FALSE;

    qm = memchr(p, '?', len);
    plen = qm ? (uint)(qm - p) : len;
    qlen = qm ? (uint)(q - qm - 1) : 0;
    if (plen >= sizeof(r->path) || qlen >= sizeof(r->query))
        return FALSE;

    memcpy(r->path, p, plen);
    r->path[plen] = '\0';
    r->query[0] = '\0';
    if (qlen)
    {
        memcpy(r->query, qm + 1, qlen);
        r->query[qlen] = '\0';
    }

    if (r->path[0] != '/')
        return FALSE;
    return url_decode_path(r->path);
}

/** Devuelve la posicion justo despues de la linea en blanco que cierra las
 *  cabeceras, o -1 si no esta todavia. */
static int httpdHeaderEnd(const char *req, int used)
{
    int i;

    for (i = 0; i + 1 < used; i++)
    {
        if (req[i] == '\n' && req[i + 1] == '\n')
            return i + 2;
        if (i + 3 < used && req[i] == '\r' && req[i + 1] == '\n'
            && req[i + 2] == '\r' && req[i + 3] == '\n')
            return i + 4;
    }
    return -1;
}

static int ci_ncmp(const char *a, const char *b, int n)
{
    while (n-- > 0)
    {
        int ca = (unsigned char)*a++;
        int cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return ca - cb;
        if (!ca)
            return 0;
    }
    return 0;
}

static long content_length(const char *req, int hdrlen)
{
    const char *p = req;
    const char *end = req + hdrlen;

    while (p < end)
    {
        const char *e = p;

        while (e < end && *e != '\n')
            e++;

        if (ci_ncmp(p, "Content-Length:", 15) == 0)
        {
            p += 15;
            while (p < e && (*p == ' ' || *p == '\t'))
                p++;
            return atoi(p);
        }
        p = e + 1;
    }
    return -1;
}

static void httpdSend(int tcpdev, const char *body, int blen,
                      const char *ctype, const char *status)
{
    char hdr[160];
    int hlen;

    hlen = sprintf(hdr, "HTTP/1.0 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n", status, ctype, blen);
    if (SYSERR == write(tcpdev, hdr, hlen))
    {
        return;
    }
    if (blen > 0)
    {
        (void)write(tcpdev, (void *)body, blen);
    }
}

static void send_error(int dev, int code, const char *text)
{
    char status[48];

    sprintf(status, "%d %s", code, text);
    httpdSend(dev, text, (int)strlen(text), "text/plain", status);
}

/**
 * Sirve un fichero de la SD.  `/` y `/index.html` caen a la pagina embebida
 * si la tarjeta no los tiene, que es el estado del arranque en frio.
 */
static void serve_static(int dev, const char *path)
{
    char full[FS_PATH_MAX];
    char hdr[256];
    char buf[512];
    void *f;
    uint len;
    int r;

    if (strcmp(path, "/") == 0)
        path = "/index.html";
    if (!fs_mounted() || !fs_resolve(path, full, sizeof(full)))
    {
        if (!strcmp(path, "/index.html"))
            httpdSend(dev, httpd_index, (int)sizeof(httpd_index) - 1,
                      "text/html; charset=utf-8", "200 OK");
        else
            send_error(dev, 404, "Not Found");
        return;
    }

    f = fl_fopen(full, "r");
    if (!f)
    {
        if (!strcmp(path, "/index.html"))
        {
            httpdSend(dev, httpd_index, (int)sizeof(httpd_index) - 1,
                      "text/html; charset=utf-8", "200 OK");
            return;
        }
        send_error(dev, 404, "Not Found");
        return;
    }

    fl_fseek(f, 0, 2);
    len = (uint)fl_ftell(f);
    fl_fseek(f, 0, 0);

    sprintf(hdr,
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n\r\n",
            mime_type(full), (unsigned long)len);
    if (SYSERR == write(dev, hdr, strlen(hdr)))
    {
        fl_fclose(f);
        return;
    }

    while ((r = fl_fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (write(dev, buf, r) != r)
            break;
    }
    fl_fclose(f);
}

/** Cierra la conexion sin esperarse al FIN del otro lado. */
static thread httpdReleaser(int dev)
{
    close(dev);
    return OK;
}

static void httpdRelease(int dev)
{
    tid_typ tid = create((void *)httpdReleaser, MINSTK, INITPRIO,
                         "httpdRel", 1, dev);

    if (SYSERR == tid)
    {
        close(dev);
        return;
    }
    ready(tid, RESCHED_YES);
}

/**
 * Bucle del servidor: una conexion atendida cada vez.
 *
 * El primer argumento es un numero de device ya resuelto por el comando (con
 * getdev() a partir de nombre), igual que hace `telnetserver': el servidor no
 * tiene por que saber como se llama la interfaz en xinu.conf.
 */
thread httpdServer(int dev, int port)
{
    struct netif *iface;
    int tcpdev;
    int n;
    int used;
    int hdrlen;
    long clen;

    iface = netLookup(dev);
    if (NULL == iface)
    {
        fprintf(stderr, "httpd: no hay interfaz %d arriba\n", dev);
        httpdRunning = 0;
        return SYSERR;
    }

    while (TRUE)
    {
        tcpdev = tcpAllocPassive();
        if (SYSERR == (short)tcpdev)
        {
            sleep(100);
            continue;
        }

        if (open(tcpdev, &iface->ip, NULL, port, NULL, TCP_PASSIVE) < 0)
        {
            close(tcpdev);
            sleep(100);
            continue;
        }

        control(tcpdev, TCP_CTRL_RCVTIMEO, HTTPD_RCVTIMEO, 0);

        /* Se leen las cabeceras; el bucle para en la linea en blanco, pero un
         * cliente rapido ya habra mandado parte del body: eso se pasa a la
         * API como prebody. */
        used = 0;
        httpdReq[0] = '\0';
        while (used < HTTPD_RQMAX &&
               (n = read(tcpdev, httpdReq + used, HTTPD_RQMAX - used)) > 0)
        {
            used += n;
            httpdReq[used] = '\0';
            if (httpdHeaderEnd(httpdReq, used) > 0)
            {
                break;
            }
        }

        hdrlen = httpdHeaderEnd(httpdReq, used);

        if (hdrlen > 0 && parse_request(httpdReq, &httpdRq))
        {
            if (strncmp(httpdRq.path, "/api/", 5) == 0)
            {
                clen = content_length(httpdReq, hdrlen);

                /* curl -T espera esto antes de mandar el body. */
                if (strstr(httpdReq, "100-continue") != NULL)
                {
                    static const char cont[] = "HTTP/1.0 100 Continue\r\n\r\n";

                    write(tcpdev, cont, sizeof(cont) - 1);
                }

                httpApiHandle(tcpdev, &httpdRq, httpdReq + hdrlen,
                              used - hdrlen, clen);
            }
            else if (strcmp(httpdRq.method, "GET") == 0)
            {
                serve_static(tcpdev, httpdRq.path);
            }
            else
            {
                send_error(tcpdev, 405, "Method Not Allowed");
            }
        }
        else if (used > 0)
        {
            send_error(tcpdev, 400, "Bad Request");
        }

        httpdRelease(tcpdev);
    }

    httpdRunning = 0;
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (httpd): arranca el servidor HTTP.
 *
 * La interfaz se resuelve con getdev() sobre el nombre del devtab, no con el
 * ETH0 del header: `telnetserver' hace lo mismo con `-d' y asi el comando sigue
 * valiendo aunque el conf renumere los devices.
 *
 * @param nargs numero de argumentos en args
 * @param args  vector de argumentos
 * @return SHELL_OK si la hebra nacio
 */
shellcmd xsh_httpd(int nargs, char *args[])
{
    const char *devname = HTTPD_DEVNAME;
    int port = HTTPD_PORT;
    int descrp;
    tid_typ tid;
    int i;

    if (nargs > 1 && 0 == strcmp(args[1], "--help"))
    {
        printf("Uso:\n");
        printf("\thttpd [-d <dispositivo>] [-p <puerto>]\n");
        printf("Descripcion:\n");
        printf("\tSirve la microSD por HTTP y expone la API de ficheros\n");
        printf("\t/api/files, /api/download, /api/file (PUT/DELETE),\n");
        printf("\t/api/mkdir, /api/rmdir.\n");
        printf("\t-d dispositivo\tinterfaz donde escuchar (por defecto: %s)\n",
               HTTPD_DEVNAME);
        printf("\t-p puerto\tpuerto TCP (por defecto: %d)\n", HTTPD_PORT);
        return SHELL_OK;
    }

    for (i = 1; i < nargs; i++)
    {
        if (0 == strcmp(args[i], "-d"))
        {
            i++;
            if (i >= nargs)
            {
                fprintf(stderr, "httpd: falta el nombre del dispositivo\n");
                return SHELL_ERROR;
            }
            devname = args[i];
        }
        else if (0 == strcmp(args[i], "-p"))
        {
            i++;
            if (i >= nargs)
            {
                fprintf(stderr, "httpd: falta el puerto\n");
                return SHELL_ERROR;
            }
            port = atoi(args[i]);
        }
        else
        {
            fprintf(stderr, "httpd: argumento desconocido \"%s\"\n", args[i]);
            return SHELL_ERROR;
        }
    }

    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "httpd: puerto invalido\n");
        return SHELL_ERROR;
    }

    descrp = getdev(devname);
    if (isbaddev(descrp))
    {
        fprintf(stderr, "httpd: dispositivo \"%s\" invalido\n", devname);
        return SHELL_ERROR;
    }

    if (httpdRunning)
    {
        printf("httpd: ya esta escuchando\n");
        return SHELL_OK;
    }
    httpdRunning = 1;

    tid = create((void *)httpdServer, HTTPD_STK, INITPRIO, "httpd", 2,
                 descrp, port);
    if (SYSERR == tid)
    {
        httpdRunning = 0;
        fprintf(stderr, "httpd: no se pudo crear la hebra\n");
        return SHELL_ERROR;
    }
    ready(tid, RESCHED_NO);

    printf("httpd: escuchando en %s puerto %d\n", devname, port);
    return SHELL_OK;
}

#endif                          /* NTCP */
