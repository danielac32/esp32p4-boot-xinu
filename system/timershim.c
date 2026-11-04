/**
 * @file timershim.c
 *
 * Timing shim for the ported ESP32-P4 drivers (see include/timer.h).
 * It maps the SDK's timer API onto the Xinu clock:
 *   millis()           -> clktime/clkticks (el reloj del kernel)
 *   micros()           -> clkcount() / platform.clkfreq
 *   delay_ms()/delay_us()-> mdelay()/udelay()
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <conf.h>

#if RTCLOCK

#include <clock.h>
#include <interrupt.h>
#include <platform.h>
#include <timer.h>

void timer_init(void)
{
    /* The Xinu clock (clkinit/clkhandler) is already running. */
}

/**
 * Milisegundos desde el arranque, sobre el reloj del kernel.
 *
 * Antes se derivaba de clkcount(), pero en RV32 ese contador llega como
 * `ulong`: la palabra baja del mtime de 360 MHz, que envuelve cada
 * 2^32/360e6 = 11,93 s.  Un millis() con diente de sierra de 12 s no sirve
 * para marcas absolutas (reposo de pantalla, LVGL, cualquier dato pintado),
 * y medido en placa era exacto eso: el aviso del demo saltaba en cada vuelta.
 *
 * clktime/clkticks los mantiene clkhandler() a CLKTICKS_PER_SEC (1000 Hz),
 * asi que la resolucion es de 1 ms y el contador es monotono hasta
 * 2^32 ms = 49 dias.  La pareja se lee con las interrupciones enmascaradas:
 * clkintr() resetea clkticks y sube clktime en el mismo handler, y una
 * lectura a caballo entre las dos cosas daria un salto de un segundo.
 */
uint32_t millis(void)
{
    irqmask im;
    ulong secs;
    ulong ticks;
    uint32_t ms;

    im = disable();
    secs = clktime;
    ticks = clkticks;
    restore(im);

    ms = (uint32_t)secs * 1000U + (uint32_t)ticks;

    return ms;
}

/**
 * Microsegundos: aqui si se usa el mtime, porque 1 us no lo da el tick.
 *
 * Ojo: se lee solo la palabra baja, asi que envuelve cada 11,93 s.  Vale para
 * medir intervalos cortos (la resta es correcta por aritmetica modular
 * mientras el intervalo quepa en esa vuelta); para tiempo absoluto, millis().
 */
uint32_t micros(void)
{
    return (uint32_t)(clkcount() / (platform.clkfreq / 1000000UL));
}

void delay_ms(uint32_t ms)
{
    mdelay((ulong) ms);
}

void delay_us(uint32_t us)
{
    udelay((ulong) us);
}

void systick_handler(void)
{
    /* The Xinu clock handler owns the tick; nothing to do here. */
}

#endif                          /* RTCLOCK */
