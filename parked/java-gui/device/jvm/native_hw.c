#include <device.h>
#include <conf.h>
/*
 * native_hw.c — la "librería" esp32p4.HW implementada para el ESP32-S3.
 *
 * (La clase Java sigue llamándose "esp32p4.HW" para que las apps .class del
 * SDK del P4 corran sin recompilar; sólo cambia el cuerpo nativo.)
 *
 * Solo hardware: tiempo (delay/millis), gpio y sonido.  La consola/texto
 * vive en native_io.c y los ficheros en native_fat.c.  En el host lo
 * implementa jvm/esp32p4_hw.c.
 *
 * Convención de pila (la misma que el host): los argumentos se empujaron de
 * izquierda a derecha, así que el ÚLTIMO está en la cima; el valor de
 * retorno se empuja de vuelta.
 *
 * gpio/sound quedan como traza (printf) a la espera de fijar pines/canal.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <clock.h>
#include <memory.h>                    /* memlist / psramlist / memheap */
#include <platform.h>                  /* platform.maxaddr, .clkfreq */
#include <version.h>                   /* VERSION: la cadena del build */
#include <power.h>                      /* POWER_CTRL_* y power_info_t */

#include "simple_jvm.h"
#include "java_lib.h"

/* ---- Math.random --------------------------------------------------- */

/* double en [0,1).  El RNG de Xinu (rand) sembrado con el reloj. */
int java_lang_math_random(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    srand((unsigned int) clkcount());
    pushDouble(stack, (double) rand() / (double) RAND_MAX);
    return 0;
}

/* ---- tiempo -------------------------------------------------------- */

int esp32p4_hw_delay(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int ms = popInt(stack);

    (void) p;
    (void) type;
    if (ms > 0)
    {
        sleep((uint) ms);
    }
    return 0;
}

int esp32p4_hw_millis(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    /* Milisegundos desde el arranque.  En este puerto CLKTICKS_PER_SEC es
     * 1000, así que clktime son segundos y clkticks las marcas dentro del
     * segundo (una marca = un milisegundo). */
    pushInt(stack, (int)(clktime * CLKTICKS_PER_SEC + clkticks));
    return 0;
}

/* ---- gpio / sonido (traza) ---------------------------------------- */

int esp32p4_hw_gpioWrite(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int val = popInt(stack);
    int pin = popInt(stack);

    (void) p;
    (void) type;
    printf("[native] gpioWrite(pin=%d, val=%d)\n", pin, val);
    return 0;
}

int esp32p4_hw_gpioRead(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int pin = popInt(stack);

    (void) p;
    (void) type;
    printf("[native] gpioRead(pin=%d) -> 0\n", pin);
    pushInt(stack, 0);
    return 0;
}

int esp32p4_hw_sound(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int ms = popInt(stack);
    int freq = popInt(stack);

    (void) p;
    (void) type;
    printf("[native] sound(freq=%d, ms=%d)\n", freq, ms);
    return 0;
}


/* ---- esp32p4.RGB: sin device en el P4 ------------------------------- */

/* El S3 tiene un NeoPixel en RGB0; la tarjeta del P4 no tiene ningún LED
 * RGB adressable, así que estos tres natives se quedan como traza.  No los
 * borro de la tabla porque las demos .class del S3 (RgbDemo, LedBlink)
 * enlazan contra esos nombres y sirven de regresión del intérprete. */

int esp32p4_rgb_set(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int b = popInt(stack);
    int g = popInt(stack);
    int r = popInt(stack);

    (void) p;
    (void) type;
    printf("[native] rgbSet(%d,%d,%d): sin LED RGB en el P4\r\n", r, g, b);
    return 0;
}

int esp32p4_rgb_off(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) stack;
    (void) p;
    (void) type;
    printf("[native] rgbOff(): sin LED RGB en el P4\r\n");
    return 0;
}

int esp32p4_rgb_get(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, 0);
    return 0;
}

/* ---- esp32p4.Power: el PMIC de solo lectura -------------------------- */

/*
 * El AXP2101 manda la rail de 3V3, asi que la clase Power es deliberadamente
 * de solo leer: percent() y voltage() salen por /dev/power0, y no hay ningun
 * metodo que escriba un registro (0x10/0x80 pueden apagar o resetear el SoC).
 *
 * El acceso va por el device, no por el driver crudo: el device `i2c' es quien
 * serializa el bus I2C0, y en el bus estan el PMIC y el tactile FT6336 que lee
 * la hebra de UI.  Desde la hebra java hay que respetar esa espera.
 */

/** Estado bruto del registro 0x01[2:0] -> 0 descargando, 1 lleno, 2 cargando. */
static int info2charge(int c)
{
    switch (c)
    {
    case POWER_CHG_NONE:
        return 0;
    case POWER_CHG_DONE:
        return 1;
    case POWER_CHG_TRICKLE:
    case POWER_CHG_FAST:
    case POWER_CHG_ABSORB:
        return 2;
    default:
        return 0;
    }
}

