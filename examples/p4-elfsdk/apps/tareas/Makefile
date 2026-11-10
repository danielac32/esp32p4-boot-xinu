# Makefile del monitor de tareas: monitor.c + la libreria de gestos + start.S,
# enlazados con 'ld -r'.  El 'clean' NO borra los objetos de ../lib: los
# comparten todas las apps.

TOPDIR = ../../../..

CC = riscv32-unknown-elf-gcc
LD = riscv32-unknown-elf-ld

CFLAGS = -Os -fno-builtin -fno-pic -ffunction-sections -std=gnu11 \
         -march=rv32imaf_zicsr_zifencei -mabi=ilp32f -mno-relax \
         -msmall-data-limit=0 -Wall \
         -I$(TOPDIR) -I$(TOPDIR)/include -I$(TOPDIR)/lvgl -I$(TOPDIR)/fatfs \
         -I$(TOPDIR)/system/platforms/esp32p4 -I../lib

APP  = tareas
OBJS = monitor.o ../lib/gestos.o ../lib/start.o

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
	rm -f monitor.o tareas.elf

.PHONY: all clean
