# p4-elfapp — cargar un ELF con símbolos sin resolver y saltar a él

Ejercicio **fuera de la compilación de Xinu**: `examples/` no está en `COMPS`
(`compile/Makefile:242`), así que nada de este directorio entra en
`compile/xinu.elf`.  El kernel no se toca.

Lo que hay aquí es la segunda mitad del ejercicio de ejecución en PSRAM
(`examples/psram-blob`).  Allí el blob era un puñado de instrucciones **sin
símbolos**: se copiaba, se verificaba y se saltaba.  Aquí el objeto ya es un
programa con `.symtab` y `.rela.*`, es decir, con huecos que hay que rellenar
antes de ejecutar nada.

## Los dos ficheros que importan

| Fichero | Qué es |
|---|---|
| `app.c` + `helper.c` + `start.S` | el programa cargable. Se compila freestanding, sin libc, y se enlaza **parcial** con `ld -r`. |
| `xsh_elfapp.c` | el cargador (un enlazador en tiempo de ejecución, estilo `elfrun`). **Referencia, no compilado.** |

`make` produce `app.elf` y `app_elf.h` (el ELF como array de bytes, para no
depender de la microSD) y luego imprime las comprobaciones de `verify`.

## Lo que `make verify` mide sobre `app.elf` (no es teoria, es salida)

```
Type: REL (Relocatable file)     <- no es ET_EXEC; tal cual no arranca
UND -> exit  memset  printf  psram_freemem  psram_malloc  strcmp  strlen
     14 R_RISCV_CALL_PLT
      8 R_RISCV_HI20
     15 R_RISCV_LO12_I
```

Tres tipos de relocación y siete símbolos.  Nada de 40 tipos, y eso es
consecuencia directa de las opciones de `CFLAGS`:

- `-fno-pic` → las direcciones absolutas van parcheadas en instrucción (HI20 +
  LO12_I), no por GOT.  Con `-fPIC` habrían aparecido `R_RISCV_GOT20_I12` y
  `.got`, y el caso que ya probamos a mano en este proyecto **petó**.
- `-mno-relax` → no aparecen `R_RISCV_RELAX` / `R_RISCV_ALIGN`, y el par
  `auipc`+`jalr` conserva las dos palabras enteras.  Si GCC comprime una de las
  dos, parchear la palabra vecina deja de ser correcto.
- Sin `.sdata`/`.sbss` (este GCC no acepta `-G0` y, en la práctica, no los
  genera) → no hay que calcular `__global_pointer$` ni cargar `gp`.  Si algún
  día aparecen, esto es obligatorio antes de saltar.

## El cargador, en orden

1. **Cabecera**: magic, `ELFCLASS32`, `EM_RISCV` y `ET_REL`.  Si alguien enlaza
   completo (sin `-r`), no queda nada que resolver y el cargador lo dice, en vez
   de fallar raro.
2. **Colocar**: una pasada calcula el tamaño del bloque (alineación de sección
   clampada a 4 en las **dos** pasadas, para que el cálculo no se desfase) y
   `psram_malloc`; la segunda asigna a cada sección con `SHF_ALLOC` su dirección
   dentro del bloque.  `SHT_NOBITS` (`.bss`) ocupa sitio pero no se copia del
   fichero: se pone a cero.
3. **Resolver**: los símbolos definidos valen `base[sección] + st_value`; los
   `UND` se buscan por nombre en `exports[]`, que apunta a funciones **reales**
   del kernel (`printf`, `memset`, `psram_malloc`, …).  Un `UND` sin exportar
   aborta antes de saltar.
