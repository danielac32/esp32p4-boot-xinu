/*
 * ESP32-P4 SBL (Second Stage Bootloader)
 *
 * The ROM bootloader loads this code into HP_TCM (flash offset 0x2000)
 * and jumps to it. Both cores start here: core 0 initializes the SoC
 * (WDT, clock, PSRAM, MMU, releases core1, optional ULP) and then both
 * cores fall through to the kernel entry point at 0x40005000.
 *
 * Copied from Chalandi's Baremetal_esp32p4_nosdk (Code/SBL/boot.S).
 */

    .section .boot
    .type _start, @function
    .align 2
    .globl _start

    .set BOOT_ADDR_HP_CORE1, 0x50110164
    .set BOOT_ADDR_LP_CORE, 0x50110028
    .set SOC_CLK_CTRL0, 0x500E6014
    .set HP_RST_EN0, 0x500E60C0
    .set L2_MEM_RAM_PWR_CTRL0_REG, 0x500e5060
    .set SCRATCHPAD_MEM_RAM_PWR_CTRL0_REG, 0x500e503c

_start:
    /* Read machine core id */
    csrr a0, mhartid
    bnez a0, .L_core1

    /* turn-on the internal memories' clock */
    li a5, L2_MEM_RAM_PWR_CTRL0_REG
    li a6, SCRATCHPAD_MEM_RAM_PWR_CTRL0_REG
    li a0, 1
    sw a0, 0(a5)
    sw a0, 0(a6)

    /* setup the stack (using the HP_L2MEM mem space) */
    li sp, 0x4ffa0000

    /* initialize the SoC */
    jal wdt_disable_all_wdt
    jal clock_init
    jal psram_init
    jal mmu_init

    /* release the core1 from reset */
    la a0, .L_core1
    li a5, BOOT_ADDR_HP_CORE1
    sw a0, 0(a5)
    li a5, SOC_CLK_CTRL0
    lw a0, 0(a5)
    ori a0, a0, (1 << 4)
    sw a0, 0(a5)
    li a5, HP_RST_EN0
    lw a0, 0(a5)
    andi a0, a0, ~(1 << 8)
    sw a0, 0(a5)

#ifdef COPROCESSOR_ENABLED
    /* wakeup ULP from sleep-mode */
    jal ulp_boot
#endif

    /* fall through: both cores jump to the kernel entry point */
.L_core1:
    la a5, __kernel_entry_point
    jr a5
    j .

    .size _start, .-_start
