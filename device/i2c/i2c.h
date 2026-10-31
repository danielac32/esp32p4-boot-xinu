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

#endif /* I2C_H */
