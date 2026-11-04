/**
 * @file xsh_elfapp.c
 *
 * Copia compilada del cargador de examples/p4-elfapp/xsh_elfapp.c (ahi esta el
 * README y el make que genera app_elf.h).  Lo unico cambiado es la ruta del
 * include generado.
 *
 * El 'elfrun' de Valdanylchuk y el elf_execve() de riscvima-emu4
 * (kernel/src/elf.c, 129 lineas) hacen COSAS DISTINTAS, y la diferencia es lo
 * que hay que entender antes de escribir una linea:
 *
 *   - emu4 carga un ELF **ya enlazado** (ET_EXEC, un solo PT_LOAD enlazado en 0):
 *     copia el segmento a un buffer, pone .bss a cero, suma la base al .got y a
 *     .init_array, y salta a e_entry.  No resuelve NI UN SIMBOLO.  Solo funciona
 *     si el app no necesita nada del anfitrion y si todas sus direcciones
 *     absolutas estan en el GOT (o sea, compilado PIC).  Ese es exactamente el
 *     caso del binario -fPIC que se pego a mano y dio excepcion.
 *   - elfrun (la tabla exports[]/resolve() que copiaste) si resuelve simbolos:
 *     es un enlazador.  Y eso es lo que queremos, porque el app tiene que llamar
 *     a printf y a psram_malloc del kernel.
 *
 * Aqui va la version Xinu del enlazador, y los numeros los da el propio
 * 'make verify' de este directorio sobre app.elf: 3 tipos de relocacion y 7
 * simbolos sin resolver.  Nada de 40 tipos: -fno-pic + -mno-relax + sin datos
 * pequenos deja un subconjunto que cabe en una funcion.
 *
 * QUE HACE, PASO A PASO
 *
 *   1. Comprueba la cabecera: magic, ELFCLASS32, EM_RISCV y **ET_REL**.  ET_REL
 *      es lo que produce 'ld -r'; si alguien enlaza completo sin -r no habra
 *      .symtab que resolver y esto tiene que decirno, no fallar raro.
 *   2. Coloca: recorre las secciones con SHF_ALLOC y les asigna direccion dentro
 *      del bloque reservado en PSRAM, respetando sh_addralign.  .bss (NOBITS)
 *      ocupa sitio pero no se copia del fichero: se pone a cero.
 *   3. Resuelve simbolos: los definidos valen base[seccion] + st_value; los UND
 *      se buscan por nombre en la tabla de exportaciones, que apunta a funciones
 *      reales del kernel (printf, memset, psram_malloc...).  Esta es la parte que
 *      no tiene emu4.
 *   4. Aplica relocaciones.  Tres tipos, y los tres son inmediatos de 32 bits,
 *      no datos:
 *        R_RISCV_HI20    auipc/lui   -> bits 31:12 = (S+A+0x800)>>12
 *        R_RISCV_LO12_I  addi/i-type -> bits 31:20 = (S+A) & 0xFFF
 *        R_RISCV_CALL(_PLT) auipc+jalr (par) -> d = (S+A)-P, con P = la propia
 *                                        instruccion auipc; hi = (d+0x800)>>12,
 *                                        lo = d - (hi<<12)
 *      CUALQUIER otro tipo aborta ANTES de saltar.  Una relocacion sin aplicar
 *      apunta a 0x0000xxxx y eso es mcause=5 o mcause=2, el fallo de siempre.
 *   5. Salta por la ventana DIRECTA (0x88......), que es la ruta ya probada en
 *      este proyecto (README del ejercicio anterior): copiar por 0x88 y ejecutar
 *      por 0x88 no necesita operaciones de cache.  Los DATOS del app tambien
 *      quedan en direcciones 0x88 (los parches HI20/LO12 llevan la base directa),
 *      asi que el app ve celdas reales, no lineas rancias.
 *
 * ALCANCE DE LAS LLAMADAS (por que no hacen falta trampolines): en RV32 la pareja
 * auipc+jalr cubre la diferencia con signo de 32 bits, o sea +-2 GiB.  Desde
 * PSRAM directa (0x88000000) al texto del kernel (0x40000000) hay 0x48000000 =
 * 1,21 G, dentro de rango.  Se comprueba de verdad en cada relocacion y, si algun
 * dia no cabe, aborta con el valor impreso en vez de ejecutar una direccion
 * equivocada.
 *
 * LA PILA: el app NO es una tarea.  Corre en el hilo del comando de la shell, asi
 * que SP ya es valido y el marco de la shell queda debajo; por eso start.S solo
 * salva ra y s0.  printf funciona porque ese proceso tiene su stdout en el tty (el
 * kernel, antes de tener contexto de tarea, usa kprintf).
 *
 * exit(): no mata nada.  El cargador la exporta como una funcion propia que
 * guarda el codigo y DEVUELVE; el _start del app cae entonces en su 'ret' y
 * vuelve al punto de llamada del cargador.  Sin setjmp y sin segundo punto de
 * salida.
 */
