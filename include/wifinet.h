/**
 * @file wifinet.h
 *
 * Lo que una app ELF (o el arranque) necesita del WiFi: listar las redes que
 * ve el C6 y asociarse a una.  Es la parte comun entre la app `settings` y
 * `netBootTask`, para que las dos hagan exactamente lo mismo.
 *
 * La implementacion reutiliza los comandos de shell que ya existian
 * (`wifi up/join` y `netup`): aqui no hay una segunda copia del protocolo RPC
 * ni del DHCP, solo un envoltorio con los argumentos ya montados.
 */
#ifndef _WIFINET_H_
#define _WIFINET_H_

/** Topes de /wifi.txt: 32 de SSID y 64 de clave, como el estandar 802.11. */
#define WIFI_RED_SSID      33
#define WIFI_RED_CLAVE     65

/** Redes que se leen de /wifi.txt y se prueban al arrancar. */
#define WIFI_REDES_MAX     8

/** Una red del escaneo.  rssi solo vale si hay_rssi. */
struct wifi_red
{
    char ssid[WIFI_RED_SSID];
    int canal;
    int auth;
    int rssi;
    int hay_rssi;
};

/**
 * Enciende el C6 si aun no esta adjunto.  Idempotente: si ya lo estaba, no
 * toca el bus.  Devuelve OK o SYSERR.
 */
int wifiAsegurar(void);

/**
 * Rellena hasta `max` redes visibles y devuelve cuantas encontro (0 si
 * ninguna), o SYSERR si el escaneo no se pudo hacer.  Repite el mismo
 * procedimiento que `wifi scan`: 286 arranca el barrido, 288 da el recuento y
 * 289 entrega los registros.  No filtra redes ocultas ni ordena.
 */
int wifiScanFill(struct wifi_red *out, int max);

/**
 * Asegura el C6, asocia la STA a `ssid` con `clave` y sube ETH0 por DHCP.
 * Devuelve OK solo si las tres cosas salieron; SYSERR en cualquier otra.
 * Es la misma secuencia que `netboot` y la que usa la app de ajustes.
 */
int wifiRedConectar(const char *ssid, const char *clave);

#endif /* _WIFINET_H_ */
