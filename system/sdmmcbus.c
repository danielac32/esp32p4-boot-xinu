/*
 * sdmmcbus.c — candado del controlador SDMMC (propiedad del KERNEL).
 *
 * La microSD (slot 0, drivers/sd/sd.c) y el ESP32-C6 (slot 1,
 * drivers/sdio/esp_hosted_sdio.c) comparten el MISMO controlador SDMMC: hay
 * que serializar sus transacciones.  El candado es un MONITOR del kernel y
 * vive aquí, en un sitio NEUTRAL (system/), no dentro de ningún device.
 *
 * Lo crea el arranque (sysinit -> sdmmcBusInit) antes de inicializar los
 * devices, así que está disponible para todos.  Las librerías crudas de
 * drivers/ no crean ni esperan monitores: sólo llaman a
 * sdmmcBusLock()/sdmmcBusUnlock() (include/sdmmc.h) alrededor de sus
 * transacciones.  El monitor es reentrante (dueño + profundidad), así que una
 * rutina que ya lo tiene puede volver a pedirlo sin autobloquearse.
 */
#include <kernel.h>
#include <monitor.h>
#include <sdmmc.h>

static monitor sdmmcmon = SYSERR;

/* Idempotente: el arranque lo llama una vez; el resto sólo usa Lock/Unlock. */
void sdmmcBusInit(void)
{
    if (SYSERR == sdmmcmon)
        sdmmcmon = moncreate();
}

void sdmmcBusLock(void)
{
    if (SYSERR != sdmmcmon)
        lock(sdmmcmon);
}

void sdmmcBusUnlock(void)
{
    if (SYSERR != sdmmcmon)
        unlock(sdmmcmon);
}
