/**
 * @file elfload.c
 *
 * El cargador de apps ELF del kernel, en una funcion publica: elf_run().
 * Su unico cliente de shell es 'elf' (shell/xsh_elf.c): saca el ELF de la
 * microSD, lo coloca en PSRAM, resuelve sus simbolos contra system/exports.c,
 * le parchea las relocaciones y salta a su '_start' con argc/argv en la hebra
 * que llama.  El app ve un 'int main(int argc, char *argv[])' corriente, y su
 * 'exit' es elf_exit(): kill(gettid()), y es kill() del kernel quien devuelve
 * el bloque de PSRAM al heap (los campos elf_img/elf_size de la hebra).  Si el
 * app volviera sin llamar a exit --un _start propio--, elf_run() suelta el
 * bloque igual antes de devolver.
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
 *      se buscan por nombre en system/exports.c (printf, memset, psram_malloc,
 *      las ~1.000 de LVGL...).  Sin esta tabla el app no tiene ni printf.
 *   4. Aplica relocaciones.  Los tipos que salen de 'ld -r' con las banderas del
 *      SDK son cinco y los cinco son inmediatos de 32 bits, no datos:
 *        R_RISCV_32        datos: una palabra absoluta
 *        R_RISCV_HI20      auipc/lui   -> bits 31:12 = (S+A+0x800)>>12
 *        R_RISCV_LO12_I    addi/i-type -> bits 31:20 = (S+A) & 0xFFF
 *        R_RISCV_CALL(_PLT) auipc+jalr (par) -> d = (S+A)-P, con P = la propia
 *                                        instruccion auipc; hi = (d+0x800)>>12,
 *                                        lo = d - (hi<<12)
 *      CUALQUIER otro tipo aborta ANTES de saltar: una relocacion sin aplicar
 *      apunta a 0x0000xxxx y eso es mcause=5 o mcause=2, el fallo de siempre.
 *   5. Salta por la ventana DIRECTA (0x88......), y por eso ANTES de copiar
 *      hace una operacion de cache sobre el bloque reservado: writeback +
 *      invalidate de L1-D y L2 sobre [cached, cached+total).  Ejecutar por 0x88
 *      esquiva la icache, pero NO esquiva el write-back diferido: el kernel deja
 *      lineas SUCIAS de esas mismas celdas (la cabecera que escribe
 *      psram_malloc, y todo lo que ese bloque haya contenido antes de
 *      reasignarse, que nunca se borra).  Esas lineas se vacian CUANDO QUIERE el
 *      controlador de cache, y al vaciarse escriben por encima de lo que nosotros
 *      pusimos por 0x88.  MEDIDO en placa (#1639): el codigo del app estaba
 *      correcto al cargar y 0,1 s despues, y 2,7 s despues era 0xaaaaa...; la
 *      excepcion caia en la propia coda de crt0.  Con el writeback+invalidate
 *      previo ya no queda ninguna linea sucia que pueda escribir atras.
 *      Los DATOS del app tambien quedan en 0x88 (los parches HI20/LO12 llevan la
 *      base directa), asi que el app ve celdas reales, no lineas rancias.
 *
 * ALCANCE DE LAS LLAMADAS (por que no hacen falta trampolines): en RV32 la pareja
 * auipc+jalr cubre la diferencia con signo de 32 bits, o sea +-2 GiB.  Desde
 * PSRAM directa (0x88000000) al texto del kernel (0x40000000) hay 0x48000000 =
 * 1,21 G, dentro de rango.  Se comprueba de verdad en cada relocacion CALL: se
 * parchea y DESPUES se recalcula el destino con las dos instrucciones ya
 * escritas; si no sale S+A, aborta.  Es la unica comprobacion que no depende de
 * la aritmetica de este fichero.
 *
 * LA TABLA DE SIMBOLOS no es de este fichero: vive en system/exports.c
 * (include/exports.h).  Que el kernel la nombre es lo que mantiene esas
 * funciones dentro del ELF: con --gc-sections una funcion a la que nadie
 * referencia NO existe en la imagen, asi que la tabla define la ABI en vez de
 * describirla.  'exit' sigue siendo local (elf_exit aqui), porque no es del
 * kernel sino la trampa de retorno del app.
 *
 * LA PILA: el app corre en la hebra que llama a elf_run(), asi que SP ya es
 * valido y el marco del llamador queda debajo (por eso start.S solo salva ra y
 * s0).  printf funciona porque esa hebra tiene su tty abierto; una hebra sin
 * console escribe en NULL, y ahi hay que usar kprintf.
 *
 * argc/argv: elf_run deja la ruta en argv[0] del app, detras van los argumentos
 * del llamador, y el start.S llama a 'main' sin tocar a0/a1, asi que el app es
 * C estandar.  Nada del cargador depende de la firma del app: '_start' es un
 * simbolo mas y las relocaciones ya son absolutas.
 *
 * EL EXIT: 'exit' no es del kernel, es elf_exit() de aqui.  Imprime el codigo
 * y mata la hebra que corre el app con kill(gettid()); como elf_run() le dejo
 * a la hebra los campos elf_img/elf_size/elf_task, el kill() del kernel suelta
 * el bloque de PSRAM antes de irse.  kill() de la propia hebra no devuelve,
 * asi que el '_start' del app no vuelve a ejecutar ni una instruccion mas: sin
 * setjmp y sin segundo punto de salida.  El modelo es de UN nivel: si un app
 * lanza a otro con elf_run() y el de dentro llama a exit(), la hebra entera
 * muere y el bloque del app de fuera queda reservado.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <fat.h>
#include <elf.h>                       /* ELF32_hdr/shdr, Elf32_Sym/Rela        */
#include <exports.h>                   /* la tabla del kernel                   */
#include <elfload.h>
#include <fsutil.h>                   /* fs_mounted(): el FAT de la microSD    */
#include <fat_filelib.h>               /* fl_* para leer el ELF de la microSD   */
#include <soc/cache_struct.h>          /* CACHE.sync_* (operaciones de cache)   */

