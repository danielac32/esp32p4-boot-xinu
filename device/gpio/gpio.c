/*
 * ESP32-P4 GPIO driver
 *
 * Addresses verified against esp32p4.svd:
 *   GPIO   0x500E0000: OUT 0x04, OUT_W1TS 0x08, OUT_W1TC 0x0C,
 *          OUT1_W1TS 0x14, OUT1_W1TC 0x18,
 *          ENABLE 0x20, ENABLE_W1TS 0x24, ENABLE_W1TC 0x28,
 *          ENABLE1_W1TS 0x30, ENABLE1_W1TC 0x34,
 *          IN 0x3C, IN1 0x40, CLOCK_GATE 0x64C (reset value 1)
 *   IO_MUX 0x500E1000: pin n register at +4 + 4*n (dim=54, stride 4).
 *          The header's IO_MUX struct has a wrong stride — raw offsets
 *          are used here instead (same formula Chalandi uses).
 *
 * Pad function bits (verified vs SVD + Espressif io_mux_reg.h):
 *   FUN_WPD=7, FUN_WPU=8, FUN_IE=9, FUN_DRV=10..11,
 *   MCU_SEL=12..14  — y PIN_FUNC_GPIO = 1 (Espressif's own header),
 *   so MCU_SEL must be 1 for plain GPIO. FUNC%s_OUT_SEL_CFG resets to
 *   256 (OUT signal = GPIO_OUT_REG); OEN_SEL (bit 10) is set here
 *   to make the pin's output follow GPIO_ENABLE (Chalandi does this,
 *   ESP-IDF/Zephyr never touch the field and default works too).
 *
 * Used pins on this board (<= 54): I2S_AMP_POWER=53, C6_RESET=54.
 */

#include "gpio.h"
#include "esp32p4.h"

/* raw register offsets inside GPIO (SVD-verified) */
#define GPIO_OUT_W1TS_REG     (GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC_REG     (GPIO_BASE + 0x0C)
#define GPIO_OUT1_W1TS_REG    (GPIO_BASE + 0x14)
#define GPIO_OUT1_W1TC_REG    (GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_REG  (GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TC_REG  (GPIO_BASE + 0x28)
#define GPIO_ENABLE1_W1TS_REG (GPIO_BASE + 0x30)
#define GPIO_ENABLE1_W1TC_REG (GPIO_BASE + 0x34)
#define GPIO_IN_REG           (GPIO_BASE + 0x3C)
#define GPIO_IN1_REG          (GPIO_BASE + 0x40)
#define GPIO_CLOCK_GATE_REG   (GPIO_BASE + 0x64C)

/* IO_MUX: fórmula y bits en gpio.h (bases de esp32p4.h). */

/* GPIO_FUNC%s_OUT_SEL_CFG[pin] (SVD: dim=57, stride 4, offset 0x558,
 * reset 0x00000100): OUT_SEL bits 8:0 (=256 -> signal = GPIO_OUT_REG),
 * INV_SEL bit 9, OEN_SEL bit 10, OEN_INV_SEL bit 11 */
#define GPIO_OUT_SEL_CFG_REG(pin) (GPIO_BASE + 0x558UL + 4UL * (pin))
#define GPIO_IN_SEL_CFG_REG(sig)  (GPIO_BASE + 0x158UL + 4UL * (sig))

#define GPIO_OUT_SEL_MASK     0x1FFUL
#define GPIO_OUT_SEL_DEFAULT  256UL       /* OUT signal = GPIO_OUT_REG */
#define GPIO_OEN_SEL          (1UL << 10) /* OEN = GPIO_ENABLE_REG */

#define GPIO_PIN_MAX          54U   /* SOC_GPIO_PIN_COUNT=55 -> GPIO0..54 */
#define IO_MUX_PIN_MAX        54U   /* IO_MUX_GPIO54_REG existe (0xDC) */

/* GPIO_PINn_REG (SVD: dim=57, stride 4, offset 0x74): PAD_DRIVER bit 2
 * (1 = open-drain, 0 = normal). */
#define GPIO_PIN_REG(pin)     (GPIO_BASE + 0x74UL + 4UL * (pin))
#define GPIO_PIN_PAD_DRIVER   (1UL << 2)

