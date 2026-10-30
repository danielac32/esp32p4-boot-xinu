/*
 * fatfs.h — API del pegamento FAT (fatfs/disk.c).
 *
 * La lib FAT32 se monta con fatfs_mount() (levanta la SD, lee el BPB y arma
 * los callbacks de sector).  El candado reentrante interno de la lib
 * (fl_attach_locks) lo instala el device `fat` del kernel —no la lib— porque
 * el candado es un MONITOR del kernel (dueño + profundidad); las librerías de
 * fatfs/ no crean monitores ni semáforos.
 */
#ifndef _FATFS_H_
#define _FATFS_H_

/* Levanta la SD (slot 0) y monta el volumen FAT.  0 = OK, <0 = error. */
int fatfs_mount(void);

/* Candado de la lib FAT: el device pasa sus dos envoltorios sobre el monitor.
 * Declarado aquí para no arrastrar fat_filelib.h (y sus macros) al device. */
void fl_attach_locks(void (*lock)(void), void (*unlock)(void));

#endif                          /* _FATFS_H_ */