/** Ventana directa de PSRAM: 0x48...... + 0x40000000 = 0x88...... (mismas celdas). */
#define PSRAM_DIRECT        0x40000000U

/** Alineacion minima de una seccion colocada: 4.  Con 2 basta para instrucciones
 *  comprimidas, pero los parches escriben palabras completas, asi que se exige 4
 *  en las DOS pasadas (tamano y direccion) para que el calculo no se desfase. */
#define ALINEA_MIN          4U

#define ET_REL              1           /* objeto relocalizable: lo que hace ld -r  */
#define SHN_UNDEF           0
#define SHN_ABS             0xfff1      /* p.ej. __global_pointer$                  */

#define P_RISCV_32          1           /* numero binutils                          */
#define P_RISCV_CALL        18
#define P_RISCV_CALL_PLT    19
#define P_RISCV_HI20        26
#define P_RISCV_LO12_I      27

/** Secciones que se detallan en el volcado; mas de ese numero, solo censo.  Una
 *  app con -ffunction-sections tiene una .text.X por funcion y llena la consola. */
#define ELF_DETALLE_MAX     12

/* ---- coherencia entre las dos ventanas de la PSRAM ----------------------------
 * El P4 responde a las MISMAS celdas de PSRAM por dos direcciones: 0x48......
 * pasa por L1-D y L2 (write-back) y 0x88...... va recto a la celda.  Escribir
 * por 0x88 llega a la celda, pero no invalida nada: si el kernel tenia ahi una
 * linea SUCIA de ese bloque, la vacia cuando el controlador de cache decide, y
 * al vaciarla escribe lo viejo POR ENCIMA de nuestro codigo.
 *
 * La operacion explicita se hace con CACHE.sync_* (la receta esta medida en
 * examples/psram-blob/xsh_runblob.c): sync_map dice QUE caches se sincronizan
 * ([4]=L1-DCache, [5]=L2-Cache), sync_addr/sync_size el rango --en la ventana
 * CON cache, que es la direccion que vio el kernel, y redondeado a lineas de 64
 * bytes-- y sync_ctrl dispara la operacion.  Los envoltorios de
 * include/hal/cache_ll.h no sirven aqui: delegan en simbolos del ROM
 * (Cache_WriteBack_Invalidate_Addr) y este arbol no trae esa tabla.
 *
 * El bit de la operacion se limpia solo al terminar ('R/W/SC'), asi que esa es
 * la senal de fin; sync_done ([4]) es el aviso del hardware para una espera por
 * interrupcion. */