void gpio_init(void)
{
    /* enable the GPIO peripheral clock (reset value is already 1) */
    *(volatile uint32_t *)GPIO_CLOCK_GATE_REG = 1;
}

void gpio_set_dir(uint8_t pin, gpio_dir_t dir)
{
    volatile uint32_t *out_sel = (volatile uint32_t *)GPIO_OUT_SEL_CFG_REG(pin);
    uint32_t val;

    if (pin > GPIO_PIN_MAX) {
        return;
    }

    /* pad: function GPIO, drive strength 2 (same recipe as Chalandi) */
    if (pin <= IO_MUX_PIN_MAX) {
        volatile uint32_t *pad = (volatile uint32_t *)IO_MUX_PIN_REG(IO_MUX_BASE, pin);

        val = *pad;
        val &= ~(IO_MCU_SEL_MASK | IO_FUN_IE | IO_FUN_DRV_MASK);
        val |= IO_MCU_SEL_GPIO | IO_FUN_DRV_20MA;
        if (dir == GPIO_INPUT) {
            val |= IO_FUN_IE; /* input enable */
        }
        *pad = val;
    }

    if (dir == GPIO_OUTPUT) {
        /* out mux: signal from OUT_REG, output enable from ENABLE_REG */
        *out_sel = (*out_sel & ~GPIO_OUT_SEL_MASK) | GPIO_OUT_SEL_DEFAULT
                   | GPIO_OEN_SEL;
        /* enable the output driver */
        if (pin < 32) {
            *(volatile uint32_t *)GPIO_ENABLE_W1TS_REG = 1UL << pin;
        } else {
            *(volatile uint32_t *)GPIO_ENABLE1_W1TS_REG = 1UL << (pin - 32);
        }
    } else {
        if (pin < 32) {
            *(volatile uint32_t *)GPIO_ENABLE_W1TC_REG = 1UL << pin;
        } else {
            *(volatile uint32_t *)GPIO_ENABLE1_W1TC_REG = 1UL << (pin - 32);
        }
    }
}

void gpio_set_pull(uint8_t pin, gpio_pull_t pull)
{
    volatile uint32_t *pad = (volatile uint32_t *)IO_MUX_PIN_REG(IO_MUX_BASE, pin);
    uint32_t val;

    if (pin > IO_MUX_PIN_MAX) {
        return; /* fuera de rango (GPIO0..54) */
    }

    val = *pad & ~(IO_FUN_WPU | IO_FUN_WPD);
    if (pull == GPIO_PULL_UP) {
        val |= IO_FUN_WPU;
    } else if (pull == GPIO_PULL_DOWN) {
        val |= IO_FUN_WPD;
    }
    *pad = val;
}

void gpio_set_open_drain(uint8_t pin, bool enable)
{
    volatile uint32_t *cfg;
    uint32_t v;

    if (pin > GPIO_PIN_MAX) {
        return;
    }

    cfg = (volatile uint32_t *)GPIO_PIN_REG(pin);
    v = *cfg;
    if (enable) {
        v |= GPIO_PIN_PAD_DRIVER;
    } else {
        v &= ~GPIO_PIN_PAD_DRIVER;
    }
    *cfg = v;
}

void gpio_write(uint8_t pin, bool level)
{
    if (pin > GPIO_PIN_MAX) {
        return;
    }

    if (pin < 32) {
        if (level) {
            *(volatile uint32_t *)GPIO_OUT_W1TS_REG = 1UL << pin;
        } else {
            *(volatile uint32_t *)GPIO_OUT_W1TC_REG = 1UL << pin;
        }
    } else {
        if (level) {
            *(volatile uint32_t *)GPIO_OUT1_W1TS_REG = 1UL << (pin - 32);
        } else {
            *(volatile uint32_t *)GPIO_OUT1_W1TC_REG = 1UL << (pin - 32);
        }
    }
}

bool gpio_read(uint8_t pin)
{
    uint32_t reg;

    if (pin > GPIO_PIN_MAX) {
        return false;
    }

    reg = (pin < 32) ? *(volatile uint32_t *)GPIO_IN_REG
                     : *(volatile uint32_t *)GPIO_IN1_REG;
    return (reg >> (pin & 31)) & 1;
}

