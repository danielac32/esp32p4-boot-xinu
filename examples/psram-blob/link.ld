/*
 * link.ld -- guiado minima para producir un BLOB PLANO.
 *
 * Pone .text en la direccion 0 para que objcopy -O binary vuelque el codigo
 * desde el primer byte, sin cabecera ELF y sin relleno.  Si enlazas en otra
 * direccion (por ejemplo 0x48000000), objcopy te crea un hueco de 1,2 GB de
 * ceros antes del codigo: ese es el primer error clasico de este proceso.
 *
 * Solo .text: una seccion .data aca romperia la premisa de "el blob se toca
 * solo con sus argumentos".
 */

/* Sin ENTRY(): vale para cualquier blob.  Lo que importa aqui no es la
 * direccion de entrada del ELF (se ignora al volcar), sino que la funcion
 * marcada con .text.entry caiga la primera en .text, y eso lo garantiza el
 * orden de las reglas de abajo. */

SECTIONS
{
    . = 0;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    /DISCARD/ : {
        *(.comment .note .eh_frame .rodata .data .bss)
    }
}