#define LINEA_CACHE         64U         /* tamano de linea de L1-D y de L2      */
#define SYNC_MAP_L1D        0x10U       /* bit [4] de sync_map: L1-DCache       */
#define SYNC_MAP_L2         0x20U       /* bit [5] de sync_map: L2-Cache        */
#define SYNC_INVAL          0x01U       /* sync_ctrl.invalidate_ena             */
#define SYNC_WBINVAL        0x08U       /* sync_ctrl.writeback_invalidate_ena   */

/** Vacia y descarta (op=SYNC_WBINVAL) o solo descarta (op=SYNC_INVAL) las
 *  lineas de L1-D y L2 que cubren el rango de la ventana con cache. */
static void elf_cache_sync(uintptr_t addr, uintptr_t size, uint32_t op)
{
    uintptr_t fin;
    unsigned  intentos;

    if (0U == size)
    {
        return;
    }

    /* Redondeo HACIA ABAJO: el bloque puede empezar en mitad de una linea que
     * comparte con la cabecera que psram_malloc escribio por 0x48, y esa es
     * justo la que hay que vaciar. */
    addr &= ~(uintptr_t)(LINEA_CACHE - 1U);
    fin = (addr + size + (LINEA_CACHE - 1U)) & ~(uintptr_t)(LINEA_CACHE - 1U);

    CACHE.sync_map.val  = SYNC_MAP_L1D | SYNC_MAP_L2;
    CACHE.sync_addr.val = (uint32_t)addr;
    CACHE.sync_size.val = (uint32_t)(fin - addr);
    CACHE.sync_ctrl.val = op;

    for (intentos = 0U; intentos < 5000000U; intentos++)
    {
        if (0U == (CACHE.sync_ctrl.val & op))
        {
            return;
        }
    }
    printf("elfload: AVISO: operacion de cache 0x%x sobre 0x%08lx no termino\r\n",
           (unsigned)op, (unsigned long)addr);
}

/* ---- una imagen cargada, y cual de ellas responde a 'exit' ------------------ */

/**
 * Una imagen cargada.  Es estado local de elf_run(): no hay registro de imagenes
 * vivas ni anidamiento con hebras -- el app corre en la hebra que llama y su
 * bloque se suelta antes de volver.  'elf_actual' es el unico global y solo
 * apunta a la imagen que esta corriendo AHORA, para que elf_exit() y el volcado
 * de traps sepan a quien dirigirse.
 */
struct elfimg
{
    void        *bloque;        /* psram_malloc del app, puntero CACHED       */
    unsigned long tam;          /* bytes de secciones colocadas               */
    unsigned int  entrada;      /* '_start' en DIRECTA; firma int (int, char**) */
    unsigned int  texto;        /* bytes de secciones SHF_EXEC                */
    unsigned int  datos;        /* bytes de secciones de datos (NOBITS cuenta)*/
    char          nombre[ELF_MAX_NOMBRE];
};

/** La imagen que corre ahora mismo, o NULL.  Solo la usa elf_exit() y la
 *  atribucion de traps; el bloque lo suelta kill(), no este fichero. */
static struct elfimg *elf_actual;

static void elf_exit(int codigo)
{
    printf("elf: exit(%d)\r\n", codigo);
    elf_actual = NULL;
    kill(gettid());                     /* libera la PSRAM (elf_img) y no vuelve */
    for (;;)
    {
        ;                               /* por si kill() de uno mismo regresara   */
    }
}