int esp32p4_power_percent(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int fd, v;

    (void) p;
    (void) type;

    fd = getdev("/dev/power0");
    if (SYSERR == fd)
    {
        pushInt(stack, -1);
        return 0;
    }
    v = control(fd, POWER_CTRL_PERCENT, 0, 0);
    close(fd);
    pushInt(stack, (SYSERR == v) ? -1 : v);
    return 0;
}

int esp32p4_power_voltage(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    power_info_t info;
    int fd, rc;

    (void) p;
    (void) type;

    fd = getdev("/dev/power0");
    if (SYSERR == fd)
    {
        pushInt(stack, -1);
        return 0;
    }
    rc = control(fd, POWER_CTRL_INFO, (long)&info, 0);
    close(fd);
    pushInt(stack, (OK == rc) ? (int) info.vbat_mv : -1);
    return 0;
}

/* charge(): 0 descargando, 1 en corriente, 2 cargando, -1 sin PMIC.  Es el
 * estado bruto del registrador 0x01[2:0] traducido a algo que una barra de UI
 * pueda pintar sin conocer el datasheet. */
int esp32p4_power_charge(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    power_info_t info;
    int fd, rc;

    (void) p;
    (void) type;

    fd = getdev("/dev/power0");
    if (SYSERR == fd)
    {
        pushInt(stack, -1);
        return 0;
    }
    rc = control(fd, POWER_CTRL_INFO, (long)&info, 0);
    close(fd);
    if (OK != rc)
    {
        pushInt(stack, -1);
        return 0;
    }
    pushInt(stack, info2charge(info.charge));
    return 0;
}

/* ---- memoria de los dos montones y datos del build -------------------- */

/*
 * El P4 tiene dos listas de libres: L2MEM, el SRAM interno de donde salen las
 * pilas de las hebras (la de esta misma app) y el heap del kernel, y PSRAM, de
 * donde salen el estado de la VM y los pixmaps.  Una app que solo vea "libre"
 * de uno no entiende por que la galeria no puede abrir una foto: se pedian
 * 370 KB contiguos y el monton tenia libres repartidos.
 *
 * Los totales salen de los extremos del monton (los mismos que usa `mem' en la
 * shell) y lo libre de la cabecera de su lista, que el allocator mantiene
 * actualizada.  El bloque mayor se tiene que recorrer: se corta a los 512
 * nodos y se validan las direcciones contra el rango del monton, igual que
 * psram_free_bytes(), porque estas listas las mueve otra hebra (la de LVGL
 * reserva pixmaps mientras esta app pinta).
 */
#define HEAP_WALK_MAX 512

static uintptr_t heapMaxBlock(struct memblock *head, uintptr_t lo,
                              uintptr_t hi)
{
    struct memblock *mb;
    uintptr_t largest = 0;
    int n = 0;

    for (mb = head->next; mb != NULL; mb = mb->next)
    {
        if (n++ >= HEAP_WALK_MAX || (uintptr_t)mb < lo || (uintptr_t)mb >= hi)
        {
            break;
        }
        if (mb->length > largest)
        {
            largest = mb->length;
        }
    }
    return largest;
}

static uintptr_t l2Lo(void)
{
    return (uintptr_t)memheap;
}

static uintptr_t l2Hi(void)
{
    return (uintptr_t)platform.maxaddr;
}

int esp32p4_hw_memTotal(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)(l2Hi() - l2Lo()));
    return 0;
}

int esp32p4_hw_memFree(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)memlist.length);
    return 0;
}

int esp32p4_hw_memMaxBlock(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)heapMaxBlock(&memlist, l2Lo(), l2Hi()));
    return 0;
}

int esp32p4_hw_psramTotal(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)((uintptr_t)psmaxheap - (uintptr_t)psminheap));
    return 0;
}

int esp32p4_hw_psramFree(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)psramlist.length);
    return 0;
}

int esp32p4_hw_psramMaxBlock(StackFrame *stack, SimpleConstantPool *p,
                             char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)heapMaxBlock(&psramlist, (uintptr_t)psminheap,
                                     (uintptr_t)psmaxheap));
    return 0;
}

/* version(): la cadena del build, con el contador #NNNN que identifica la
 * imagen que esta corriendo.  Sin ella, una app no puede decir que kernel ve
 * el usuario desde el panel. */
int esp32p4_hw_version(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushObj(stack, heap_new_string(VERSION));
    return 0;
}

/* clkFreq(): la frecuencia del reloj del kernel (platform.clkfreq), en Hz.
 * Ojo: es la del timer, no la del nucleo; el nucleo va a 320 MHz medidos
 * (syscheck) y eso no lo publica ninguna variable del kernel. */
int esp32p4_hw_clkFreq(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    (void) p;
    (void) type;
    pushInt(stack, (int)platform.clkfreq);
    return 0;
}

