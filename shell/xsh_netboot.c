/**
 * @file xsh_netboot.c
 *
 * Arranque de red completo: enciende el C6, se asocia a la AP, sube ETH0 por
 * DHCP y lanza los servidores (telnet).  Es la misma secuencia que en
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
#include <device.h>
#include <network.h>
#include <thread.h>

shellcmd xsh_wifi(int nargs, char *args[]);
shellcmd xsh_netup(int nargs, char *args[]);
shellcmd xsh_telnetserver(int nargs, char *args[]);
shellcmd xsh_httpd(int nargs, char *args[]);

/* Margen de arranque del C6, en milisegundos: espera antes de la primera
 * pasada de la tarea y espera entre reintentos. */
#define NETBOOT_SETTLE_MS   10000U
#define NETBOOT_RETRY_MS    10000U
#define NETBOOT_TRIES       5U

/**
 * @ingroup shell
 *
 * `netboot [ssid] [pass]': trae la red y arranca los servidores.
 *
 * Las credenciales van aqui como constantes porque el arranque tiene que
 * funcionar sin que nadie las escriba; si se pasa ssid (y pass) por linea de
 * comandos se usan esos en su lugar, lo que deja probar otra AP sin recompilar.
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
    const char *ssid = "PhMNet_2";
    const char *pass = "TofMagSim47";
    /* Un solo sitio de donde sale la interfaz: `netup' la recibe por nombre y
     * aqui se resuelve con getdev() para el resumen, igual que `telnetserver
     * -d'.  "ETH0" es el nombre en el devtab (system/conf.c), no la macro. */
    const char *ifname = "ETH0";
    char *wifi_up[] = { (char *)"wifi", (char *)"up", NULL };
    char *wifi_join[4];
    char *netup[3];
    char *telnet[] = { (char *)"telnetserver", (char *)"-n", (char *)"1",
                       NULL };
    char *httpd[] = { (char *)"httpd", NULL };
    struct netif *iface;
    int descrp;

    if (nargs > 1 && strcmp(args[1], "--help") == 0)
    {
        printf("Usage: netboot [ssid] [pass]\n\n");
        printf("Description:\n");
        printf("\tEnciende el C6, se asocia a la AP, sube ETH0 por DHCP y\n");
        printf("\tlanza los servidores.  Sin argumentos usa la AP grabada.\n");
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

    /* `wifi join <ssid> <pass>': el propio join reintenta tres veces, porque
     * medido en placa el primer Req_WifiConnect suele morir con reason=39
     * (STA desconectada por el C6) y el segundo intento asocia. */
    wifi_join[0] = (char *)"wifi";
    wifi_join[1] = (char *)"join";
    wifi_join[2] = (char *)ssid;
    wifi_join[3] = (char *)pass;

    netup[0] = (char *)"netup";
    netup[1] = (char *)ifname;
    netup[2] = NULL;

    printf("netboot: encendiendo el C6...\n");
    if (SYSERR == xsh_wifi(2, wifi_up))
    {
        fprintf(stderr, "netboot: fallo el attach SDIO del C6\n");
        return SYSERR;
    }

    printf("netboot: asociandose a \"%s\"...\n", ssid);
    if (SYSERR == xsh_wifi(4, wifi_join))
    {
        fprintf(stderr, "netboot: no hubo asociacion con \"%s\"\n", ssid);
        return SYSERR;
    }

    /* Ojo al convenio: xsh_wifi() devuelve OK/SYSERR (1/-1) pero xsh_netup() y
     * xsh_telnetserver() usan SHELL_OK/SHELL_ERROR (0/1); son los dos estilos
     * que conviven en la shell de Xinu y mirar el equivocado daria por bueno un
     * paso que fallo. */
    printf("netboot: subiendo %s por DHCP...\n", ifname);
    if (SHELL_OK != xsh_netup(2, netup))
    {
        fprintf(stderr, "netboot: netup %s fallo\n", ifname);
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