/* Embedded Xinu's reference loader, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <elf.h>                       /* ELF32_hdr/shdr, Elf32_Sym/Rela: ya existe */

#include "examples/p4-elfapp/app_elf.h"  /* generado por make en examples/      */

/** Ventana directa de PSRAM: 0x48...... + 0x40000000 = 0x88...... (mismas celdas). */
#define PSRAM_DIRECT        0x40000000U

/** Alineacion minima de una seccion colocada: 4.  Con 2 basta para instruciones
 *  comprimidas, pero los parches escriben palabras completas, asi que se exige 4
 *  en las DOS pasadas (tamano y direccion) para que el calculo no se desfase. */
#define ALINEA_MIN          4U

#define ET_REL              1           /* objeto relocalizable: lo que hace ld -r  */
#define SHN_UNDEF           0
#define SHN_ABS             0xfff1      /* p.ej. __global_pointer$                  */

#define P_RISCV_32          1           /* numero binutils: ver 'make verify'       */
#define P_RISCV_CALL        18
#define P_RISCV_CALL_PLT    19
#define P_RISCV_HI20        26
#define P_RISCV_LO12_I      27

#define MAX_SECC            48          /* secciones ALLOC + tablas de relocacion.
                                           Con -ffunction-sections cada funcion del
                                           app es su .text.X y su .rela.text.X: el
                                           selftest de 10 funciones llega a 35.  */
#define MAX_SIMB            192
#define MAX_EXPORTS         16

/* ---- la interfaz del kernel, resuelta por nombre como en elfrun --------------
 * Aqui esta la diferencia con emu4: sin esta tabla el app no tiene ni printf.
 * Son punteros a funciones REALES del kernel, enlazados contra el arbol de
 * include/; no hay trampolines porque en RV32 el call PC-relativo alcanza. */
struct export
{
    const char *name;
    void       *addr;
};

static int eapp_codigo = -1;

static void eapp_exit(int codigo)
{
    eapp_codigo = codigo;               /* devuelve: el _start hace 'ret' despues  */
}

static const struct export exports[MAX_EXPORTS] = {
    { "printf",         (void *)printf         },
    { "kprintf",        (void *)kprintf        },
    { "memset",         (void *)memset         },
    { "memcpy",         (void *)memcpy         },
    { "memmove",        (void *)memmove        },
    { "memcmp",         (void *)memcmp         },
    { "strlen",         (void *)strlen         },
    { "strcmp",         (void *)strcmp         },
    { "strncmp",        (void *)strncmp        },
    { "strcpy",         (void *)strcpy         },
    { "psram_malloc",   (void *)psram_malloc   },
    { "psram_free",     (void *)psram_free  },
    { "sleep",          (void *)sleep          },
    { "exit",           (void *)eapp_exit      },
};

/** Busqueda lineal por nombre: 14 entradas, no merece un hash. */
static void *resolve(const char *name)
{
    unsigned i;

    for (i = 0; i < MAX_EXPORTS && exports[i].name != NULL; i++)
    {
        if (strcmp(exports[i].name, name) == 0)
        {
            return exports[i].addr;
        }
    }
    return NULL;
}

