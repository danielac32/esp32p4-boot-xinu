/**
 * @file exports.h
 *
 * Interfaz de la tabla de simbolos que el kernel regala a las apps ELF.
 *
 * Quien la usa: el enlazador del cargador (shell/xsh_elfapp.c hoy y
 * system/elfload.c despues).  Un app en C se compila con 'ld -r', queda con sus
 * llamadas sin resolver en .rela.text.* y .symtab, y el cargador busca cada
 * nombre UND aqui para saber donde apunta.  La lista de nombres y por que hay
 * unos si y otros no esta comentada en system/exports.c; la version para el que
 * escribe apps esta en examples/p4-elfsdk/README.md.
 *
 * Ojo con lo que NO es esta tabla: no es una ABI versionada ni un saltador de
 * trampolines.  Son punteros directos a funciones del kernel y el app salta a
 * ellos en modo supervisor, con la pila de la shell.  Cuando haya que soportar
 * kernel y app compilados por separado con garantia de compatibilidad, esto se
 * convierte en la 'struct api' (api_v1) que se pidio en su dia; mientras, el
 * binario del app y el del kernel tienen que salir del MISMO arbol.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _EXPORTS_H_
#define _EXPORTS_H_

/** Una entrada: el nombre con el que el app llama, y la direccion del kernel. */
struct export
{
    const char *name;
    const void *addr;
};

/**
 * Direccion de 'name', o NULL si no esta exportado.
 *
 * El argumento puede ser la cadena del .strtab del app, que vive en PSRAM y ya
 * esta copiada y relocada cuando se llama.  Nombre vacio (el simbolo nulo de
 * .symtab) devuelve NULL en vez de buscar.
 */
void *export_resolve(const char *name);

/** Numero de entradas de la tabla. */
unsigned int export_count(void);

/** Nombre de la entrada i, o NULL si i se pasa.  Solo para volcados de shell. */
const char *export_name(unsigned int i);

#endif                          /* _EXPORTS_H_ */
