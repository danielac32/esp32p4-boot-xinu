/**
 * @file xsh_netboot.c
 *
 * Arranque de red completo: enciende el C6, se asocia a la AP, sube ETH0 por
 * DHCP y lanza los servidores (telnet, http y el de comandos por UDP).  Es la misma secuencia que en
 * xinu-s3-amp hace system/main.c llamando a los comandos de la shell, solo que
 * aqui el primer paso no existe: el S3 trae el Ethernet en el propio chip, y
 * en el P4 la WiFi vive detras del esp_hosted (RPC por SDIO) y hay que
 * encenderla y asociarla antes de poder subir la interfaz.
 *
 * Se expone como comando `netboot' y como tarea netaBootTask() para que main.c
 * la arranque al boot; el comando permite volver a intentarlo a mano si la AP
 * todavia no estaba despierta.
 */
/* Embedded Xinu, ported to ESP32-P4. */

#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wifinet.h>
#include <device.h>
#include <esp_hosted_sdio.h>
#include <fat_filelib.h>
#include <fsutil.h>
#include <network.h>
#include <shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>

shellcmd xsh_wifi(int nargs, char *args[]);
shellcmd xsh_netup(int nargs, char *args[]);
shellcmd xsh_telnetserver(int nargs, char *args[]);
shellcmd xsh_httpd(int nargs, char *args[]);
shellcmd xsh_udpserver(int nargs, char *args[]);

/* Margen de arranque del C6, en milisegundos: espera antes de la primera
 * pasada de la tarea y espera entre reintentos. */
#define NETBOOT_SETTLE_MS   10000U
#define NETBOOT_RETRY_MS    10000U
#define NETBOOT_TRIES       5U

/* Redes guardadas por la app de ajustes: una por linea, "ssid;clave". */
#define NETBOOT_FICHERO     "/wifi.txt"

struct red_guardada
{
    char ssid[WIFI_RED_SSID];
    char clave[WIFI_RED_CLAVE];
};

/**
 * Enciende el C6 si aun no esta adjunto.  El attach se probo en placa y el
 * comando `wifi up' ya lleva sus tres configuraciones y su semaforo, asi que
 * aqui solo se evita repetirlo cuando ya hay enlace.
 */
int wifiAsegurar(void)
{
    char *up[] = { (char *)"wifi", (char *)"up", NULL };

    if (esp_hosted_sdio_is_attached())
    {
        return OK;
    }
    return xsh_wifi(2, up);
}

/**
 * Asegura el C6, asocia la STA y sube ETH0 por DHCP.  Es la secuencia que ya
 * hacia `netboot', extraida para que la app de ajustes no tenga que repetirla
 * (y para que las dos vayan siempre por el mismo camino).
 */
int wifiRedConectar(const char *ssid, const char *clave)
{
    char *join[5];
    char *netup[3];
    char *netdown[3];

    if (OK != wifiAsegurar())
    {
        return SYSERR;
    }

    join[0] = (char *)"wifi";
    join[1] = (char *)"join";
    join[2] = (char *)ssid;
    join[3] = (char *)clave;
    join[4] = NULL;
    if (OK != xsh_wifi(4, join))
    {
        return SYSERR;
    }

    /* Si ETH0 ya estaba arriba (la placa ya tenia red al entrar a la app),
     * `netup' se niega.  Se baja antes para renovar la direccion; si no estaba
     * arriba, `netdown' devuelve error y se ignora. */
    netdown[0] = (char *)"netdown";
    netdown[1] = (char *)"ETH0";
    netdown[2] = NULL;
    (void)xsh_netdown(2, netdown);

    /* netup usa SHELL_OK/SHELL_ERROR, no OK/SYSERR: son los dos convenios que
     * conviven en la shell y mirar el equivocado daria por buena una interfaz
     * que no subio. */
    netup[0] = (char *)"netup";
    netup[1] = (char *)"ETH0";
    netup[2] = NULL;
    if (SHELL_OK != xsh_netup(2, netup))
    {
        return SYSERR;
    }

    return OK;
}

/**
 * Lee las redes de /wifi.txt.  Devuelve cuantas lineas validas hay (0 si no
 * existe el fichero o la tarjeta no esta montada).  Se tolera el CR final
 * porque el fichero se edita en el PC.
 */
