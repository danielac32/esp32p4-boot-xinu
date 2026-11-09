/**
 * @file sdio.h
 *
 * Interfaz pública de la sonda SDIO del ESP32-C6 sobre el slot 1 del
 * controlador SDMMC compartido (W1).  El cuerpo está en device/sdio-esp32p4
 * y reutiliza el motor de comandos del host del slot 0 (sdHw.c) bajo el
 * candado del bus.  Esto no es todavía un device del devtab: es la prueba de
 * que el enlace físico y la enumeración SDIO funcionan contra el slave
 * esp_hosted del C6.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SDIODRV_H_
#define _SDIODRV_H_

#include <stddef.h>                     /* uint8/uint16/uint32 + bool         */

/* Por qué falló la última transacción SDIO (tag del host en el nibble alto),
 * reutilizando el mismo registro que el device SD.  Definido en sdHw.c. */
extern uint32 sd_hwerr;

/** Resultado de enumerar el C6: todo lo que sigue se leyó del periférico. */
struct sdioinfo
{
    bool     up;            /* TRUE si el C6 respondió y quedó seleccionado */
    uint32   ocr;           /* R4 del CMD5                                   */
    uint32   rca;           /* dirección relativa asignada por CMD3          */
    uint8    cio_enable;    /* CCCR 0x02 tras habilitar la función 1         */
    uint8    cio_ready;     /* CCCR 0x03 (bit1 = fn1 lista)                  */
    uint8    sdio_ver;      /* CCCR 0x08                                     */
    uint8    fn0_info[8];   /* CCCR 0x0C..0x13: región de identificación      */
    uint16   blksize;       /* CCCR 0x10/0x11: tamaño de bloque de fn0         */
    uint32   clk_khz;       /* reloj de tarjeta del host                     */
};

/** Enumerar el C6 en el slot 1.  Devuelve OK o SYSERR; rellena \p out. */
int sdioEnumerate(struct sdioinfo *out);

/**
 * Igual pero fijando el reloj del slot AL DIVISOR \p card_div inmediatamente
 * despues del reset del C6, dentro de la unica ventana en la que el
 * controlador admite un "update clock": antes de que el esclavo afirme DAT0.
 * \p trace vuelca el estado del bus paso a paso (se apaga en los reintentos).
 */
int sdioEnumerateAt(struct sdioinfo *out, uint8 card_div, bool trace);

/** Imprimir el resultado por la consola. */
void sdioProbeReport(const struct sdioinfo *info);

/**
 * Retener al C6 en reset (CHIP_PU abajo) y salir.  Se usa al final de la sonda
 * del transporte: con el slave retenido sus pads del slot 1 dejan de conducir,
 * y asi se puede comprobar si el DAT0 que el C6 afirma con una interrupcion
 * pendiente es lo que inhibe el camino de datos COMPARTIDO del bloque, es decir
 * si ese es el motivo de que la microSD del slot 0 quede muerta.
 */
void sdioC6HoldReset(void);

/**
 * Diagnostico: averiguar QUIEN sostiene las lineas de datos del C6.
 *
 * Medido en placa: con un CMD53 de lectura en marcha, D0 se lee a 0 y el motor
 * marca EBE (bit de fin erroneo); en 4-bit el fallo es SBE (bit de inicio), o
 * sea D1..D3 no bajan con D0.  Hay dos explicaciones y el nivel solo no las
 * separa: o el C6 conduce esas lineas, o hay otra cosa en la placa que las
 * mantiene.  El C6 se puede sacar de la ecuacion: mientras CHIP_PU esta a
 * bajo sus pads no conducen nada.  Se toma una foto de las patas en reposo,
 * otra con el C6 retenido en reset y otra tras liberarlo.  Lo que cambie al
 * retenerlo es del C6; lo que no cambie, no.
 */
void sdioPadProbe(void);

/**
 * Sonda del transporte de la funcion 1 (W2.1): levanta la funcion 1 reusando
 * la enumeracion W1 y ejercita el camino de datos (CMD53/CMD52) contra los
 * registros del slave esp_hosted, imprimiendo los valores leidos del propio
 * periférico.  Devuelve OK si la funcion 1 respondio.
 */
int sdioFn1TransportProbe(void);

/**
 * Diagnostico dirigido de la fase de datos del CMD53 (`wifi dma`).  Enumera a
 * 400 kHz, apaga la interrupcion SDIO y prueba cuatro transferencias (4 bytes a
 * un registro, la cola con la longitud exacta, la cola redondeada a 4 y una
 * escritura), en 4-bit y en 1-bit, con la traza registro a registro de
 * sdTransferSlot.  Deja el bloque como al arranque (microSD viva).
 */
