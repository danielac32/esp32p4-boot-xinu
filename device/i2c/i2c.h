/*
 * i2c — maestro I2C0 en GPIO 7/8 (bus compartido: touch + ES8311 + AXP2101).
 *
 * SCL/SDA por la matriz GPIO (gpio_sig_map: SCL=68, SDA=69), open-drain
 * con pull-ups internos (+ los externos de la placa).
 * Polling con timeouts: sin interrupciones, sin DMA.
 *
 * Este driver es una librería cruda: habla con los registros por offsets
 * (ver esp32p4.h) y NO usa semáforos/monitores del kernel.  El device
 * `i2c` (device/i2c) es quien serializa el acceso al bus.
 */
#ifndef I2C_H
#define I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void i2c_init(void);

/* true si el dispositivo responde a su dirección (7 bits). */
bool i2c_probe(uint8_t dev);

/* Transacciones completas con START/STOP (len>0). */
bool i2c_write(uint8_t dev, const uint8_t *data, size_t len);
bool i2c_read(uint8_t dev, uint8_t *data, size_t len);

/* Atajos registro de 8 bits (write reg+valor / write reg + repeated START + read). */
bool i2c_write_reg8(uint8_t dev, uint8_t reg, uint8_t val);
bool i2c_read_regs(uint8_t dev, uint8_t reg, uint8_t *buf, size_t len);

/* Registro de 16 bits big-endian (p.ej. GT911): addr_hi+addr_lo. */
bool i2c_write_reg16(uint8_t dev, uint16_t reg, uint8_t val);
bool i2c_read_regs16(uint8_t dev, uint16_t reg, uint8_t *buf, size_t len);

/* Motivo del último fallo de transacción (lo reescribe cada i2c_run()).
 * I2C_ERR_STUCK = el FSM no reportó nada en 50 ms: línea retenida a 0 o
 * reloj del controlador parado; los demás son los intentos normales del bus. */
#define I2C_ERR_NONE          0
#define I2C_ERR_NACK          1   /* el esclavo no respondió a la dirección */
#define I2C_ERR_BUS_TIMEOUT   2   /* SCL/SDA retenidas > 2^tout ciclos       */
#define I2C_ERR_ARBITRATION   3   /* perdió la arbitraje (otro maestro)      */
#define I2C_ERR_STUCK         4   /* sin FIN de transacción al pasar el reloj */
int i2c_last_error(void);

#endif /* I2C_H */
