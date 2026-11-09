/**
 * @file mp3.h
 *
 * Decodificador MP3 del kernel, en streaming y a la frecuencia del cable.
 *
 * Por que vive en el kernel: el codigo de una app ELF se ejecuta por la
 * ventana directa de PSRAM (0x88......), sin cache de instrucciones; un
 * decodificador de este tamano ahi iria a un tercio de la velocidad y no
 * llegaria a tiempo real.  En el kernel corre desde flash con su cache, y la
 * app solo se lleva los PCM ya listos.
 *
 * El formato de salida es SIEMPRE el del device de audio: tramas estereo de
 * 16 bits a 48 kHz (dos slots de 16 bits, 4 bytes por trama).  Si el MP3 es
 * de 44,1 kHz se remuestrea aqui, con interpolacion lineal y aritmetica
 * entera; el device y el codec se quedan como estan (48 kHz fijos), que es lo
 * que ya esta medido en placa.
 *
 * Uso tipico desde una app:
 *
 *     if (mp3_abrir("/mus/tema.mp3") != OK) { ... }
 *     while (mp3_leer(buf, 1024) > 0)
 *     {
 *         write(fd, buf, 1024 * 4);
 *     }
 *     mp3_cerrar();
 *
 * Hay UN decodificador: no es reentrante y no se pueden abrir dos ficheros a
 * la vez.  Basta para el reproductor de la placa.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _MP3_H_
#define _MP3_H_

/** Abre un MP3 y lo deja listo.  OK, o SYSERR (no existe, no es MP3, sin RAM). */
int mp3_abrir(const char *ruta);

/** Cierra el fichero y libera el decodificador.  Seguro si no hay nada abierto. */
void mp3_cerrar(void);

/** Frecuencia original del flujo (44100, 48000...).  0 si no hay fichero. */
int mp3_hz(void);

/** Canales originales (1 o 2).  0 si no hay fichero. */
int mp3_canales(void);

/** Bitrate del primer frame, en kbps.  0 si el fichero usa free-format. */
int mp3_kbps(void);

/** Duracion estimada, en ms (tamano del fichero / bitrate).  0 si no se sabe. */
unsigned mp3_ms_total(void);

/** Posicion de reproduccion, en ms, contada a la salida (48 kHz). */
unsigned mp3_ms_actual(void);

/**
 * Decodifica y entrega hasta \p frames tramas estereo a 48 kHz en \p dst
 * (16 bits con signo, entrelazadas L,R).  Devuelve las tramas entregadas;
 * 0 al final del fichero, SYSERR si el flujo no se puede decodificar.
 */
int mp3_leer(short *dst, int frames);

/** Retrocede al principio del flujo (para repetir el tema).  OK/SYSERR. */
int mp3_rebobinar(void);

#endif                          /* _MP3_H_ */
