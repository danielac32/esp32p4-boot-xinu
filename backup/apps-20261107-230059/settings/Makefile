# Makefile de ajustes: settings.c + la libreria de gestos + start.S, enlazados
# con 'ld -r'.  Ver apps/ejemplo/Makefile para el por que de cada bandera.
#
# Para una prueba sin dedo:
#   make PRUEBA_AUTO=1              abre WiFi y escanea sola
#   make PRUEBA_AUTO=1 PRUEBA_CONECTA=1
#                                   ademas conecta a la primera red del fichero

TOPDIR = ../../../..

CC = riscv32-unknown-elf-gcc
LD = riscv32-unknown-elf-ld

CFLAGS = -Os -fno-builtin -fno-pic -ffunction-sections -std=gnu11 \
         -march=rv32imaf_zicsr_zifencei -mabi=ilp32f -mno-relax \
         -msmall-data-limit=0 -Wall \
         -I$(TOPDIR) -I$(TOPDIR)/include -I$(TOPDIR)/lvgl -I$(TOPDIR)/fatfs \
         -I$(TOPDIR)/system/platforms/esp32p4 -I../lib

ifdef PRUEBA_AUTO
CFLAGS += -DPRUEBA_AUTO
endif
ifdef PRUEBA_CONECTA
CFLAGS += -DPRUEBA_CONECTA
endif

APP  = settings
OBJS = settings.o ../lib/gestos.o ../lib/start.o

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
	rm -f settings.o settings.elf

.PHONY: all clean