int sdioDmaDiag(void);

/**
 * Diagnostico en modo PIO (`wifi pio`): la fase de datos del CMD53 moviendo
 * las palabras a mano por el puerto BUFFIFO, con la IDMAC apagada
 * (CTRL.use_internal_dma=0, CTRL.dma_enable=0, BMOD=0).  Sin descriptores, sin
 * DMA y sin paginas de PSRAM en la ecuacion, lo unico que puede impedir que
 * lleguen palabras al FIFO es que el esclavo no conduzca los bits.  Compara
 * CMD52 y PIO sobre los mismos registros (CCCR y TOKEN), lee la cola C2H y
 * escribe el doorbell, en 4-bit y en 1-bit.  Termina restaurando el slot 0.
 */
int sdioPioDiag(void);

/**
 * Barrido de reloj del CMD53 en PIO (`wifi slow`).  Todas las pruebas de la
 * fase de datos se hicieron a 20 MHz, que es la unica velocidad donde pesan los
 * 51K de pull-up del esquematico sobre DAT0..DAT3 y el retardo de la matriz
 * GPIO obligatoria del slot 1.  Aqui se re-enumera a 400 kHz, 800 kHz, 1,67 MHz,
 * 3,3 MHz, 6,7 MHz y 20 MHz y, en cada una, se comparan los MISMOS registros
 * vistos por CMD52 (solo CMD) y por CMD53 en PIO (fase de datos): si a algun
 * divisor bajo coinciden, el fallo era de tiempo de senal, no de alambre ni de
 * protocolo.  Termina restaurando el slot 0.
 */
int sdioPioClkDiag(void);

/**
 * Diagnostico de conduccion del bus (`wifi bus`).  Con la IDMAC y el modo PIO
 * ya descartados (por DAT no pasa ni una palabra), queda separar al C6 de la
 * placa.  Deja cada pad del slot 1 tri-estado (OEN_SEL=1 + GPIO_ENABLE=0) con
 * pull-up interno y luego con pull-down, y lee el pin: una linea flotante
 * cambia entre las dos tomas, una conducida por fuera no.  Ademas valida la
 * palanca antes de usarla: con CHIP_PU abajo pregunta un CCCR por CMD52; si el
 * C6 contesta, GPIO54 no lo resetea y ninguna foto "en reset" separa al C6 de
 * la placa.  Restaura cada pad despues de cada foto y termina dejando el slot 0
 * vivo.
 */
int sdioBusOwnershipDiag(void);

/**
 * Sonda del alambre de datos (`wifi wires`).  Conducir D0 (gpio14) y D1
 * (gpio15) como GPIO push-pull no los mueve nada, mientras D2/D3/CMD/CLK y el
 * control (TP_RST) obedecen: esas dos patas no estan en las redes que creemos.
 * En lugar de suponer numeros, se pregunta por el cable: el esclavo esp_hosted
 * interrumpe en banda (baja DAT0 cuando tiene un paquete C2H y lo suelta al
 * acknowledgear INT_CLR).  Se fotografian los 48 receptores del P4 con la IRQ
 * pendiente, tras el ack y tras tocar el doorbell HOST_TO_SLAVE, y se listan los
 * pads que cambian: el que se mueve con la interrupcion es el DAT0 real.  Solo
 * enciende y apaga el receptor (fun_ie) de cada pad; no toca direcciones, pulls
 * ni salidas, y termina restaurando el slot 0.
 */
int sdioWiresDiag(void);

/**
 * Prueba causal de DAT0 (`wifi dat`).  Con el reloj ya descartado por
 * `wifi slow` (el CMD53 falla identico de 400 kHz a 20 MHz, con palabra de
 * ceros y EBE: firma de DAT0 clavado a 0), aqui se usa la palanca del esclavo
 * para separar culpables: se desarma CCCR_INT_ENABLE y se acknowleddea
 * INT_RAW sobre INT_CLR por CMD52 (solo viajan por CMD), se mira el nivel de
 * D0 con el pad tri-estado, se rearma y se vuelve a mirar; despues se reintenta
 * el CMD53 con el esclavo en reposo y, por ultimo, se conduce D0 y D1 como GPIO
 * push-pull para descartar un cortocircuito de la red a masa.
 */
int sdioDatDiag(void);

