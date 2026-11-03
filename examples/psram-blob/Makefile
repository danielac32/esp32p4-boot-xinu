# Makefile -- cadena correcta para obtener blobs ejecutables desde PSRAM.
#
#   make          compila, enlaza, VUELCA y VERIFICA los dos blobs
#   make verify   solo las comprobaciones
#   make clean
#
# Dos blobs, mismo pipeline:
#   sum.c    -> sum_blob.h     (hoja de 4 bytes; el minimo didactico)
#   sqsum.c  -> sqsum_blob.h   (bucle + helper + marco de pila; varias lineas)
#
# No forma parte del build de Xinu: examples/ no esta en COMPS
# (compile/Makefile:242).  Todo el ejercicio vive aqui: los blobs, los headers
# generados y los cargadores de referencia (xsh_runblob.c, xsh_runbig.c,
# xsh_irqmap.c), que NO se compilan en este Makefile.  README.md explica lo que
# se midio en placa y como enganchar los comandos en shell/ si algun dia se
# quieren.

CROSS   := riscv32-unknown-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf

BLOBS   := sum sqsum

# Las mismas marchas que usa el kernel (compile/platforms/esp32p4/platformVars:12)
MARCH := -march=rv32imafc_zicsr_zifencei -mabi=ilp32f
CFLAGS := $(MARCH) -O2 -ffreestanding -fno-pic -nostdlib -Wall

# Los intermedios se quedan en el arbol: sin esto, GNU make borra .bin/.o/.elf
# por ser prerequisitos de patron, y 'make verify' ya no tendria con que trabajar.
.PRECIOUS: %.o %.elf %.bin

all: $(BLOBS:%=%_blob.h) verify

# --- pipeline generico ------------------------------------------------------

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Enlazado en 0 para que el volcado plano empiece en el primer byte, con la
# entrada en el offset 0 gracias a la seccion .text.entry (link.ld).
# ld no acepta -march/-mabi (eso es del compilador): el .o ya trae los
# atributos de ISA y ABI, asi que aqui no hacen falta.
%.elf: %.o link.ld
	$(LD) -T link.ld -o $@ $<

# El blob: solo .text, sin cabecera ELF.
%.bin: %.elf
	$(OBJCOPY) -O binary --only-section=.text $< $@

# Header con el array de bytes, listo para #include en Xinu.  El nombre del
# simbolo sale del propio stem: sum_blob[] / sum_blob_len, sqsum_blob[]...
# (od en hex por byte; awk monta las lineas. No hay xxd en esta maquina.)
%_blob.h: %.bin
	@stem=`basename $< .bin`; \
	 printf '/* Generado por make a partir de %s.bin -- no editar. */\n\nstatic const unsigned char '"$$stem"'_blob[] = {\n' "$$stem" > $@; \
	 od -An -v -tx1 -w12 $< \
	   | sed 's/^ \+//; s/ \+$$//' \
	   | awk '{n=split($$0,b," "); printf "   "; for(i=1;i<=n;i++) printf " 0x%s,", b[i]; printf "\n"}' >> $@; \
	 printf '};\n\nstatic const unsigned int '"$$stem"'_blob_len = sizeof('"$$stem"'_blob);\n' >> $@

# --- comprobaciones ---------------------------------------------------------

verify: $(BLOBS:%=%_blob.h)
	@for b in $(BLOBS); do \
	  echo "=============== $$b ==============="; \
	  echo '--- 1. desmontaje (la entrada tiene que salir primera, en 0x0) ---'; \
	  $(OBJDUMP) -d $$b.elf | sed -n '/^[0-9a-f]\+ <[a-z]\+>:/,/^$$/p'; \
	  echo '--- 2. relocations: DEBE salir 0 (si no, el blob no es plano) ---'; \
	  $(READELF) -r $$b.elf | grep -c 'R_RISCV' | sed 's/^/relocations: /'; \
	  echo '--- 3. simbolo de entrada: su direccion tiene que ser 0 ---'; \
	  $(READELF) -s $$b.elf | grep -w $$b | sed 's/^ *//'; \
	  echo '--- 4. tamano del blob y lineas de cache que ocupa (64 B/linea) ---'; \
	  n=`wc -c < $$b.bin`; echo "bytes: $$n   lineas: $$(( (n + 63) / 64 ))"; \
	  echo '--- 5. primera palabra (lo que hay que verificar tras el memcpy) ---'; \
	  od -An -v -tx4 -N4 $$b.bin; \
	done

clean:
	rm -f $(BLOBS:%=%.o) $(BLOBS:%=%.elf) $(BLOBS:%=%.bin) $(BLOBS:%=%_blob.h)

.PHONY: all verify clean