static int netboot_leer_redes(struct red_guardada *out, int max)
{
    char linea[WIFI_RED_SSID + WIFI_RED_CLAVE + 4];
    void *f;
    int n = 0;

    if (!fs_mounted())
    {
        return 0;
    }

    f = fl_fopen(NETBOOT_FICHERO, "r");
    if (NULL == f)
    {
        return 0;
    }

    while (n < max && NULL != fl_fgets(linea, sizeof(linea), f))
    {
        char *sep;
        size_t len;

        len = strlen(linea);
        while (len > 0 && ('\n' == linea[len - 1] || '\r' == linea[len - 1]))
        {
            linea[--len] = '\0';
        }
        if (0 == len || '#' == linea[0])
        {
            continue;
        }

        sep = strchr(linea, ';');
        if (NULL == sep || sep == linea)
        {
            continue;               /* sin separador o sin SSID */
        }
        *sep = '\0';

        strncpy(out[n].ssid, linea, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        strncpy(out[n].clave, sep + 1, sizeof(out[n].clave) - 1);
        out[n].clave[sizeof(out[n].clave) - 1] = '\0';
        n++;
    }

    fl_fclose(f);
    return n;
}

/**
 * @ingroup shell
 *
 * `netboot [ssid] [pass]': trae la red y arranca los servidores.
 *
 * Las credenciales por defecto siguen grabadas aqui porque el arranque tiene
 * que funcionar sin que nadie las escriba, pero ahora se mira primero
 * /wifi.txt (lo escribe la app de ajustes): se prueban sus redes en orden y
 * solo si ninguna asocia se cae a la AP grabada.  Pasar ssid y pass por linea
 * de comandos ignora el fichero, util para probar otra AP sin recompilar.
 *
 * Cada paso es un comando de la shell, invocado como hace main.c en el puerto
 * del S3: asi el output es exactamente el que se veria tecleando, y un paso que
 * falla corta la secuencia en vez de seguir con una interfaz muerta.
 *
 * @param nargs numero de argumentos
 * @param args  vector de argumentos
 * @return OK si la red quedo arriba; SYSERR en cualquier otro caso
 */
shellcmd xsh_netboot(int nargs, char *args[])
{
    const char *ssid = "wifi2.4Ghz";
    const char *pass = "ac32mqn422308";
    /* Un solo sitio de donde sale la interfaz: `netup' la recibe por nombre y
     * aqui se resuelve con getdev() para el resumen, igual que `telnetserver
     * -d'.  "ETH0" es el nombre en el devtab (system/conf.c), no la macro. */
    const char *ifname = "ETH0";
    struct red_guardada redes[WIFI_REDES_MAX];
    char *telnet[] = { (char *)"telnetserver", (char *)"-n", (char *)"1",
                       NULL };
    char *httpd[] = { (char *)"httpd", NULL };
    char *udpserver[] = { (char *)"udpserver", NULL };
    struct netif *iface;
    int descrp;
    int nredes = 0;
    int i;
    int ok;

    if (nargs > 1 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: netboot [ssid] [pass]\n\n");
        printf("Description:\n");
        printf("\tEnciende el C6, se asocia a la AP, sube ETH0 por DHCP y\n");
        printf("\tlanza los servidores.  Sin argumentos prueba las redes de\n");
        printf("\t%s y, si no hay ninguna, la AP grabada.\n", NETBOOT_FICHERO);
        return OK;
    }

    if (nargs >= 2)
    {
        ssid = args[1];
    }
    if (nargs >= 3)
    {
        pass = args[2];
    }

    /* Sin argumentos, la lista sale de /wifi.txt (la escribe la app de
     * ajustes).  Se lee ANTES de encender el C6: la microSD y el SDIO del C6
     * comparten el bloque SDMMC y asi el arranque no mezcla las dos cosas.
     * Con `netboot ssid pass' el fichero se ignora, que es lo util para probar
     * otra AP a mano. */
    if (nargs == 1)
    {
        nredes = netboot_leer_redes(redes, WIFI_REDES_MAX);
        if (nredes > 0)
        {
            printf("netboot: %d red(es) en %s\n", nredes, NETBOOT_FICHERO);
        }
    }

    printf("netboot: encendiendo el C6...\n");
    if (OK != wifiAsegurar())
    {
        fprintf(stderr, "netboot: fallo el attach SDIO del C6\n");
        return SYSERR;
    }

    /* En orden de prioridad: la ultima que guardo la app va la primera.  Cada
     * `wifi join' ya reintenta tres veces por dentro (medido en placa: la AP
     * de casa rechaza a veces con motivo 39 y asocia en el intento siguiente). */
    ok = 0;
    for (i = 0; i < nredes && !ok; i++)
    {
        printf("netboot: probando \"%s\" (%d/%d)...\n",
               redes[i].ssid, i + 1, nredes);
        ok = (OK == wifiRedConectar(redes[i].ssid, redes[i].clave));
        if (!ok)
        {
            printf("netboot: \"%s\" no respondio; siguiente\n", redes[i].ssid);
        }
    }

    /* Ultimo recurso: la AP grabada en el codigo.  Mantiene el arranque que ya
     * funcionaba si el fichero no existe o se quedo con claves viejas. */
    if (!ok)
    {
        printf("netboot: probando la AP grabada \"%s\"...\n", ssid);
        ok = (OK == wifiRedConectar(ssid, pass));
    }

    if (!ok)
    {
        fprintf(stderr, "netboot: no hubo asociacion con ninguna red\n");
        return SYSERR;
    }

    printf("netboot: lanzando los servidores...\n");
    /* nargs=3, no 4: el NULL del vector no es un argumento.  Contarlo de mas
     * hacia que xsh_telnetserver() llegara a strcmp(NULL, "-d") y el kernel
     * moria con un load access fault (medido en placa). */
    if (SHELL_OK != xsh_telnetserver(3, telnet))
    {
        fprintf(stderr, "netboot: no se pudo lanzar el servidor telnet\n");
        return SYSERR;
    }

    if (SHELL_OK != xsh_httpd(1, httpd))
    {
        fprintf(stderr, "netboot: no se pudo lanzar el servidor http\n");
        return SYSERR;
    }

    /* El servidor UDP va aparte y un fallo suyo NO corta el arranque: se
     * lanzan tres hebras y solo dos dispositivos UDP (NUDP=2), y DHCP o `dns'
     * aun podrian tener uno.  Sin udpserver no hay subida de firmware, pero la
     * consola y el http siguen valiendo. */
    if (SHELL_OK != xsh_udpserver(1, udpserver))
    {
        fprintf(stderr, "netboot: sin servidor UDP (se puede lanzar a mano "
                        "con `udpserver')\n");
    }

    descrp = getdev(ifname);
    if (isbaddev(descrp))
    {
        fprintf(stderr, "netboot: no hay device \"%s\" en el conf\n", ifname);
        return SYSERR;
    }

    iface = netLookup(descrp);
    if (iface != NULL)
    {
        char a[32], b[32], c[32];

        netaddrsprintf(a, &iface->ip);
        netaddrsprintf(b, &iface->mask);
        netaddrsprintf(c, &iface->gateway);
        printf("netboot: %s es %s mascara %s puerta %s\n",
               ifname, a, b, c);
    }
    printf("netboot: telnet en el puerto 23, http en el 80\n");

    return OK;
}

/**
 * @ingroup shell
 *
 * Tarea de arranque: main.c la crea para que la red quede arriba sin tocar
 * nada el usuario.  Usa las credenciales por defecto de xsh_netboot().
 *
 * Lleva espera y reintentos porque medido en placa el attach SDIO del C6 no
 * sale a la primera si se lanza dos segundos despues del reset del P4: los 3
 * intentos internos del driver fallan ("host init failed"), y el mismo `wifi
 * up' lanzado unos segundos despues si enumera.  El modulo necesita ese margen
 * de arranque, asi que aqui se duerme antes de la primera pasada y se repite
 * la secuencia completa.  Todo con sleep(), que cede la CPU; delay_ms() es
 * mdelay() y se queda girando.
 *
 * @return OK si la red quedo arriba; SYSERR si se agotaron los intentos
 */
int netBootTask(void)
{
    char *argv[] = { (char *)"netboot", NULL };
    uint try;

    sleep(NETBOOT_SETTLE_MS);

    for (try = 1U; try <= NETBOOT_TRIES; try++)
    {
        if (try > 1U)
        {
            printf("netboot: intento %u de %u en %u s\n",
                   try, NETBOOT_TRIES, NETBOOT_RETRY_MS / 1000U);
        }

        if (OK == xsh_netboot(1, argv))
        {
            return OK;
        }

        sleep(NETBOOT_RETRY_MS);
    }

    fprintf(stderr, "netboot: la red no quedo arriba; usa `netboot' a mano\n");
    return SYSERR;
}
