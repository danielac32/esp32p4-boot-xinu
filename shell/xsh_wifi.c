/**
 * @file     xsh_wifi.c
 *
 * Shell command (wifi): W1 trae la sonda de enumeración SDIO del ESP32-C6
 * sobre el slot 1 del controlador SDMMC compartido; W2.c añade `wifi up`, que
 * levanta el transporte esp_hosted portado verbatim desde
 * /home/daniel/esp32-riscv-bare-metal-sdk-main/drivers/esp_hosted.  Todo lo que
 * se imprime se lee del propio periférico (registros del bloque SDHOST y de la
 * función 1 del slave vía CMD53), no de un caché del driver: eso es lo que
 * convierte el comando en una prueba del bus.  Los estados de red (IP, SSID)
 * llegan en etapas posteriores (W4) y aún no existen.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <shell.h>
#include <timer.h>
#include <sdio.h>
#include <esp_hosted.h>
#include <esp_hosted_sdio.h>

/* Escalón de reintento del ejemplo de referencia
 * (examples/esp_hosted/source/main.c): arranque con el config por defecto,
 * luego con pull-ups internos, y al final 1 bit a 5 MHz. */
#define WIFI_TRIES  3

static void wifi_usage(const char *cmd)
{
    printf("Usage: %s up | down | diag | sdio | fn1\n\n", cmd);
    printf("Description:\n");
    printf("\t'up'    attaches the esp_hosted SDIO transport on SDMMC slot 1\n");
    printf("\t        (ESP32-C6) and reports the registers read from the slave.\n");
    printf("\t'diag'  dumps the shared SDHOST clocks, the DMA pages and the fn1\n");
    printf("\t        registers, all read from the peripheral itself.\n");
    printf("\t'down'  detaches the transport (slot 1 idle again).\n");
    printf("\t'sdio'  enumerates the ESP32-C6 on SDMMC slot 1 and prints the\n");
    printf("\t        SDIO registers read from the device (W1 probe).\n");
    printf("\t'fn1'   brings up function 1 and probes the CMD53/CMD52 data\n");
    printf("\t        path against the esp_hosted slave registers (W2.1).\n");
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
    static esp_hosted_packet_t pkt;   /* 1.6 KB: fuera de la pila del shell */

    esp_hosted_sdio_get_default_config(&cfg);

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

            /* Intento de leer la primera trama que suba el slave.  Si no hay
             * ninguna es lo normal: todavía no hay ninguna RPC en vuelo (eso
             * es el siguiente paso, la version del firmware). */
            for (i = 0; i < 20U; i++)
            {
                esp_hosted_poll();
                if (esp_hosted_rx_pop(&pkt))
                {
                    printf("wifi: trama if_type=%u if_num=%u len=%u seq=%u\n",
                           (unsigned)pkt.info.if_type, (unsigned)pkt.info.if_num,
                           (unsigned)pkt.info.payload_len,
                           (unsigned)pkt.info.seq_num);
                    return OK;
                }
                delay_ms(2);
            }
            printf("wifi: sin tramas del slave (aun no hay RPC en vuelo)\n");
            return OK;
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

    fprintf(stderr, "wifi: el C6 no adjunto\n");
    return SYSERR;
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
 * Shell command (wifi up | down | diag | sdio | fn1).
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

    if (nargs == 2 && strcmp(args[1], "fn1") == 0)
    {
        printf("wifi: sonda del transporte de la funcion 1 del C6 (W2.1)...\n");
        return (sdioFn1TransportProbe() == OK) ? OK : SYSERR;
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