/* ---- parches de inmediato, registro a registro -------------------------------
 * Los dos formatos de instruccion que tocan estas relocaciones:
 *
 *   U-type (auipc/lui) : opcode[6:0] rd[11:7] imm[31:12]
 *   I-type (addi/jalr) : opcode[6:0] rd[11:7] funct3[14:12] rs1[19:15] imm[31:20]
 *
 * Se reescribe SOLO el campo del inmediato; el resto de la palabra es la
 * instruccion que ya puso el compilador.  El +0x800 redondea para que el bit 12
 * que se lleva el lo12 con signo no doble la suma. */

static unsigned int patch_hi20(unsigned int instr, unsigned int valor)
{
    return (instr & 0x00000FFFU) | ((valor + 0x800U) & 0xFFFFF000U);
}

static unsigned int patch_lo12(unsigned int instr, unsigned int valor)
{
    return (instr & 0x000FFFFFU) | ((valor & 0xFFFU) << 20);
}

/* ---- el cargador ------------------------------------------------------------- */

shellcmd xsh_elfapp(int nargs, char *args[])
{
    const ELF32_hdr *hdr;
    const ELF32_shdr *sh, *shstr, *symtab, *strtabsec;
    const Elf32_Sym  *sym;
    const Elf32_Rela *rela;
    const char *shstrtab, *strtab;
    unsigned int base[MAX_SECC];        /* direccion directa de cada seccion ALLOC */
    unsigned int simval[MAX_SIMB];      /* direccion final de cada simbolo          */
    uintptr_t cursor, total, fin, i, j;
    uintptr_t cached, directo;
    unsigned int nsec, nsym, relocs = 0, parches = 0;
    unsigned int hi, lo, diff;
    const char *nombre;
    int (*entrada)(void);

    (void)nargs;
    (void)args;

    hdr = (const ELF32_hdr *)app_elf;

    /* 1. Cabecera: se comprueba TODO antes de tocar memoria. */
    if (hdr->e_ident[0] != 0x7FU || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L'  || hdr->e_ident[3] != 'F')
    {
        printf("elfapp: no es ELF (magic)\r\n");
        return SYSERR;
    }
    if (hdr->e_ident[4] != EC_32 || hdr->e_machine != EM_RISCV)
    {
        printf("elfapp: no es RV32 (class=%u machine=0x%x)\r\n",
               hdr->e_ident[4], (unsigned)hdr->e_machine);
        return SYSERR;
    }
    if (hdr->e_type != ET_REL)
    {
        printf("elfapp: e_type=%u, hace falta ET_REL (ld -r).  Si enlazas "
               "completo no queda .symtab que resolver.\r\n",
               (unsigned)hdr->e_type);
        return SYSERR;
    }

    nsec = hdr->e_shnum;
    if (nsec > MAX_SECC)
    {
        printf("elfapp: %u secciones, sube MAX_SECC\r\n", nsec);
        return SYSERR;
    }

    sh     = (const ELF32_shdr *)((const unsigned char *)hdr + hdr->e_shoff);
    shstr  = &sh[hdr->e_shstrndx];
    shstrtab = (const char *)((const unsigned char *)hdr + shstr->sh_off);

    printf("elfapp: %u secciones, %u bytes de fichero\r\n", nsec, app_elf_len);

    /* 2. Colocar: primero se calcula el tamano total alineado, luego se reserva,
     *    y luego se asignan las direcciones.  Se reserva UNA vez porque el
     *    layout tiene que ser el mismo en las dos pasadas. */
    total = 0;
    for (i = 0; i < nsec; i++)
    {
        uintptr_t al;

        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0)
        {
            base[i] = 0;
            continue;
        }
        al = (uintptr_t)sh[i].sh_addralign;
        if (al < ALINEA_MIN)
        {
            al = ALINEA_MIN;
        }
        total = (total + al - 1) & ~(al - 1);
        total += sh[i].sh_size;
    }
    /* Margen para que el align de la primera seccion no se coma el calculo. */
    total += 64U;

    /* psram_malloc devuelve (cabecera + 1), o sea el puntero DE USUARIO 8 bytes
     * despues del struct memblock; por eso se suelta con psram_free(), que hace el
     * block-- y usa block->length.  psram_freemem(cached, total) pediria la
     * CABECERA y la longitud real (total + 8): dado el puntero de usuario inserta
     * un nodo dentro del bloque y deja la cabecera huerfana, que es como el monton
     * se veia pisado y psram_malloc devolvia NULL habiendo 31 MB libres. */
    cached = (uintptr_t)psram_malloc(total);
    if (cached == 0U)
    {
        printf("elfapp: psram_malloc(%lu) fallo\r\n", (unsigned long)total);
        return SYSERR;
    }
    directo = cached + PSRAM_DIRECT;
    printf("elfapp: bloque cached=0x%08lx  directo=0x%08lx  tam=%lu\r\n",
           (unsigned long)cached, (unsigned long)directo, (unsigned long)total);

    cursor = directo;
    for (i = 0; i < nsec; i++)
    {
        uintptr_t al;

        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0)
        {
            base[i] = 0;
            continue;
        }
        al = (uintptr_t)sh[i].sh_addralign;
        if (al < ALINEA_MIN)
        {
            al = ALINEA_MIN;
        }
        cursor = (cursor + al - 1) & ~(al - 1);
        base[i] = (unsigned int)cursor;

        nombre = shstrtab + sh[i].sh_name;
        if (sh[i].sh_type == SHT_NOBITS)
        {
            /* .bss: no esta en el fichero, se pone a cero en la celda real. */
            memset((void *)(uintptr_t)base[i], 0, sh[i].sh_size);
        }
        else
        {
            /* Se copia POR LA VENTANA DIRECTA: es la parte que garantiza que lo
             * que se ejecuta despues es lo que hay en la celda. */
            memcpy((void *)(uintptr_t)base[i],
                   (const unsigned char *)hdr + sh[i].sh_off, sh[i].sh_size);
        }
        cursor += sh[i].sh_size;
        if (sh[i].sh_flags & SHF_EXEC)
        {
            printf("   [texto]  %-22s 0x%08x  %u bytes\r\n",
                   nombre, base[i], (unsigned)sh[i].sh_size);
        }
        else
        {
            printf("   [datos]  %-22s 0x%08x  %u bytes%s\r\n",
                   nombre, base[i], (unsigned)sh[i].sh_size,
                   (sh[i].sh_type == SHT_NOBITS) ? "  (a cero)" : "");
        }
    }
    fin = cursor;

    /* 3. Simbolos: los definidos valen base[seccion] + st_value; los UND se
     *    buscan por nombre en exports[].  Esta es la mitad que le falta al
     *    elf_execve() de emu4, y sin ella el app no tiene ni printf. */
    symtab    = NULL;
    strtabsec = NULL;
    for (i = 0; i < nsec; i++)
    {
        if (sh[i].sh_type == SHT_SYMTAB)
        {
            symtab    = &sh[i];
            strtabsec = &sh[sh[i].sh_link];
        }
    }
    if (NULL == symtab || NULL == strtabsec)
    {
        printf("elfapp: sin .symtab: no hay nada que resolver\r\n");
        psram_free((void *)cached);
        return SYSERR;
    }

    sym    = (const Elf32_Sym *)((const unsigned char *)hdr + symtab->sh_off);
    strtab = (const char *)((const unsigned char *)hdr + strtabsec->sh_off);
    nsym   = (unsigned int)(symtab->sh_size / sizeof(Elf32_Sym));
    if (nsym > MAX_SIMB)
    {
        printf("elfapp: %u simbolos, sube MAX_SIMB\r\n", nsym);
        psram_free((void *)cached);
        return SYSERR;
    }

    /* El indice 0 de .symtab es el simbolo nulo por convencion ELF:
     * st_name=0 y st_shndx=SHN_UNDEF.  NO es un externo que haya que resolver;
     * si se le deja entrar en el bucle, resolve("") devuelve NULL y el cargador
     * aborta con "simbolo SIN EXPORTAR: " (nombre vacio), que es exactamente lo
     * que salio en placa.  Se empieza en 1. */
    for (i = 1; i < nsym; i++)
    {
        simval[i] = 0U;
        if (sym[i].st_shndx == SHN_UNDEF)
        {
            void *d = resolve(strtab + sym[i].st_name);

            if (NULL == d)
            {
                printf("elfapp: simbolo SIN EXPORTAR: %s\r\n",
                       strtab + sym[i].st_name);
                psram_free((void *)cached);
                return SYSERR;
            }
            simval[i] = (unsigned int)(uintptr_t)d;
        }
        else if (sym[i].st_shndx == SHN_ABS)
        {
            /* p.ej. __global_pointer$: valor absoluto, sin seccion que sumar */
            simval[i] = (unsigned int)sym[i].st_value;
        }
        else if (sym[i].st_shndx < nsec && base[sym[i].st_shndx] != 0U)
        {
            simval[i] = base[sym[i].st_shndx] + (unsigned int)sym[i].st_value;
        }
    }

    /* 4. Relocaciones.  Los indices de 'make verify' sobre app.elf son la
     *    lista exacta de lo que hace falta: R_RISCV_CALL_PLT (19), HI20 (26) y
     *    LO12_I (27).  Se consagra tambien R_RISCV_32 (1) y R_RISCV_CALL (18)
     *    porque salen del mismo ld con otras banderas; cualquier otro tipo se
     *    aborta.  Una relocacion sin aplicar deja 0x0000xxxx y eso es mcause=5
     *    o mcause=2, el fallo de siempre, asi que es mejor no saltar.  Se
     *    recorren TODAS las .rela.* y se cuenta lo que se parchea. */
    for (i = 0; i < nsec; i++)
    {
        unsigned int tgt;

        if (sh[i].sh_type != SHT_RELA)
        {
            continue;
        }
        tgt = (unsigned int)sh[i].sh_info;
        if (tgt >= nsec || base[tgt] == 0U)
        {
            nombre = shstrtab + sh[i].sh_name;
            printf("elfapp: %s no apunta a seccion colocada\r\n", nombre);
            psram_free((void *)cached);
            return SYSERR;
        }

        rela = (const Elf32_Rela *)((const unsigned char *)hdr + sh[i].sh_off);
        for (j = 0; j < sh[i].sh_size / sizeof(Elf32_Rela); j++)
        {
            unsigned int  sitio = base[tgt] + (unsigned int)rela[j].r_offset;
            unsigned int  s     = ELF32_R_SYM(rela[j].r_info);
            unsigned int  tipo  = ELF32_R_TYPE(rela[j].r_info);
            unsigned int  A     = (unsigned int)rela[j].r_addend;
            unsigned int  S     = (s < nsym) ? simval[s] : 0U;
            unsigned int *pal   = (unsigned int *)(uintptr_t)sitio;

            if (sitio + 4U > fin || (sitio & 3U) != 0U)
            {
                nombre = shstrtab + sh[tgt].sh_name;
                printf("elfapp: relocacion %u fuera de %s (0x%08x)\r\n",
                       relocs, nombre, sitio);
                psram_free((void *)cached);
                return SYSERR;
            }

            relocs++;
            switch (tipo)
            {
                case P_RISCV_HI20:
                    /* S+A absoluto: los 20 bits altos van en bits 31:12 de la
                     * auipc/lui que ya escribio el compilador. */
                    *pal = patch_hi20(*pal, S + A);
                    parches++;
                    break;

                case P_RISCV_LO12_I:
                    /* La parte baja, con signo, en el immediate I-type.  El
                     * redondeo del HI20 (-0x800 en el arrastre) ya esta hecho
                     * por el propio +0x800, aqui solo se colocan los 12 bits. */
                    *pal = patch_lo12(*pal, S + A);
                    parches++;
                    break;

                case P_RISCV_32:
                    /* Datos: una palabra con el valor absoluto. */
                    *pal = S + A;
                    parches++;
                    break;

                case P_RISCV_CALL:
                case P_RISCV_CALL_PLT:
                    /* d = (S + A) - P, con P = la propia auipc.  En RV32 la pareja
                     * cubre cualquier int32, pero 'cubre' con aritmetica modular: si
                     * las dos direcciones estan en mitades opuestas del espacio, la
                     * resta envuelve.  Por eso, en vez de confiar en un rango
                     * teorico, se parchea y DESPUES se recalcula el destino con las
                     * dos instrucciones ya escritas: si no sale S+A, se aborta.  Es
                     * la unica comprobacion que no depende de mi aritmetica.
                     *
                     * lo = d - hi, con hi el valor ya redondeado que coloca
                     * patch_hi20; patch_lo12 se queda con los 12 bits de abajo y el
                     * signo sale solo al firmar la instruccion.  La jalr es la
                     * palabra siguiente: la pareja auipc+jalr es fija gracias a
                     * -mno-relax. */
                    diff = (unsigned int)((int)(S + A) - (int)sitio);
                    *pal = patch_hi20(*pal, diff);
                    hi   = (diff + 0x800U) & 0xFFFFF000U;
                    lo   = diff - hi;
                    *(unsigned int *)(uintptr_t)(sitio + 4U) =
                        patch_lo12(*(unsigned int *)(uintptr_t)(sitio + 4U), lo);

                    /* Verificacion: se releen las DOS instrucciones ya parcheadas,
                     * se suman sus inmediatos con signo sobre P y tiene que salir
                     * S+A.  Si no sale, la llamada no alcanza y se aborta antes de
                     * saltar. */
                    {
                        unsigned int w0 = *(unsigned int *)(uintptr_t)sitio;
                        unsigned int w1 = *(unsigned int *)(uintptr_t)(sitio + 4U);
                        long h = (long)(w0 & 0xFFFFF000U);      /* signo en bit31 */
                        long l = (long)(w1 >> 20);
                        l = (l ^ 0x800L) - 0x800L;              /* signo en bit11 */

                        if ((unsigned long)(sitio + (unsigned long)h +
                                            (unsigned long)l)
                            != (unsigned long)(S + A))
                        {
                            printf("elfapp: CALL no alcanza a %s: "
                                   "0x%08x -> 0x%08x\r\n",
                                   strtab + sym[s].st_name,
                                   (unsigned)sitio, (unsigned)(S + A));
                            psram_free((void *)cached);
                            return SYSERR;
                        }
                    }
                    parches += 2;
                    break;

                default:
                    nombre = shstrtab + sh[tgt].sh_name;
                    printf("elfapp: relocacion %u (tipo %u) NO SOPORTADA en %s\r\n",
                           relocs, tipo, nombre);
                    psram_free((void *)cached);
                    return SYSERR;
            }
        }
    }

    printf("elfapp: %u relocaciones leidas, %u campos parcheados\r\n",
           relocs, parches);

    /* 5. Saltar.  _start es el simbolo que busca el cargador; no se usa
     *    e_entry porque en ET_REL vale 0.  La direccion ya es de la ventana
     *    DIRECTA (base[] se formo desde 'directo'), asi que la icache no tiene
     *    linea rancia que servir: es la ruta probada en psram-blob. */
    entrada = NULL;
    for (i = 0; i < nsym; i++)
    {
        if (sym[i].st_shndx != SHN_UNDEF &&
            strcmp(strtab + sym[i].st_name, "_start") == 0)
        {
            entrada = (int (*)(void))(uintptr_t)simval[i];
            break;
        }
    }
    if (NULL == entrada)
    {
        printf("elfapp: no hay _start definido\r\n");
        psram_free((void *)cached);
        return SYSERR;
    }

    printf("elfapp: saltando a _start=0x%08x por ventana DIRECTA\r\n",
           (unsigned int)(uintptr_t)entrada);

    /* SP no se toca: el app corre en el hilo de este comando, con su marco de
     * pila debajo.  'exit' del app es eapp_exit, que guarda el codigo y
     * devuelve, asi que el _start cae en su 'ret' y esta llamada termina sola. */
    {
        int devuelto = entrada();

        printf("elfapp: el app devolvio %lu; exit() dejo codigo=%d\r\n",
               (unsigned long)devuelto, eapp_codigo);
    }

    psram_free((void *)cached);

    return OK;
}
   