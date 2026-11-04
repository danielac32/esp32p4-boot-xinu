/**
 * @file xsh_httpd.c
 *
 * Servidor HTTP de prueba: una sola pagina.
 *
 * Es el `httpd' de xinu-s3-amp recortado a lo minimo que hace falta aqui para
 * cerrar la W5 por encima de la pila TCP nativa de Xinu: escuchar en el 80,
 * leer la linea de request y devolver un index HTML que va embebido en el
 * propio .c.  Se queda fuera todo lo del S3 que aun no existe en el P4 (FAT,
 * /api/, webterm, memtty); cuando eso llegue, este fichero se vuelve el caso
 * particular "ruta estatica embebida" del servidor completo.
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

#define HTTPD_PORT      80
#define HTTPD_RQMAX     512
#define HTTPD_STK       4096
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
"<p>Datos del equipo:</p>\n"
"<ul>\n"
"<li>Sistema: <code>Embedded Xinu, puerto ESP32-P4</code></li>\n"
"<li>Interfaz: <code>ETH0 (ESP32-C6 sobre SDIO, esp_hosted)</code></li>\n"
"<li>Servidores: <code>telnet puerto 23</code>, <code>http puerto 80</code></li>\n"
"</ul>\n"
"<p>Pruebas: <code>GET /</code> devuelve esto; cualquier otra ruta da 404.</p>\n"
"</body>\n"
"</html>\n";

static volatile int httpdRunning = 0;

/** Devuelve la posicion del doble CRLF de cabeceras, o -1 si no esta todavia. */
static int httpdHeaderEnd(const char *req, int used)
{
    int i;

    for (i = 0; i + 3 < used; i++)
    {
        if ('\r' == req[i] && '\n' == req[i + 1] &&
            '\r' == req[i + 2] && '\n' == req[i + 3])
        {
            return i + 4;
        }
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
 *
 * El buffer de request es static a proposito: solo hay un httpd (lo garantiza
 * httpdRunning) y 512 B en la pila de una hebra de MINSTK ya no dejan sitio
 * para el write().
 */
thread httpdServer(int dev, int port)
{
    static char req[HTTPD_RQMAX + 1];
    struct netif *iface;
    int tcpdev;
    int n;
    int used;
    int hdrlen;
    int pathlen;

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

        used = 0;
        req[0] = '\0';
        while (used < HTTPD_RQMAX &&
               (n = read(tcpdev, req + used, HTTPD_RQMAX - used)) > 0)
        {
            used += n;
            req[used] = '\0';
            if (httpdHeaderEnd(req, used) >= 0)
            {
                break;
            }
        }

        hdrlen = httpdHeaderEnd(req, used);

        if (0 == strncmp(req, "GET ", 4))
        {
            /* La ruta es lo que va entre "GET " y el primer espacio. */
            pathlen = 0;
            while (pathlen < HTTPD_RQMAX - 4 && used > 4 + pathlen &&
                   ' ' != req[4 + pathlen] && '\t' != req[4 + pathlen] &&
                   '\r' != req[4 + pathlen] && '\n' != req[4 + pathlen])
            {
                pathlen++;
            }

            if ((1 == pathlen && '/' == req[4]) ||
                (11 == pathlen && 0 == strncmp(req + 4, "/index.html", 11)))
            {
                httpdSend(tcpdev, httpd_index,
                          (int)sizeof(httpd_index) - 1,
                          "text/html; charset=utf-8", "200 OK");
            }
            else if (used > 0 && hdrlen >= 0)
            {
                httpdSend(tcpdev, "", 0, "text/plain", "404 Not Found");
            }
        }
        else if (used > 0 && hdrlen >= 0)
        {
            httpdSend(tcpdev, "", 0, "text/plain", "405 Method Not Allowed");
        }

        httpdRelease(tcpdev);
    }

    httpdRunning = 0;
    return SYSERR;
}

/**
 * @ingroup shell
 *
 * Shell command (httpd): arranca el servidor de la pagina de prueba.
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
        printf("\tSirve la pagina de prueba (index embebido) por HTTP.\n");
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
