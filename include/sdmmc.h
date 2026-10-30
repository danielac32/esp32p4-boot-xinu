/*
 * sdmmc.h — candado del CONTROLADOR SDMMC compartido (microSD + ESP32-C6).
 *
 * La microSD (slot 0, drivers/sd/sd.c) y el C6 (slot 1,
 * drivers/sdio/esp_hosted_sdio.c) usan el MISMO controlador SDMMC: sus
 * transacciones hay que serializarlas.
 *
 * El candado es un MONITOR del kernel y lo POSEE el kernel en un sitio
 * neutral: system/sdmmcbus.c, creado en el arranque (sysinit) antes de
 * inicializar los devices.  Las librerías crudas de drivers/ no crean
 * semáforos/monitores: sólo llaman a sdmmcBusLock()/sdmmcBusUnlock()
 * alrededor de cada transacción.  El monitor es reentrante (dueño +
 * profundidad), así que una rutina que ya lo tiene puede volver a pedirlo.
 *
 * Antes de sdmmcBusInit() (arranque, monohilo) el candado es no-op: no hay
 * nada que serializar.
 */
#ifndef _SDMMC_H_
#define _SDMMC_H_

/* Crea el monitor del controlador (idempotente). */
void sdmmcBusInit(void);

/* Toma/suelta el candado del controlador (no-op si aún no existe). */
void sdmmcBusLock(void);
void sdmmcBusUnlock(void);

#endif                          /* _SDMMC_H_ */
