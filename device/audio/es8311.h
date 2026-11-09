/**
 * @file es8311.h
 *
 * Driver crudo del codec ES8311 (direccion 0x18 en el bus I2C0 compartido).
 * Solo la parte de control por I2C: arranque, reloj, formato, volumen, mute y
 * ganancia del micro.  El flujo PCM lo monta la capa de al lado (i2s.c +
 * ahbdma.c + audioOut.c); aqui se manda al codec, no se mueve ni una muestra.
 *
 * Las secuencias de registros estan tomadas del driver de referencia del SDK
 * (drivers/es8311/es8311.c), que a su vez viene del componente ES8311 de
 * esp-bsp de Espressif.  Nada de aqui esta inventado.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _ES8311DRV_H_
#define _ES8311DRV_H_

#include <stdbool.h>
#include <stdint.h>

/* Ajuste con el que se trae el codec: 48 kHz, 16 bits, MCLK de 12,288 MHz
 * (256 x fs) entrando por el pin MCK.  Es la configuracion que usara la
 * etapa I2S, asi que E1 ya la deja escrita y verificable. */
#define ES8311_MCLK_HZ        12288000U
#define ES8311_SAMPLE_RATE_HZ 48000U

/* Pasos de ganancia escala del ADC (registro 0x16): 0 = 0 dB ... 7 = 42 dB,
 * siempre de 6 en 6.  Es ganancia digital detras del ADC. */
#define ES8311_MIC_GAIN_MAX   7U

/* Pasos del PGA analogico (registro 0x14, bits 3:0): 0 = 0 dB ... 10 = +30 dB,
 * de 3 en 3.  Es lo que amplifica la capsula antes de convertir, asi que es el
 * mando que cambia lo que capta el micro; el valor de la referencia es el
 * maximo. */
#define ES8311_MIC_PGA_MAX    10U

/* El volumen del DAC (registro 0x32) es lineal en decibelios, no en amplitud:
 * 191 escalones hacia arriba o hacia abajo valen medio decibelio, y el 0 dB
 * cae justo en el registro 191 (0xBF del datasheet), con 0 = -95,5 dB y
 * 255 = +32 dB.  Se trabaja en decimas de dB para no meter coma flotante en el
 * kernel. */
#define ES8311_VOL_0DB_REG    191
#define ES8311_VOL_STEP_DB10  5
#define ES8311_VOL_MUTE_DB10  (-955)

/* Reset, config de reloj/formato y sistema.  False si el codec no responde. */
bool es8311_init(void);

bool es8311_present(void);

/* Volumen de salida 0..100.  Devuelve el valor leido de vuelta del codec, o
 * SYSERR si alguna transaccion falla: leer de vuelta es lo que demuestra que
 * el bus y el chip estan vivos. */
int es8311_set_volume(uint32_t percent);

/* Volumen actual tal y como lo tiene el codec (no un cache del driver). */
int es8311_get_volume(void);

/* El mismo escalon convertido a decimas de dB, leido del registro: es la cifra
 * que dice lo que suena, porque el porcentaje es lineal sobre un registro que
 * es lineal en decibelios. */
int es8311_get_volume_db(void);

bool es8311_set_mute(bool mute);

/* Estado del mute leido del codec: 1 muteado, 0 activo, SYSERR si no hay
 * respuesta. */
int es8311_get_mute(void);

/* Escala digital del ADC (registro 0x16): 0..7 = 0..+42 dB en pasos de 6.  Es
 * el mando de nivel de la captura; set guarda el valor y start_mic lo vuelve a
 * escribir, y get lee el registro del codec en lugar del valor pedido. */
bool es8311_set_mic_gain(uint32_t gain);
int es8311_get_mic_scale(void);

/* Ruta de captura: PGA encendido, la pareja MIC1P/MIC1N seleccionada como
 * entrada diferencial y el volumen digital del ADC subido.  Es lo que hace que
 * por el pad de datos salgan muestras en vez de ceros.  stop_mic hace lo
 * contrario: deja el ADC en silencio (-95,5 dB) y apaga el PGA. */
bool es8311_start_mic(void);
bool es8311_stop_mic(void);
bool es8311_mic_running(void);

/* Ganancia analogica del PGA: 0..10 = 0..+30 dB en pasos de 3 (0x14[3:0]).
 * get lee el registro de vuelta del codec, no un cache. */
bool es8311_set_mic_pga(uint32_t gain);
int es8311_get_mic_pga(void);

/* Volumen digital del ADC (0x17), leido del registro: 0 = -95,5 dB (silencio),
 * 191 = 0 dB, 200 = +4,5 dB. */
int es8311_get_mic_volume(void);

/* Estado de la ruta leido del codec: bit0 = PGA encendido, bit1 = entrada
 * diferencial seleccionada, bit2 = ADC con volumen.  SYSERR si no responde. */
int es8311_mic_status(void);

#endif                          /* _ES8311DRV_H_ */