/** Simbolo del app -> direccion: la tabla del kernel y, para 'exit', la trampa. */
static void *elf_resolve(const char *name)
{
    if (strcmp(name, "exit") == 0)
    {
        return (void *)elf_exit;
    }
    return export_resolve(name);
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

static unsigned int elf_patch_hi20(unsigned int instr, unsigned int valor)
{
    return (instr & 0x00000FFFU) | ((valor + 0x800U) & 0xFFFFF000U);
}

static unsigned int elf_patch_lo12(unsigned int instr, unsigned int valor)
{
    return (instr & 0x000FFFFFU) | ((valor & 0xFFFU) << 20);
}

/** Soltar lo que se haya llegado a reservar en un elf_load() que falla. */
static void elf_fracaso(struct elfimg *img, unsigned int *tablas, uintptr_t cached)
{
    if (NULL != tablas)
    {
        psram_free(tablas);
    }
    if (0U != cached)
    {
        psram_free((void *)cached);
    }
    if (NULL != img)
    {
        img->bloque = NULL;
        img->entrada = 0U;
        img->tam = 0UL;
    }
}

/* ---- colocar + enlazar + parchear -------------------------------------------- */

static int elf_load(const void *imagen, unsigned long len, const char *etiqueta,
                    struct elfimg *img)
{
    const ELF32_hdr *hdr;
    const ELF32_shdr *sh, *shstr, *symtab, *strtabsec;
    const Elf32_Sym  *sym;
    const Elf32_Rela *rela;
    const char *shstrtab, *strtab;
    unsigned int *tablas = NULL;        /* base[] y simval[], en PSRAM          */
    unsigned int *base, *simval;
    uintptr_t cursor, total, fin, i, j;
    uintptr_t cached, directo, inicio, reserva;
    unsigned int nsec, nsym, relocs = 0, parches = 0;
    unsigned int hi, lo, diff, ntexto = 0, ndatos = 0;
    unsigned int bytes_texto = 0U, bytes_datos = 0U;
    const char *nombre;
    unsigned int start = 0U;            /* direccion de '_start', en DIRECTA    */

    if (NULL == imagen || NULL == img || 0UL == len)
    {
        return SYSERR;
    }

    img->bloque = NULL;
    snprintf(img->nombre, sizeof(img->nombre), "%s",
             (NULL != etiqueta) ? etiqueta : "elf");

    hdr = (const ELF32_hdr *)imagen;

    /* 1. Cabecera: se comprueba TODO antes de tocar memoria. */
    if (hdr->e_ident[0] != 0x7FU || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L'  || hdr->e_ident[3] != 'F')
    {
        printf("elfload: %s no es ELF (magic)\r\n", img->nombre);
        return SYSERR;
    }
    if (hdr->e_ident[4] != EC_32 || hdr->e_machine != EM_RISCV)
    {
        printf("elfload: %s no es RV32 (class=%u machine=0x%x)\r\n",
               img->nombre, hdr->e_ident[4], (unsigned)hdr->e_machine);
        return SYSERR;
    }
    if (hdr->e_type != ET_REL)
    {
        printf("elfload: %s e_type=%u, hace falta ET_REL (ld -r).  Si enlazas "
               "completo no queda .symtab que resolver.\r\n",
               img->nombre, (unsigned)hdr->e_type);
        return SYSERR;
    }

    nsec = hdr->e_shnum;
    if (0U == nsec || nsec > ELF_MAX_SECC)
    {
        printf("elfload: %s tiene %u secciones (tope %u)\r\n",
               img->nombre, nsec, (unsigned)ELF_MAX_SECC);
        return SYSERR;
    }

    sh     = (const ELF32_shdr *)((const unsigned char *)hdr + hdr->e_shoff);
    shstr  = &sh[hdr->e_shstrndx];
    shstrtab = (const char *)((const unsigned char *)hdr + shstr->sh_off);

    /* 2. Colocar: primero el tamano total alineado, luego la reserva, luego las
     *    direcciones.  Se reserva UNA vez porque el layout tiene que ser el mismo
     *    en las dos pasadas. */
    total = 0;
    for (i = 0; i < nsec; i++)
    {
        uintptr_t al;

        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0)
        {
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
     * se veia pisado y psram_malloc devolvia NULL habiendo 31 MB libres.
     *
     * Se reserva con DOS LINEAS DE CACHE de sobra (128 B) y el codigo no empieza
     * en `cached` sino en la siguiente frontera de 64.  Motivo: la linea de cache
     * es de 64 bytes y se comparte.  La cabecera del memblock (los 8 bytes de
     * antes) vive en la MISMA linea que los primeros bytes del bloque, y el
     * allocator la escribe por la ventana CON cache (0x48) cada vez que se
     * reorganiza el heap; eso deja la linea sucia con la copia VIEJA de nuestro
     * codigo y el write-back la devuelve por encima cuando le da la gana.  Con
     * la banda de guarda ni nuestra cabecera ni la del bloque vecino caen dentro
     * de una linea que contenga codigo del app. */
    reserva = total + 2U * LINEA_CACHE;
    cached = (uintptr_t)psram_malloc(reserva);
    if (cached == 0U)
    {
        printf("elfload: %s necesita %lu bytes de PSRAM y no hay\r\n",
               img->nombre, (unsigned long)reserva);
        return SYSERR;
    }
    inicio = (cached + (LINEA_CACHE - 1U)) & ~(uintptr_t)(LINEA_CACHE - 1U);
    directo = inicio + PSRAM_DIRECT;

    /* Antes de escribir por 0x88 hay que vaciar el cache de ESAS celdas: si el
     * kernel dejo ahi una linea sucia --el uso que ese bloque tuvo en otra vida--
     * el controlador la vacia cuando quiere y tapa el codigo que acabamos de
     * copiar.  Es el trap en la coda de crt0 medido con la imagen #1639. */
    elf_cache_sync(inicio, total, SYNC_WBINVAL);

    /* base[] y simval[] tambien van en PSRAM: con -ffunction-sections una app de
     * 60 funciones pasa de 120 secciones y 700 simbolos, y 3 KB de arrays en la
     * pila del llamador no tienen gracia. */
    {
        symtab = NULL;
        for (i = 0; i < nsec; i++)
        {
            if (sh[i].sh_type == SHT_SYMTAB)
            {
                symtab = &sh[i];
            }
        }
        if (NULL == symtab)
        {
            printf("elfload: %s no tiene .symtab: no hay nada que resolver\r\n",
                   img->nombre);
            elf_fracaso(img, NULL, cached);
            return SYSERR;
        }
        strtabsec = &sh[symtab->sh_link];
        nsym = (unsigned int)(symtab->sh_size / sizeof(Elf32_Sym));
        if (0U == nsym || nsym > ELF_MAX_SIMB)
        {
            printf("elfload: %s tiene %u simbolos (tope %u)\r\n",
                   img->nombre, nsym, (unsigned)ELF_MAX_SIMB);
            elf_fracaso(img, NULL, cached);
            return SYSERR;
        }
        if (strtabsec->sh_type != SHT_STRTAB)
        {
            printf("elfload: %s: .symtab no apunta a un .strtab\r\n", img->nombre);
            elf_fracaso(img, NULL, cached);
            return SYSERR;
        }

        tablas = (unsigned int *)psram_malloc((nsec + nsym) * sizeof(unsigned int));
        if (NULL == tablas)
        {
            printf("elfload: %s: sin sitio para %u+%u indices\r\n",
                   img->nombre, nsec, nsym);
            elf_fracaso(img, NULL, cached);
            return SYSERR;
        }
    }
    base = tablas;
    simval = tablas + nsec;

    sym    = (const Elf32_Sym *)((const unsigned char *)hdr + symtab->sh_off);
    strtab = (const char *)((const unsigned char *)hdr + strtabsec->sh_off);

    printf("elfload: %s: %u secciones, %u simbolos, %lu bytes de fichero, "
           "bloque psram=0x%08lx codigo cached=0x%08lx directo=0x%08lx tam=%lu\r\n",
           img->nombre, nsec, nsym, len,
           (unsigned long)cached, (unsigned long)inicio,
           (unsigned long)directo, (unsigned long)total);

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
            ntexto++;
            bytes_texto += (unsigned int)sh[i].sh_size;
        }
        else
        {
            ndatos++;
            bytes_datos += (unsigned int)sh[i].sh_size;
        }
        if (i < ELF_DETALLE_MAX || (sh[i].sh_flags & SHF_EXEC) == 0)
        {
            nombre = shstrtab + sh[i].sh_name;
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
    }
    if (ntexto > ELF_DETALLE_MAX)
    {
        printf("   ... y %u secciones de texto mas (cada funcion es la suya "
               "con -ffunction-sections)\r\n", ntexto - ELF_DETALLE_MAX);
    }
    fin = cursor;

    /* 3. Simbolos: los definidos valen base[seccion] + st_value; los UND se
     *    buscan por nombre en la tabla del kernel.  Sin esto el app no tiene ni
     *    printf. */
    for (i = 1; i < nsym; i++)
    {
        simval[i] = 0U;
        if (sym[i].st_shndx == SHN_UNDEF)
        {
            void *d = elf_resolve(strtab + sym[i].st_name);

            if (NULL == d)
            {
                printf("elfload: %s: simbolo SIN EXPORTAR: %s\r\n",
                       img->nombre, strtab + sym[i].st_name);
                elf_fracaso(img, tablas, cached);
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

    /* 4. Relocaciones: los cinco tipos que produce 'ld -r' con las banderas del
     *    SDK; cualquier otro aborta antes de saltar.  Se recorren TODAS las
     *    .rela.* y se cuenta lo que se parchea. */
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
            printf("elfload: %s: %s no apunta a seccion colocada\r\n",
                   img->nombre, nombre);
            elf_fracaso(img, tablas, cached);
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
                printf("elfload: %s: relocacion %u fuera de %s (0x%08x)\r\n",
                       img->nombre, relocs, nombre, sitio);
                elf_fracaso(img, tablas, cached);
                return SYSERR;
            }

            relocs++;
            switch (tipo)
            {
                case P_RISCV_HI20:
                    /* S+A absoluto: los 20 bits altos van en bits 31:12 de la
                     * auipc/lui que ya escribio el compilador. */
                    *pal = elf_patch_hi20(*pal, S + A);
                    parches++;
                    break;

                case P_RISCV_LO12_I:
                    /* La parte baja, con signo, en el immediate I-type.  El
                     * redondeo del HI20 ya esta hecho por el propio +0x800. */
                    *pal = elf_patch_lo12(*pal, S + A);
                    parches++;
                    break;

                case P_RISCV_32:
                    /* Datos: una palabra con el valor absoluto. */
                    *pal = S + A;
                    parches++;
                    break;

                case P_RISCV_CALL:
                case P_RISCV_CALL_PLT:
                    /* d = (S + A) - P, con P = la propia auipc.  La jalr es la
                     * palabra siguiente: la pareja es fija gracias a -mno-relax. */
                    diff = (unsigned int)((int)(S + A) - (int)sitio);
                    *pal = elf_patch_hi20(*pal, diff);
                    hi   = (diff + 0x800U) & 0xFFFFF000U;
                    lo   = diff - hi;
                    *(unsigned int *)(uintptr_t)(sitio + 4U) =
                        elf_patch_lo12(*(unsigned int *)(uintptr_t)(sitio + 4U), lo);

                    /* Verificacion: se releen las DOS instrucciones ya parcheadas,
                     * se suman sus inmediatos con signo sobre P y tiene que salir
                     * S+A.  Si no sale, la llamada no alcanza y se aborta. */
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
                            printf("elfload: CALL no alcanza a %s: "
                                   "0x%08x -> 0x%08x\r\n",
                                   strtab + sym[s].st_name,
                                   (unsigned)sitio, (unsigned)(S + A));
                            elf_fracaso(img, tablas, cached);
                            return SYSERR;
                        }
                    }
                    parches += 2;
                    break;

                default:
                    nombre = shstrtab + sh[tgt].sh_name;
                    printf("elfload: %s: relocacion %u (tipo %u) NO SOPORTADA "
                           "en %s\r\n", img->nombre, relocs, tipo, nombre);
                    elf_fracaso(img, tablas, cached);
                    return SYSERR;
            }
        }
    }

    /* 5. Buscar '_start'.  No se usa e_entry porque en ET_REL vale 0.  Su firma
     *    real es la del crt0: int _start(int argc, char *argv[]); aqui solo se
     *    guarda la direccion. */
    for (i = 0; i < nsym; i++)
    {
        if (sym[i].st_shndx != SHN_UNDEF &&
            strcmp(strtab + sym[i].st_name, "_start") == 0)
        {
            start = simval[i];
            break;
        }
    }
    if (0U == start)
    {
        printf("elfload: %s no tiene _start definido (faltan crt0.o?)\r\n",
               img->nombre);
        elf_fracaso(img, tablas, cached);
        return SYSERR;
    }

    psram_free(tablas);                 /* los indices ya estan parcheados      */

    /* Segunda pasada, ahora solo invalidate: el copiado y el parcheo fueron por
     * 0x88, asi que el cache todavia podria tener lineas LIMPIAS y rancias de
     * estas celdas (de antes de vaciarlas).  Nadie del kernel lee el bloque del
     * app por 0x48, pero si alguien lo hiciera, lo que veria seria lo viejo. */
    elf_cache_sync(inicio, total, SYNC_INVAL);

    img->bloque = (void *)cached;
    img->tam = total;
    img->entrada = start;
    img->texto = bytes_texto;
    img->datos = bytes_datos;

    printf("elfload: %s: %u relocaciones, %u campos parcheados, %u secciones "
           "de texto (%u B) y %u de datos (%u B), _start=0x%08x por DIRECTA\r\n",
           img->nombre, relocs, parches, ntexto, bytes_texto, ndatos,
           bytes_datos, img->entrada);

    return OK;
}



