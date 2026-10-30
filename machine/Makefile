##############################################################################
#  machine/ — imágenes de arranque del ESP32-P4 (SBL y ULP)
#
#  El kernel (xinu.boot) lo construye compile/Makefile; aquí se construyen las
#  otras dos imágenes del sistema, que NO forman parte del ELF del kernel:
#
#    machine/sbl  → build/sbl.bin   bootloader de 2ª etapa (HP: TCM 0x30100000,
#                                   flash 0x2000) — lleva cabecera esptool
#    machine/ulp  → build/ulp.bin   firmware del coprocesador LP (XIP
#                                   0x40100000, flash 0x100000) — binario crudo
#
#  El SBL hace el trabajo de arranque del SoC tal como el proyecto de Chalandi
#  (WDT, clock, PSRAM, MMU) y salta al kernel en __kernel_entry_point
#  (0x40005000). El kernel, por su parte, asume el SoC ya inicializado.
##############################################################################

TOOLSET  ?= riscv32-unknown-elf-
CC       := $(TOOLSET)gcc
LD       := $(TOOLSET)ld
OBJCOPY  := $(TOOLSET)objcopy
SIZE     := $(TOOLSET)size
ESPTOOL  ?= esptool

CHIP     := esp32p4

BUILD    := build

# esp32p4.h (registros del chip), clock.h y libpsram.a viven en sbl/.
INCS     := -I sbl/

# HP core (SBL): mismo ISA que el kernel.
#   -DCOPROCESSOR_ENABLED  → el SBL arranca el LP core (ulp_boot).
HP_FLAGS  := -march=rv32imafdc_zicsr_zifencei -mabi=ilp32f \
             -ffreestanding -nostdlib -nostartfiles -Os -msmall-data-limit=0 \
             -DCHIP_ESP32P4 -DCOPROCESSOR_ENABLED
# LP core (ULP): RV32IMC, sin FPU.
LP_FLAGS  := -march=rv32imc_zicsr_zifencei -mabi=ilp32 \
             -ffreestanding -nostdlib -nostartfiles -Os -msmall-data-limit=0

# ---------------------------------------------------------------------------
# Fuentes del SBL y del ULP.  Los objetos se escriben en build/{sbl,ulp}/ con
# el solo nombre base (build/sbl/boot.o), y cada regla de compilación está
# anclada a su directorio de origen (sbl/%.c, ulp/%.S, ...) para que el mismo
# patrón sirva a las dos imágenes sin duplicar la ruta en el objeto.
# ---------------------------------------------------------------------------
SBL_SRCS  := sbl/boot.S sbl/clock.c sbl/wdt.c sbl/mmu.c sbl/ulp_boot.c
ULP_SRCS  := ulp/boot.S ulp/intvect.S ulp/startup.c ulp/main.c

SBL_OBJS  := $(SBL_SRCS:sbl/%=$(BUILD)/sbl/%)
SBL_OBJS  := $(SBL_OBJS:.c=.o)
SBL_OBJS  := $(SBL_OBJS:.S=.o)
ULP_OBJS  := $(ULP_SRCS:ulp/%=$(BUILD)/ulp/%)
ULP_OBJS  := $(ULP_OBJS:.c=.o)
ULP_OBJS  := $(ULP_OBJS:.S=.o)

SBL_LD    := sbl/SBL.ld
ULP_LD    := ulp/ulp.ld
# Biblioteca precompilada de la que sale psram_init(); no se reconstruye aquí,
# pero sí de ella depende el enlace del SBL.
PSRAM_LIB := sbl/libpsram.a

SBL_ELF   := $(BUILD)/sbl.elf
SBL_BIN   := $(BUILD)/sbl.bin
ULP_ELF   := $(BUILD)/ulp.elf
ULP_BIN   := $(BUILD)/ulp.bin

# Un fallo a mitad de receta (p. ej. ld sin espacio, CC interrumpido) no debe
# dejar un .o/.elf truncado que se considere válido en el siguiente make.
.DELETE_ON_ERROR:

.PHONY: all sbl ulp clean
all: sbl ulp

sbl: $(SBL_BIN)
ulp: $(ULP_BIN)

# ---- SBL -------------------------------------------------------------------
# El BootROM lee una cabecera en el offset 0x2000 para saber a qué dirección
# cargar el SBL (HP_TCM 0x30100000) y dónde está su entry; un binario crudo
# sería rechazado ("invalid magic number"). Por eso el SBL lleva cabecera, vía
# `esptool elf2image` (igual que Chalandi). psram_init viene de libpsram.a
# (sbl/libpsram.a).
$(SBL_BIN): $(SBL_ELF)
	@$(ESPTOOL) --chip $(CHIP) elf2image -o $@ $<
	@echo "sbl.bin $$(stat -c%s $@) B"

$(SBL_ELF): $(SBL_OBJS) $(SBL_LD) $(PSRAM_LIB)
	@$(LD) -T $(SBL_LD) -nostdlib -o $@ $(SBL_OBJS) -Lsbl -lpsram
	@$(SIZE) $@

# ---- ULP -------------------------------------------------------------------
$(ULP_BIN): $(ULP_ELF)
	@$(OBJCOPY) -O binary $< $@
	@echo "ulp.bin $$(stat -c%s $@) B"

$(ULP_ELF): $(ULP_OBJS) $(ULP_LD)
	@$(LD) -T $(ULP_LD) -nostdlib -o $@ $(ULP_OBJS)
	@$(SIZE) $@

# ---- Reglas de compilación -------------------------------------------------
$(BUILD)/sbl/%.o: sbl/%.S
	@mkdir -p $(dir $@)
	$(CC) $(HP_FLAGS) $(INCS) -c $< -o $@

$(BUILD)/sbl/%.o: sbl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HP_FLAGS) $(INCS) -c $< -o $@

$(BUILD)/ulp/%.o: ulp/%.S
	@mkdir -p $(dir $@)
	$(CC) $(LP_FLAGS) -c $< -o $@

$(BUILD)/ulp/%.o: ulp/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LP_FLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)