4. **Parchear** los tres tipos medidos:
   - `R_RISCV_HI20` → bits 31:12 de `auipc`/`lui` con `(S+A+0x800)>>12`.
   - `R_RISCV_LO12_I` → bits 31:20 con `(S+A) & 0xFFF`.
   - `R_RISCV_CALL` / `CALL_PLT` → pareja: `d = (S+A) - P` donde `P` es la
     propia instrucción `auipc`; `hi = (d+0x800)>>12`, `lo = d - (hi<<12)`.
   Cualquier otro tipo aborta.  Y en cada CALL se **leen de vuelta las dos
   palabras parcheadas**, se reconstruye `sitio + hi + lo` y se compara con
   `S+A`: así el alcance no se deduce de una resta mental, se comprueba.
5. **Saltar** a `_start` por la ventana **directa** `0x88......`.

## Por qué por `0x88` y no por `0x48`

Es el resultado ya medido en `examples/psram-blob/README.md`, y aquí se
reutiliza tal cual: copiar por `0x88`, verificar por `0x88` y saltar por `0x88`
no necesita ninguna operación de caché.  Las variantes por la ventana cacheada
fallaron (línea rancia de L2 → `mcause=2`, `mtval=0xaaaaaaab`).  Como los
parches HI20/LO12 llevan puesta la base **directa**, los datos del app también
viven en `0x88`: el app lee celdas reales, no líneas rancias.

## Alcance de las llamadas: por qué no hacen falta trampolines

En RV32 la pareja `auipc`+`jalr` cubre una diferencia con signo de 32 bits, o
sea ±2 GiB.  Desde PSRAM directa (`0x88000000`) al texto del kernel
(`0x40000000`) hay `0x48000000` ≈ 1,21 G: dentro de rango.  Aun así se comprueba
en cada relocación y, si algún día no cabe, aborta imprimiendo el valor, en vez
de ejecutar una dirección equivocada.

## La pila y `printf`

El app **no es una tarea**: corre en el hilo del comando de la shell, así que
`sp` ya es válido y el marco de la shell queda debajo.  Por eso `start.S` solo
salva `ra` y `s0`.

El app usa `printf`, no `kprintf`: `kprintf` es para el kernel antes de tener
contexto de tarea; un proceso de la shell tiene su `stdout` en el tty y por eso
usa `printf`.

`exit()` no mata nada: el cargador la exporta como una función propia
(`eapp_exit`) que guarda el código y **devuelve**.  El `_start` del app cae
entonces en su `ret` y vuelve al punto de llamada del cargador, que imprime el
valor devuelto por `main` y el código de `exit()`.  Sin `setjmp` y sin un
segundo punto de salida.

## Para probarlo en placa (cuando el serial esté libre)

Nada de esto está compilado en la imagen.  Para pasar la prueba hace falta, **en
tu árbol y con tu build**:

1. Añadir el cargador al shell: copiar `xsh_elfapp.c` a `shell/`, su `C_FILES +=`
   en `shell/Makerules`, la entrada `{"elfapp", FALSE, xsh_elfapp},` en
   `shell/shell.c` y el prototipo en `include/shell.h`.  Ese paso **toca el
   kernel**, así que solo lo haré si lo autorizas.
2. `examples/p4-elfapp/app_elf.h` es un `#include` del cargador: el ELF viaja
   dentro de la imagen de flash como `.rodata` del kernel.
3. `make clean && make` (tras tocar un `.h` el `Makefile` no recalcula
   dependencias), flash y `elfapp`.

Salida esperada, en orden: cabecera con `nsec` y bytes, una línea `[texto]` /
`[datos]` por sección colocada, `N relocaciones leídas, M campos parcheados`,
`saltando a _start=0x........ por ventana DIRECTA` y, desde dentro del app, las
líneas `[elfapp] ...` de `printf`, acabando en `main devolvio 0` y `exit()` sin
código.

## Qué **no** es esto

No es un `exec()` del kernel ni un cargador de sistema de ficheros.  Es el
experimento que contesta la pregunta concreta: *¿puedo compilar un ELF aparte,
cargarlo, resolver sus símbolos, y saltar a su `main`?*  Lo medido aquí dice
que sí, con tres tipos de relocación, siete símbolos exportados y la ventana
directa de PSRAM.
