/*
 * ESP32-P4 SBL (Second Stage Bootloader) Linker Script
 *
 * The SBL is linked in HP_TCM (0x30100000) but flashed at offset 0x2000.
 * The ROM copies it to TCM on every boot.
 *
 * Memory map (ESP32-P4):
 *   HP_TCM      0x30100000  8 KB    (SBL execution)
 *   FLASH       0x40000000  64 MB   (kernel, filesystem)
 *   PSRAM       0x48000000  64 MB   (heap, buffers)
 *   HP_L2MEM    0x4FF00000  768 KB  (critical code, stacks)
 *   LP_SRAM     0x50108000  32 KB   (ULP coprocessor)
 */

ENTRY(_start)

MEMORY
{
    HP_TCM(rwx)  : ORIGIN = 0x30100000, LENGTH = 8k
}

SECTIONS
{
    .text : { *(.boot) *(.rodata) } > HP_TCM
}

/* Kernel entry point (linked in flash, executed via XIP) */
__kernel_entry_point = 0x40005000;

/* ULP entry point: código en flash (XIP) en el offset 0x400000.  Estaba en
 * 0x100000 (1 MB) y se subio a 4 MB el 2026-11-04, antes de meter LVGL: el
 * kernel todavia va en 353 KB pero le queda mucho codigo por delante, y con
 * la ranura en 1 MB el `make flashxinu' (que solo escribe en 0x5000) habria
 * pisado el firmware del LP core al crecer por encima de ese MB.  La cota de
 * arranque del LP la fijan ESTE simbolo y el ORIGIN de FLASH de
 * machine/ulp/ulp.ld; el hueco libre desde aqui hasta el final del kernel lo
 * comprueba `syscheck flash' en placa (shell/xsh_syscheck.c).  El SBL sólo
 * carga este símbolo en ULP_ENTRY_POINT_REG; no hay más acoplamiento. */
__ulp_entry_point = 0x40400000;

/* Peripheral base addresses */
SPIMEM2       = 0x5008E000;
SPIMEM3       = 0x5008F000;
LPPERI        = 0x50120000;
HP_SYS_CLKRST = 0x500E6000;

/* ROM API functions (called by absolute address) */
ets_printf              = 0x4fc00024;
esp_rom_spi_set_op_mode = 0x4fc00110;
esp_rom_spi_cmd_config  = 0x4fc00108;
esp_rom_spi_cmd_start   = 0x4fc0010c;
ets_clk_get_xtal_freq   = 0x4fc0055c;
ets_clk_get_cpu_freq    = 0x4fc00560;
