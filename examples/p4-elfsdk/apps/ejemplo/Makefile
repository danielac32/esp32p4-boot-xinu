# Makefile del ejemplo: ejemplo.c + la libreria de gestos + start.S, enlazados
# con 'ld -r' en un ET_REL que el comando 'elf' del kernel carga desde la SD.
#
# Las banderas son las cuatro del ABI, cada una por un fallo medido en placa:
#   -march SIN 'c'      una instruccion comprimida cae en 2 mod 4 y el cargador
#                       parchea por palabras (misaligned, mcause=6)
#   -mno-relax          sin esto binutils encoge auipc+jalr a comprimidas y ya
#                       no se puede parchear la palabra vecina
#   -msmall-data-limit=0  una estatica pequena se iria a .sdata relativa a gp
#                       (R_RISCV_LO12_S, que el cargador no aplica)
#   -fno-pic            se resuelve por nombre; un GOT sin relocar apunta a 0
# Y -ffunction-sections va con ellas: cada funcion suelta su .text.<fn> y su
# .rela.text.<fn>, que es lo que el cargador coloca y parchea.
#
# El app se compila contra las cabeceras REALES del kernel: nada de libc ni de
# librerias propias, y 'ld -r' deja sin cerrar todo lo que llama fuera.  Ese es
# el producto: un fichero ET_REL con los UND que resuelve system/exports.c.  La
# unica fuente de codigo ajeno es ../lib/gestos.c, que cumple lo mismo.

# -std=gnu11 como el kernel: el gcc 16 por defecto compila C23, donde 'bool'
# ya es palabra reservada y el 'typedef char bool' de include/stddef.h rompe.

TOPDIR = ../../../..

CC = riscv32-unknown-elf-gcc
LD = riscv32-unknown-elf-ld

CFLAGS = -Os -fno-builtin -fno-pic -ffunction-sections -std=gnu11 \
         -march=rv32imaf_zicsr_zifencei -mabi=ilp32f -mno-relax \
         -msmall-data-limit=0 -Wall \
         -I$(TOPDIR) -I$(TOPDIR)/include -I$(TOPDIR)/lvgl -I$(TOPDIR)/fatfs \
         -I$(TOPDIR)/system/platforms/esp32p4 -I../lib

APP  = ejemplo
OBJS = ejemplo.o ../lib/gestos.o ../lib/start.o

all: $(APP).elf

$(APP).elf: $(OBJS)
	$(LD) -r -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

../lib/gestos.o: ../lib/gestos.c ../lib/gestos.h
	$(CC) $(CFLAGS) -c -o $@ $<

../lib/start.o: ../lib/start.S
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f ejemplo.o ../lib/gestos.o ../lib/start.o ejemplo.elf

.PHONY: all clean