void gpio_toggle(uint8_t pin)
{
    gpio_write(pin, !gpio_read(pin));
}

/* ----------------------------------------------------------------------
 * GPIO matrix routing (peripheral <-> pad).
 *
 * SVD (stride 4, matching gpio_reg.h — the SVD *struct* has a wrong
 * 16-byte stride, so raw offsets are used, same as the OUT mux above):
 *   FUNC%s_OUT_SEL_CFG[n] at 0x558 + 4*n   (n = GPIO number)
 *     OUT_SEL bits 8:0 (=256 -> GPIO_OUT_REG), INV_SEL bit 9,
 *     OEN_SEL bit 10, OEN_INV_SEL bit 11
 *   FUNC%s_IN_SEL_CFG[s]  at 0x158 + 4*s   (s = signal index, 0..255)
 *     IN_SEL bits 5:0 (GPIO number, or 0x3E=const0 / 0x3F=const1),
 *     IN_INV_SEL bit 6, SEL bit 7
 * ---------------------------------------------------------------------- */
#define GPIO_OUT_SEL_CFG_REG(pin) (GPIO_BASE + 0x558UL + 4UL * (pin))
#define GPIO_IN_SEL_CFG_REG(sig)  (GPIO_BASE + 0x158UL + 4UL * (sig))

#define GPIO_MAT_OUT_SEL_MASK    0x1FFUL
#define GPIO_MAT_OUT_INV_SEL     (1UL << 9)
#define GPIO_MAT_OUT_OEN_SEL     (1UL << 10)
#define GPIO_MAT_OUT_OEN_INV_SEL (1UL << 11)

#define GPIO_MAT_IN_SEL_MASK     0x3FUL
#define GPIO_MAT_IN_INV_SEL      (1UL << 6)
/* FUNC_IN_SEL_CFG[s] bit 7 (SIG_IN_SEL) picks the source of signal s:
 * 0 = the pad wired directly in the IOMUX, 1 = the GPIO matrix.  It must be
 * 1 for every route set up here, including the two constants; without it the
 * peripheral samples the IOMUX path and never sees the pin. */
#define GPIO_MAT_IN_SIG_IN_SEL   (1UL << 7)

#define GPIO_MAT_CONST_ZERO      0x3EU
#define GPIO_MAT_CONST_ONE       0x3FU

void gpio_matrix_out(uint8_t pin, uint32_t sig, bool out_inv, bool oen_inv)
{
    volatile uint32_t *r = (volatile uint32_t *)GPIO_OUT_SEL_CFG_REG(pin);
    uint32_t v = *r;

    v &= ~(GPIO_MAT_OUT_SEL_MASK | GPIO_MAT_OUT_INV_SEL |
           GPIO_MAT_OUT_OEN_SEL  | GPIO_MAT_OUT_OEN_INV_SEL);
    v |= (sig & GPIO_MAT_OUT_SEL_MASK);
    if (out_inv) { v |= GPIO_MAT_OUT_INV_SEL; }
    if (oen_inv) { v |= GPIO_MAT_OUT_OEN_INV_SEL; }
    /* OEN_SEL = 0: the output enable is governed by the peripheral. */
    *r = v;
}

void gpio_matrix_in(uint8_t pin, uint32_t sig, bool inv)
{
    volatile uint32_t *r = (volatile uint32_t *)GPIO_IN_SEL_CFG_REG(sig);
    uint32_t v;

    if (pin == GPIO_MAT_CONST_ZERO)
    {
        v = GPIO_MAT_IN_SIG_IN_SEL;             /* constant 0 */
    }
    else if (pin == GPIO_MAT_CONST_ONE)
    {
        v = GPIO_MAT_IN_SIG_IN_SEL | 1UL;       /* constant 1 */
    }
    else
    {
        v = GPIO_MAT_IN_SIG_IN_SEL | (pin & GPIO_MAT_IN_SEL_MASK);
        if (inv) { v |= GPIO_MAT_IN_INV_SEL; }
    }
    *r = v;
}
