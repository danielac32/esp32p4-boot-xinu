/**
 * @file gpio.h
 *
 * Raw ESP32-P4 GPIO / IO_MUX driver.  The pad n lives at base+4+4*n and the
 * function bits are FUN_WPD=7, FUN_WPU=8, FUN_IE=9, FUN_DRV=10..11 and
 * MCU_SEL=12..14 (PIN_FUNC_GPIO = 1); verified against the SVD, Espressif's
 * io_mux_reg.h and the Chalandi driver.  Bases come from esp32p4.h.
 */
#ifndef _GPIODRV_H_
#define _GPIODRV_H_

#include <stdint.h>
#include <stdbool.h>

/* Registro IO_MUX del pin n. */
#define IO_MUX_PIN_REG(base, pin)   ((base) + 4UL + 4UL * (pin))

/* IO_MUX field bits */
#define IO_FUN_WPD            (1UL << 7)
#define IO_FUN_WPU            (1UL << 8)
#define IO_FUN_IE            (1UL << 9)
#define IO_FUN_DRV_MASK       (3UL << 10)
#define IO_FUN_DRV_20MA       (2UL << 10)
#define IO_FUN_DRV_40MA       (3UL << 10)
#define IO_MCU_SEL_MASK       (7UL << 12)
#define IO_MCU_SEL_GPIO       (1UL << 12) /* PIN_FUNC_GPIO */
#define IO_MCU_SEL_DIRECT     (0UL << 12) /* función IOMUX directa (SDMMC...) */

/* GPIO direction */
typedef enum {
    GPIO_INPUT = 0,
    GPIO_OUTPUT = 1,
} gpio_dir_t;

/* GPIO pull */
typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP = 1,
    GPIO_PULL_DOWN = 2,
} gpio_pull_t;

/* Initialize GPIO subsystem */
void gpio_init(void);

/* Set GPIO direction */
void gpio_set_dir(uint8_t pin, gpio_dir_t dir);

/* Set GPIO pull resistor */
void gpio_set_pull(uint8_t pin, gpio_pull_t pull);

/* Set pad driver: true = open-drain (1), false = normal push-pull (0).
 * Necesario para I2C (SDA/SCL): el pad sólo hunde, la pull-up da el alto. */
void gpio_set_open_drain(uint8_t pin, bool enable);

/* Write GPIO value */
void gpio_write(uint8_t pin, bool value);

/* Read GPIO value */
bool gpio_read(uint8_t pin);

/* Toggle GPIO */
void gpio_toggle(uint8_t pin);

/* Route a peripheral signal through the GPIO matrix.
 *   out: signal index @p sig is driven out on pad @p pin.
 *   in:  pad @p pin feeds signal index @p sig.
 * @p pin may also be a constant level (0x3E = low, 0x3F = high) for the
 * input case.  These are the two ROM helpers (esp_rom_gpio_*) that the
 * peripheral drivers need, implemented with raw GPIO-matrix offsets. */
void gpio_matrix_out(uint8_t pin, uint32_t sig, bool out_inv, bool oen_inv);
void gpio_matrix_in(uint8_t pin, uint32_t sig, bool inv);

#endif /* _GPIODRV_H_ */
