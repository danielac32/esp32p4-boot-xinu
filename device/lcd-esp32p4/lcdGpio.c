/**
 * @file lcdGpio.c
 *
 * The few GPIO/IO_MUX operations the LCD device needs: put a pad on the
 * GPIO matrix, route an output signal to a pad, and drive the four panel
 * lines (CS, DC, RST, backlight).  The SPI clock and MOSI pads are routed
 * through the same matrix but keep their output enable under peripheral
 * control, so lcdSpiInit() calls lcdMatrixOut() with oenFromEnable=false.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stdint.h>
#include "lcdhw.h"

/* Pads 0..31 live in the OUT/ENABLE words, 32..54 in OUT1/ENABLE1. */
#define PIN_IN_LOW_WORD(pin)    ((pin) < 32U)

void lcdPadToGpio(uint8_t pin)
{
    volatile uint32_t *padptr = (volatile uint32_t *)P4_IOPAD(pin);
    uint32_t val;

    val = *padptr;
    val &= ~P4_IO_MCU_SEL_MASK;
    val |= P4_IO_MCU_SEL_GPIO | P4_IO_FUN_IE | P4_IO_FUN_DRV_40MA;
    *padptr = val;
}

void lcdMatrixOut(uint8_t pin, uint32_t sig, bool oenFromEnable)
{
    volatile uint32_t *regptr = (volatile uint32_t *)P4_GPIO_OUT_SEL(pin);
    uint32_t val;

    val = *regptr;
    val &= ~(P4_GPIO_OUT_SEL_MASK | P4_GPIO_OEN_SEL);
    val |= (sig & P4_GPIO_OUT_SEL_MASK);
    if (oenFromEnable)
    {
        val |= P4_GPIO_OEN_SEL;
    }
    *regptr = val;
}

void lcdPinWrite(uint8_t pin, bool level)
{
    uint32_t mask = 1UL << (PIN_IN_LOW_WORD(pin) ? pin : pin - 32U);

    if (PIN_IN_LOW_WORD(pin))
    {
        *(volatile uint32_t *)(level ? P4_GPIO_OUT_W1TS : P4_GPIO_OUT_W1TC) =
            mask;
    }
    else
    {
        *(volatile uint32_t *)(level ? P4_GPIO_OUT1_W1TS : P4_GPIO_OUT1_W1TC)
            = mask;
    }
}

void lcdPinSetup(uint8_t pin, bool level)
{
    uint32_t mask = 1UL << (PIN_IN_LOW_WORD(pin) ? pin : pin - 32U);

    lcdPadToGpio(pin);
    lcdMatrixOut(pin, P4_GPIO_OUT_SEL_GPIO, true);

    /* Set the level before the output is enabled: no glitch on CS or DC. */
    lcdPinWrite(pin, level);

    if (PIN_IN_LOW_WORD(pin))
    {
        *(volatile uint32_t *)P4_GPIO_ENABLE_W1TS = mask;
    }
    else
    {
        *(volatile uint32_t *)P4_GPIO_ENABLE1_W1TS = mask;
    }
}
