/*
 * time.h — API de tiempo mínima de Xinu (la libc de Xinu no trae time.h).
 *
 * En RV32 time_t son 32 bits.  La implementación vive en fatfs/disk.c
 * (época base + uptime); sólo la usa la lib FAT.
 */
#ifndef TIME_H_INCLUDED
#define TIME_H_INCLUDED

typedef long time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *t);
struct tm *localtime(const time_t *t);

#endif                          /* TIME_H_INCLUDED */