/**
 * Busqueda del pad que es DAT0 de verdad (`wifi hunt`).  `wifi dat` dejo dos
 * pads inservibles como salida (14 clavado a 0, 15 clavado a 1) y `wifi wires`
 * vio como el unico pad que obedecia al esclavo era el 15 --el que cae a 0
 * cuando el C6 afirma la interrupcion en banda, que en 1-bit es DAT0--.  La
 * entrada de cada senal del slot 1 es un mux independiente de su salida
 * (FUNC_IN_SEL_CFG[s] elige el pad que alimenta la senal s), asi que se puede
 * dejar la salida DAT0 donde esta y recorrer la entrada pad por pad: el pad cuyo
 * CMD53 de lectura devuelve los mismos bytes que el CMD52 (que va por CMD y si
 * funciona) es el cable de DAT0, sin suponer el mapa de pines de nadie.
 * Termina probando el sentido inverso (ack de INT_RAW por CMD53 de escritura)
 * con el pad ganador tambien como salida, y restaurando el slot 0.
 */
int sdioDatHuntDiag(void);

/**
 * Dueño y congelación de los pads 14/15 (`wifi hold`).  En el ESP32-P4 los pads
 * GPIO0..GPIO15 tienen dos dueños: el IO_MUX de alta potencia (0x500E1000, el que
 * usa este driver) y el IO_MUX de bajo poder (LP_IOMUX 0x5012B000), y el bit
 * MUX_SEL (bit 4) del registro LP decide cuál manda ("1: use LP GPIO, 0: use
 * digital GPIO").  Además cada pad tiene un bit de *hold*: LP_PAD_HOLD_REG
 * (0x5012B04C) para GPIO0..15 y HP_SYSTEM_GPIO_O_HOLD_CTRL0/1 (0x500E5074 /
 * 0x500E5078) para el resto; con el bit puesto la configuración del pad queda
 * congelada, el registro HP se escribe pero el pad no obedece.  Esto es justo lo
 * medido: D0 (14) clavado a 0 y D1 (15) clavado a 1 mientras 16..19 mandan, y el
 * 15 sin embargo sigue la interrupción del esclavo (su entrada sí funciona).
 * La sonda fotografía el dominio LP (reloj y versión, para distinguir "todo en
 * 0" de "bloque inaccesible"), hace la prueba de conducción y la de pull-up /
 * pull-down sobre 14/15 (más el pad libre 22 de contraste), suelta el hold de
 * 14/15 y 16..19 y el MUX_SEL de 14/15, repite ambas pruebas, re-enumerar y
 * vuelve a pedir los 8 bytes del CCCR por CMD53 con la entrada DAT0 en el pad 14
 * y en el 15 contra la verdad del CMD52.  No toca hold ni mux de los pads 0..13
 * (I2C e I2S) ni nada de los dominios de poder, y termina con sdBlockReset() y
 * el slot 0 vivo.
 */
int sdioPadHoldDiag(void);

/**
 * Ventana FIFO del esclavo y modos de CMD53 (`wifi fifo`).  Con los pads 14/15
 * ya bajo el GPIO digital (ver sdioPadHoldDiag) el CMD53 mueve bytes sobre la
 * función 0 y no sobre la función 1, así que esta sonda mide las cinco cosas que
 * deciden el transporte: A) la ventana FIFO leída byte a byte por CMD52 —si sale,
 * el transporte esp_hosted puede ir entero por CMD52—, B) CMD53 byte-mode sobre la
 * ventana, C) CMD53 en modo bloque (bit27, BLKSIZ=512) sobre la ventana, D) CMD53
 * byte-mode sobre un registro de fn1 (control) y E) CMD53 de escritura (sentido
 * H2C), más F) el mismo pedido sobre fn0 como control positivo.  Imprime los bytes
 * que salen de cada intento y el hwerr, reenumerando el C6 y terminando con
 * sdBlockReset() y el slot 0 vivo.
 */
int sdioFifoWindowDiag(void);

/**
 * @brief Devolver el bloque SDHOST compartido al estado con el que Xinu arrancó
 * la microSD: retener el C6, reset de protocolo, divisor global de vuelta a
 * 40 MHz, pads del slot 0, controller-init, identify y lectura real del LBA 0
 * (busca la firma 0x55AA). La usan las sondas y `wifi up` cuando el attach
 * aborta, para no dejar el FAT colgado.
 */
int sdioRestoreSlot0(void);

/**
 * Volcado de diagnóstico del transporte esp_hosted (W2.c), al final de
 * device/sdio-esp32p4/espHostedSdio.c.  Solo lee: reloj padre compartido,
 * CLKDIV/CLKENA/CLKSRC/STATUS del bloque (los dos slots en el mismo
 * registro), las direcciones cached/sin-cache de las paginas que usa su DMA y
 * tres registros de la funcion 1 leidos con CMD53.  Sirve para comprobar que
 * levantar el C6 no movio nada del slot 0.
 */
void esp_hosted_sdio_diag_print(void);

#endif                          /* _SDIODRV_H_ */
