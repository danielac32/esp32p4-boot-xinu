#ifndef _ELFLOAD_H_
#define _ELFLOAD_H_

#include <stddef.h>
#include <stdint.h>

/* Carga un ELF32 (RISC-V, ET_EXEC o ET_DYN/PIE) desde la FAT.
 * Devuelve el entry point (base + e_entry) o 0 en error.
 * *base_out recibe la dirección base del bloque alocado en PSRAM
 * (para liberar después), *size_out recibe el tamaño total. */
void *elf_load(const char *path, uint32_t *base_out, uint32_t *size_out);

#endif /* _ELFLOAD_H_ */