/**
 * @file fslock.h
 *
 * Mutex global del sistema de ficheros (FAT/microSD).  El FAT y el acceso al
 * volumen no son reentrantes: si dos hebras (p. ej. mp3_leer de musica y
 * elf_run/ls) tocan fl_* a la vez, se corrompe el estado y cuelga la placa.
 * Tomar este candado antes de operar con fl_* / el volumen FAT.
 */
#ifndef _FSLOCK_H_
#define _FSLOCK_H_

void fs_lock(void);
void fs_unlock(void);

#endif /* _FSLOCK_H_ */
