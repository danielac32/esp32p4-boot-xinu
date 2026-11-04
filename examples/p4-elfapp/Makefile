# Makefile -- app ELF cargable con simbolos sin resolver (p4-elfapp).
#
#   make          compila, enlaza PARCIAL y verifica
#   make verify   solo las comprobaciones
#   make clean
#
# No forma parte del build de Xinu: examples/ no esta en COMPS
# (compile/Makefile:242).  Lo que produce es app.elf, que se carga en placa con
# el cargador de referencia xsh_elfapp.c (vease README.md).
#
# La clave del truco es 'ld -r' (enlazado parcial, ET_REL): en vez de cerrar el
# fichero contra una libc que no existe, deja los simbolos externos como UND en
# .symtab y sus parches como .rela.*.  Eso es exactamente lo que necesita un
# cargador tipo 'elfrun': poner direcciones, resolver por nombre y aplicar.  Si
# enlazas completo (sin -r) no queda nada que resolver y el ELF exige una libc.

CROSS   := riscv32-unknown-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf
NM      := $(CROSS)nm

# Las marchas del kernel (compile/platforms/esp32p4/platformVars:12), MENOS la 'c'.
#
# La 'c' se quita a proposito: es lo que decidio el fallo medido en placa.  Con
# instrucciones comprimidas una de 4 bytes puede caer en direccion 2-mod-4 (visto
# en .text.startup.main: un 'sw' comprimido de 2 bytes en 0x8 deja el 'addi' que
# lleva R_RISCV_LO12_I en 0x0a, y la seccion sale con sh_addralign=2).  El parche
# del cargador escribe una palabra de 32 bits, y ese lw/sw en 0x...a seria
# 'load/store address misaligned'.  Sin 'c' toda instruccion mide 4, las secciones
# salen alineadas a 4 y el parche por palabra es correcto.  Un app CON comprimidas
# tambien se podria: habria que parchear por medias palabras, que el campo imm de
# un I-type cabe entero en la mitad alta y en un U-type se reparte 12/16.
MARCH := -march=rv32imaf_zicsr_zifencei -mabi=ilp32f

# -mno-relax no es de adorno: explica por que el cargador puede ser simple.  Con
# relaxation aparecen R_RISCV_RELAX/R_RISCV_ALIGN y el par HI20/LO12 puede
# encogerse a instruccion comprimida, de modo que parchear la palabra vecina ya no
# sirve.
#
# -msmall-data-limit=0 es el nombre real de lo que se pedia como '-G0' (este GCC
# 16.1 la rechaza: 'error: unrecognized command-line option -G0', medido).  Sin el
# limite a cero, CUALQUIER
# estatica de menos de 8 bytes se va a .sdata/.srodata relativa a gp y arrastra
# R_RISCV_LO12_S, que el cargador no aplica: medido con 'static int contador' en
# app.c -> .sdata + 1x LO12_S.  Con el limite a cero todo lo absoluto va por
# HI20+LO12_I, no hay que calcular __global_pointer$ ni cargar gp antes de saltar,
# y la lista de tipos vuelve a ser la que el cargador cubre.
CFLAGS := $(MARCH) -Os -ffreestanding -nostdinc -nostdlib -fno-pic -fno-builtin \
          -fno-strict-aliasing -fno-common -ffunction-sections \
          -msmall-data-limit=0 -mno-relax -Wall

OBJS := start.o app.o              # helper.o fuera: el selftest no llama a nada
                                      # de otra unidad (su app.c lo declara asi).

all: app.elf app_elf.h verify

app.elf: $(OBJS)
	$(LD) -r -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

start.o: start.S
	$(CC) $(CFLAGS) -c -o $@ $<

# --- el ELF como array de bytes, para no depender de la microSD ---------------
# El cargador parsea el ELF in situ, asi que el fichero entero se importa como
# header.  aligned(8) no es cosmestica: las estructuras ELF se leen por puntero y
# un array de 'unsigned char' solo garantiza 1.  Con -nostdlib el array vive en
# .rodata del kernel y el cargador lo copia a PSRAM por la ventana directa.
app_elf.h: app.elf
	@printf '/* Generado por make a partir de app.elf -- no editar. */\n\n__attribute__((aligned(8)))\nstatic const unsigned char app_elf[] = {\n' > $@
	@od -An -v -tx1 -w12 $< \
	  | sed 's/^ +//; s/ +$$//' \
	  | awk '{n=split($$0,b," "); printf "   "; for(i=1;i<=n;i++) printf " 0x%s,", b[i]; printf "\n"}' >> $@
	@printf '};\n\nstatic const unsigned int app_elf_len = sizeof(app_elf);\n' >> $@

# --- comprobaciones: esto es lo que tiene que saber el cargador --------------

verify: app.elf
	@echo '--- 1. tipo de fichero: ET_REL, y Entry=0 (no es ejecutable tal cual) ---'
	@$(READELF) -h $< | sed -n 's/^  *\(Type\|Machine\|Entry point\|Number of section\): */\1: /p'
	@echo '--- 2. secciones ALLOC: las que hay que colocar (y .bss es NOBITS) ---'
	@$(READELF) -W -S $< | sed -n '/Name/,$$p'
	@echo '--- 3. simbolos UND: la interfaz que el cargador tiene que exportar ---'
	@$(READELF) -W -s $< | awk '$$7=="UND" && $$8!=""{print "   UND -> " $$8}' | sort -u
	@echo '--- 4. tipos de relocacion presentes: el cargador tiene que cubrir TODOS ---'
	@$(READELF) -W -r $< | grep -o 'R_RISCV[A-Z0-9_]*' | sort | uniq -c
	@echo '--- 5. simbolo de entrada ---'
	@$(NM) $< | grep -w _start
	@echo '--- 6. desmontaje del arranque (los call sin resolver son el punto) ---'
	@$(OBJDUMP) -d -j .text.start $< | sed -n '/Disassembly of section/,$$p'

clean:
	rm -f $(OBJS) app.elf app_elf.h

.PHONY: all verify clean
