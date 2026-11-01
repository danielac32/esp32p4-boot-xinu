#include <stddef.h>
#include <stdio.h>
#include <kernel.h>
#include <device.h>
#include <board.h>
#include <lcd.h>

#include "../device/lcd-esp32p4/lcdhw.h"

/* Implementadas en lcd_cube.c */
extern void lcdCubeStart(void);
extern void lcdCubeFrame(float dt_seconds);

/* Implementadas en lcd_plasma.c */
extern void lcdPlasmaStart(void);
extern void lcdPlasmaFrame(void);


//extern void lcdStellarStart(void);
//extern void lcdStellarFrame(void);


/*
 * Unidades de pila: las mismas que recibe create()
 * en tu implementación de Xinu.
 */
#define DEMO_STACK       (INITSTK * 4)
#define DEMO_PRIORITY    INITPRIO
#define CUBE_FRAMES      100
#define FRAME_DELAY_MS   15

/*
 * Impide lanzar otra animación mientras la anterior
 * sigue utilizando la pantalla.
 *
 * Se usa desde el shell; no es un bloqueo general
 * para múltiples shells simultáneos.
 */
 
/*
 * Tarea del plasma.
 * No retorna.
 */
/*
static void stellar(void)
{
 
    lcdStellarStart();
    printf("estoy aqui en estelar \n");
    for (unsigned i = 0; i < CUBE_FRAMES; ++i) {
        //lcdStellarFrame();
       // sleepms(40);
    }
}*/

static void plasmaTask(void)
{
    printf("LCD: iniciando plasma\r\n");

    lcdPlasmaStart();

    for (unsigned i = 0; i < CUBE_FRAMES; ++i) {
        lcdPlasmaFrame();

        //sleep(FRAME_DELAY_MS);
    }
    
}

/*
 * Tarea del cubo.
 * Al terminar su demostración, crea la del plasma.
 */
static void cubeTask(void)
{
    printf("LCD: iniciando cubo 3D\r\n");

    lcdCubeStart();

    for (unsigned i = 0; i < CUBE_FRAMES; ++i) {
        lcdCubeFrame(0.040f);
        //sleep(FRAME_DELAY_MS);
    }
    ready(create(plasmaTask, INITSTK, INITPRIO, "lcdplasma", 0), RESCHED_NO);
}

/*
 * Comando del shell.
 * Crea la tarea y devuelve el control al shell.
 */
shellcmd xsh_test(int nargs, char *args[])
{
    (void)nargs;
    (void)args;

    
   ready(create(cubeTask, INITSTK, INITPRIO, "lcdcube", 0), RESCHED_NO);
   
  // ready(create(stellar, INITSTK, INITPRIO, "lcdstellar", 0), RESCHED_NO);

    return OK;
}