/* ---- el cargador entero: de la microSD a la PSRAM libre ---------------------- */

int elf_run(const char *ruta, int argc, char *argv[])
{
    struct elfimg img;
    struct thrent *yo;
    char *v[ELF_MAX_ARGS + 1];
    unsigned char *buf;
    unsigned long len;
    void *f;
    int leido, n, i, rv;

    if (NULL == ruta)
    {
        return SYSERR;
    }
    if (!fs_mounted())
    {
        printf("elfload: no hay volumen FAT montado; usa `sd' antes\r\n");
        return SYSERR;
    }

    f = fl_fopen(ruta, "r");
    if (NULL == f)
    {
        printf("elfload: no se abre %s\r\n", ruta);
        return SYSERR;
    }

    if (0 != fl_fseek(f, 0L, SEEK_END))
    {
        fl_fclose(f);
        printf("elfload: %s: no se puede medir\r\n", ruta);
        return SYSERR;
    }
    len = (unsigned long)fl_ftell(f);
    fl_fseek(f, 0L, SEEK_SET);
    if (0UL == len || len > (16UL * 1024UL * 1024UL))
    {
        fl_fclose(f);
        printf("elfload: %s: tamano %lu imposible para un ELF de app\r\n",
               ruta, len);
        return SYSERR;
    }

    buf = (unsigned char *)psram_malloc(len);
    if (NULL == buf)
    {
        fl_fclose(f);
        printf("elfload: %s: no hay %lu bytes libres en PSRAM\r\n", ruta, len);
        return SYSERR;
    }

    /* Leer en trozos: cada fl_fread toma/suelta el monitor interno de FAT
     * (fl_attach_locks en fatinit.c). Con resched() entre trozos se permite
     * que mp3_leer intercale sus lecturas y el audio no se corte al cargar. */
    {
        unsigned long off = 0;
        leido = 0;
        while (off < len)
        {
            int pedir = (int)((len - off) > 32768UL ? 32768UL : (len - off));
            int r = fl_fread(buf + off, 1, pedir, f);
            if (r <= 0)
            {
                break;
            }
            off += (unsigned long)r;
            leido = (int)off;
            resched();
        }
    }
    fl_fclose(f);

    if ((unsigned long)leido != len)
    {
        psram_free(buf);
        printf("elfload: %s: leidos %d de %lu bytes\r\n", ruta, leido, len);
        return SYSERR;
    }

    if (SYSERR == elf_load(buf, len, ruta, &img))
    {
        psram_free(buf);
        return SYSERR;
    }
    psram_free(buf);

    /* El argv que ve el app empieza por la ruta; el 'argv' de este llamador son
     * los argumentos que vienen despues, igual que en la linea de la shell.  Se
     * copian solo los punteros; las cadenas siguen en la pila del llamador, que
     * esta viva mientras el app corre porque el marco de esta funcion queda
     * debajo del del app. */
    v[0] = (char *)ruta;
    n = 1;
    for (i = 0; i < argc && n < ELF_MAX_ARGS; i++)
    {
        v[n++] = argv[i];
    }
    v[n] = NULL;

    /* Ejecutar.  Se marcan los campos ELF de ESTA hebra: si el app llama a
     * exit(), elf_exit() hace kill(gettid()) y es kill() quien suelta el
     * bloque (system/kill.c).  Si el app volviera sin exit --un _start
     * propio--, aqui se desmarcan y se suelta igual. */
    yo = &thrtab[thrcurrent];
    yo->elf_task = TRUE;
    yo->elf_img  = (void *)((struct memblock *)img.bloque - 1);
    yo->elf_size = img.tam + 2U * LINEA_CACHE + sizeof(struct memblock);

    elf_actual = &img;

    /*printf("elfload: %s: argc=%d", img.nombre, n);
    for (i = 0; i < n; i++)
    {
        printf(" argv[%d]=\"%s\"", i, v[i]);
    }
    printf("\r\n");*/

    rv = ((int (*)(int, char *[]))(uintptr_t)img.entrada)(n, v);

    yo->elf_task = FALSE;
    yo->elf_img  = NULL;
    yo->elf_size = 0;
    elf_actual = NULL;

    psram_free(img.bloque);             /* el bloque del app vuelve al heap     */
    img.bloque = NULL;

    //printf("elfload: %s volvio sin exit(): devolvio %d; PSRAM libre\r\n",
     //      img.nombre, rv);

    return OK;
}

/* ---- a quien le pasa la cosa --------------------------------------------------
 * Sin esto, un trap dentro de un app ELF es un numero (`mepc=0x88......') y nada
 * en el arbol dice que ese numero es codigo del app.  La imagen que corre ahora
 * mismo ya tiene todo lo que hace falta --nombre y el bloque--, y de ahi salen
 * las DOS ventanas de la misma imagen: la CON cache (0x48......, la que vio
 * psram_malloc) y la DIRECTA (0x88......, la que se ejecuta).  'tam' cubre el
 * bloque entero, codigo y datos incluidos, asi que una direccion cae en una de
 * las dos o no es del app.
 *
 * Dos piezas: elf_quien() (¿de quien es esta direccion?) y elf_dump_trap() (el
 * censo); las llama system/arch/riscv/panic.c como simbolos debiles.  Van con
 * kprintf a proposito: printf() exige el tty de la hebra, y la hebra que peta
 * puede ser cualquier cosa. */

/** Primera linea del bloque por la ventana con cache (el 'inicio' del load). */
static uintptr_t elf_base(const struct elfimg *img)
{
    return ((uintptr_t)img->bloque + (LINEA_CACHE - 1U))
           & ~(uintptr_t)(LINEA_CACHE - 1U);
}

/** Nombre de la imagen que corre ahora mismo si 'dir' cae en su bloque, o NULL. */
const char *elf_quien(uintptr_t dir)
{
    const struct elfimg *img = elf_actual;
    uintptr_t cached, directo;

    if (NULL == img || NULL == img->bloque || 0UL == img->tam)
    {
        return NULL;
    }
    cached  = elf_base(img);
    directo = cached + PSRAM_DIRECT;
    if ((dir >= cached && dir < cached + img->tam) ||
        (dir >= directo && dir < directo + img->tam))
    {
        return img->nombre;
    }
    return NULL;
}

/** La imagen que corre ahora mismo, por la consola; es lo que llama el trap. */
void elf_dump_trap(void)
{
    const struct elfimg *img = elf_actual;
    uintptr_t cached, directo, fin;

    if (NULL == img || NULL == img->bloque || 0UL == img->tam)
    {
        return;
    }
    cached = elf_base(img);
    directo = cached + PSRAM_DIRECT;
    fin = directo + img->tam;
    kprintf("elf '%s' cached=0x%08lx codigo=0x%08lx..0x%08lx _start=0x%08lx\r\n",
            img->nombre, (unsigned long)cached,
            (unsigned long)directo, (unsigned long)fin,
            (unsigned long)img->entrada);
}
