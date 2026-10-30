/* syscall.h — llamadas al sistema (ecall) del kernel (p4-xinu).
 *
 * Convención de nombres:
 *   XINU_<NOMBRE>   número de llamada.  Es la ABI verbatim del árbol de
 *                   referencia (p4-sdk / kernel_tinyusb): NO reordenar ni
 *                   reasignar valores, rompería la compatibilidad.
 *   sys_<nombre>()  envoltorio C de una llamada (lo que usa el kernel).
 *
 * ABI RISC-V: a7 = número, a0-a5 = argumentos, a0 = valor de retorno.
 * El gate está en system/arch/riscv/trap.c (trap_handler -> ecall_handler
 * -> syscall_dispatch, con bounds-check); la tabla sys_call_table[] vive en
 * system/sysent.c.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include <trap.h>               /* trap_frame_t (argumento del dispatcher) */

/* Implementado en system/sysent.c; lo llama el gate de ecall. */
int syscall_dispatch(trap_frame_t *frame);

enum
{
    /* ----------------------------------------------------------- Inter-Thread */
    XINU_NULLPROCESS = 0,
    XINU_SEND,                  /* send(tid, msg) */
    XINU_RECEIVE,               /* receive(void) */
    XINU_RECEIVECLR,            /* recvclr(void) */
    XINU_RECEIVETIME,           /* recvtime(ms) */

    /* ------------------------------------------------------- Thread mgmt */
    XINU_CREATE,                /* create(proc, stk, prio, name, nargs, arg0, ...) */
    XINU_KILL,                  /* kill(tid) / exit */
    XINU_READY,                 /* ready(tid, resched) */
    XINU_SLEEP,                 /* sleep(ms) */
    XINU_UNSLEEP,               /* unsleep(tid) */
    XINU_YIELD,                 /* yield(void) */
    XINU_GETTID,                /* gettid(void) */
    XINU_GETPRIO,               /* getprio(tid) */
    XINU_RESCHED,               /* resched(void) */

    /* ---------------------------------------------------------- Devices */
    XINU_PUTC,                  /* putc(dev, c) */
    XINU_PUTS,                  /* puts(dev, str) */
    XINU_GETC,                  /* getc(dev) */
    XINU_GETS,                  /* gets(dev) */
    XINU_OPEN,                  /* open(dev) */
    XINU_CLOSE,                 /* close(dev) */
    XINU_READ,                  /* read(dev, buf, n) */
    XINU_WRITE,                 /* write(dev, buf, n) */
    XINU_CONTROL,               /* control(dev, cmd, arg1, arg2) */
    XINU_GETDEV,                /* getdev(name) */

    /* ----------------------------------------------------------- GPIO */
    XINU_PIN_MODE,              /* pin_mode(pin, dir) */
    XINU_PIN_SET,               /* pin_set(pin, val) */
    XINU_PIN_GET,               /* pin_get(pin) */

    /* ---------------------------------------------------------- Memory */
    XINU_MALLOC,                /* malloc(size) -> ptr (PSRAM) */
    XINU_FREE,                  /* free(ptr) */
    XINU_FREE_HEAP,             /* heap_free(void) */
    XINU_GETPID,                /* getpid(void) */

    /* ----------------------------------------------------------- FAT */
    XINU_FS_INIT,
    XINU_ATTACH_MEDIA,
    XINU_ATTACH_LOCKS,
    XINU_FOPEN,                 /* fopen(path, mode) */
    XINU_FSEEK,
    XINU_FREAD,                 /* fread(fd, buf, n) */
    XINU_FWRITE,                /* fwrite(fd, buf, n) */
    XINU_FCLOSE,                /* fclose(fd) */
    XINU_FTELL,
    XINU_FREWIN,
    XINU_TRUNCATE,
    XINU_SIZE,
    XINU_AVAILABLE,
    XINU_MKDIR,
    XINU_FEOF,
    XINU_OPENDIR,               /* opendir(path) */
    XINU_READDIR,               /* readdir(dir, entry) */
    XINU_REMOVE,
    XINU_RENAME,
    XINU_FORMAT,
    XINU_MOUNT,
    XINU_UNMOUNT,
    XINU_DISK_FREE,
    XINU_CLOSEDIR,
    XINU_CREATEDIRECTORY,
    XINU_IS_DIR,
    XINU_FFLUSH,
    XINU_FGETC,
    XINU_FGETCS,
    XINU_FPUTC,
    XINU_FPUTCS,
    XINU_FGETPOS,

    /* --------------------------------------------------------- LVGL/Touch */
    XINU_LVGL_INIT,             /* lvgl_init() */
    XINU_LVGL_SCREEN,           /* lvgl_screen() -> handle */
    XINU_LVGL_CLEAR,            /* lvgl_clear(color) */
    XINU_LVGL_LABEL,            /* lvgl_label(parent, text, x, y) -> handle */
    XINU_LVGL_BUTTON,           /* lvgl_button(parent, text, x, y, w, h) -> handle */
    XINU_LVGL_RECT,             /* lvgl_rect(parent, x, y, w, h, color) -> handle */
    XINU_LVGL_SET_TEXT,         /* lvgl_set_text(w, text) */
    XINU_LVGL_SET_TEXT_FMT,     /* lvgl_set_text_fmt(w, fmt, a, b) */
    XINU_LVGL_SET_POS,          /* lvgl_set_pos(w, x, y) */
    XINU_LVGL_SET_SIZE,         /* lvgl_set_size(w, w, h) */
    XINU_LVGL_SET_COLOR,        /* lvgl_set_color(w, fg, bg) */
    XINU_LVGL_TICK,             /* lvgl_tick() */
    XINU_LVGL_TASK,             /* lvgl_task() */
    XINU_LVGL_POLL_EVENT,       /* lvgl_poll_event() -> 1 si hay evento */
    XINU_LVGL_EVENT_WIDGET,     /* lvgl_event_widget() -> handle */
    XINU_LVGL_DEL,              /* lvgl_del(w) */
    XINU_LVGL_GET_WIDTH,        /* lvgl_get_width(w) */
    XINU_LVGL_GET_HEIGHT,       /* lvgl_get_height(w) */
    XINU_LVGL_SCREEN_WIDTH,     /* lvgl_screen_width() */
    XINU_LVGL_SCREEN_HEIGHT,    /* lvgl_screen_height() */
    XINU_LVGL_IMG,              /* lvgl_img(parent, path, x, y, w, h) -> handle */
    XINU_TOUCH_READ,            /* touch_read(point *) */

    /* --------------------------------------------------------- Network */
    XINU_NET_PING_START,        /* net_ping_start(host, count) */
    XINU_NET_PING_POLL,         /* net_ping_poll(out, cap) */
    XINU_NET_PING_DONE,         /* net_ping_done(void) */
    XINU_NET_HAS_IP,            /* net_has_ip(void) */
    XINU_NET_IP_STR,            /* net_ip_str(buf, len) */
    XINU_NET_IP_U32,            /* net_ip_u32(void) */

    /* --------------------------------------------------------- System */
    XINU_NUM_TASK,
    XINU_STRUCT_TASK,
    XINU_LOAD_ELF,
    XINU_GLOBAL_PATH,
    XINU_UPDATE_PATH,
    XINU_GET_PATH,
    XINU_CD,
    XINU_JSON,
    XINU_GET_LEN,
};

/* Último número + 1: tamaño de sys_call_table[] (sysent.c). */
#define NSYSCALL        (XINU_GET_LEN + 1)

/**
 * Gate crudo: fija a0-a5/a7, emite ecall y devuelve a0.  Usable tanto desde
 * código de kernel (para probar el gate) como desde código de usuario.
 */
static inline long sys_ecall(long nr, long arg0, long arg1, long arg2,
                             long arg3, long arg4, long arg5)
{
    register long r_a0 __asm__("a0") = arg0;
    register long r_a1 __asm__("a1") = arg1;
    register long r_a2 __asm__("a2") = arg2;
    register long r_a3 __asm__("a3") = arg3;
    register long r_a4 __asm__("a4") = arg4;
    register long r_a5 __asm__("a5") = arg5;
    register long r_a7 __asm__("a7") = nr;

    __asm__ volatile ("ecall"
                      : "+r" (r_a0)
                      : "r" (r_a1), "r" (r_a2), "r" (r_a3),
                        "r" (r_a4), "r" (r_a5), "r" (r_a7)
                      : "memory");
    return r_a0;
}

/* Servicios implementados (ver system/sysent.c). */
static inline long sys_getpid(void)
{
    return sys_ecall(XINU_GETPID, 0, 0, 0, 0, 0, 0);
}

static inline long sys_yield(void)
{
    return sys_ecall(XINU_YIELD, 0, 0, 0, 0, 0, 0);
}

static inline long sys_putc(int dev, char c)
{
    return sys_ecall(XINU_PUTC, dev, c, 0, 0, 0, 0);
}

static inline long sys_puts(int dev, const char *s)
{
    return sys_ecall(XINU_PUTS, dev, (long)s, 0, 0, 0, 0);
}

static inline long sys_getc(int dev)
{
    return sys_ecall(XINU_GETC, dev, 0, 0, 0, 0, 0);
}

static inline long sys_open(int dev)
{
    return sys_ecall(XINU_OPEN, dev, 0, 0, 0, 0, 0);
}

static inline long sys_close(int dev)
{
    return sys_ecall(XINU_CLOSE, dev, 0, 0, 0, 0, 0);
}

static inline long sys_read(int dev, void *buf, int n)
{
    return sys_ecall(XINU_READ, dev, (long)buf, n, 0, 0, 0);
}

static inline long sys_write(int dev, const void *buf, int n)
{
    return sys_ecall(XINU_WRITE, dev, (long)buf, n, 0, 0, 0);
}

static inline long sys_sleep(int ms)
{
    return sys_ecall(XINU_SLEEP, ms, 0, 0, 0, 0, 0);
}

static inline void *sys_malloc(int size)
{
    return (void *)sys_ecall(XINU_MALLOC, size, 0, 0, 0, 0, 0);
}

static inline long sys_free(void *ptr)
{
    return sys_ecall(XINU_FREE, (long)ptr, 0, 0, 0, 0, 0);
}

static inline void sys_exit(int code)
{
    sys_ecall(XINU_KILL, code, 0, 0, 0, 0, 0);
}

/* ---- FAT (ficheros en la SD) ---- */
static inline long sys_fopen(const char *path, const char *mode)
{
    return sys_ecall(XINU_FOPEN, (long)path, (long)mode, 0, 0, 0, 0);
}
static inline long sys_fclose(int fd)
{
    return sys_ecall(XINU_FCLOSE, fd, 0, 0, 0, 0, 0);
}
static inline long sys_fread(int fd, void *buf, int n)
{
    return sys_ecall(XINU_FREAD, fd, (long)buf, n, 0, 0, 0);
}
static inline long sys_fwrite(int fd, const void *buf, int n)
{
    return sys_ecall(XINU_FWRITE, fd, (long)buf, n, 0, 0, 0);
}
static inline long sys_fseek(int fd, int off, int whence)
{
    return sys_ecall(XINU_FSEEK, fd, off, whence, 0, 0, 0);
}
static inline long sys_ftell(int fd)
{
    return sys_ecall(XINU_FTELL, fd, 0, 0, 0, 0, 0);
}
static inline long sys_feof(int fd)
{
    return sys_ecall(XINU_FEOF, fd, 0, 0, 0, 0, 0);
}
static inline long sys_fgetc(int fd)
{
    return sys_ecall(XINU_FGETC, fd, 0, 0, 0, 0, 0);
}
static inline long sys_fputc(int fd, int c)
{
    return sys_ecall(XINU_FPUTC, fd, c, 0, 0, 0, 0);
}
static inline long sys_mkdir(const char *path)
{
    return sys_ecall(XINU_MKDIR, (long)path, 0, 0, 0, 0, 0);
}
static inline long sys_remove(const char *path)
{
    return sys_ecall(XINU_REMOVE, (long)path, 0, 0, 0, 0, 0);
}
static inline long sys_isdir(const char *path)
{
    return sys_ecall(XINU_IS_DIR, (long)path, 0, 0, 0, 0, 0);
}
static inline long sys_exists(const char *path)
{
    return sys_ecall(XINU_AVAILABLE, (long)path, 0, 0, 0, 0, 0);
}
static inline long sys_filesize(const char *path)
{
    return sys_ecall(XINU_SIZE, (long)path, 0, 0, 0, 0, 0);
}

/* ---- Directorios (FAT) ---- */
static inline long sys_opendir(const char *path)
{
    return sys_ecall(XINU_OPENDIR, (long)path, 0, 0, 0, 0, 0);
}
static inline long sys_readdir(int dir, void *entry)
{
    return sys_ecall(XINU_READDIR, dir, (long)entry, 0, 0, 0, 0);
}
static inline long sys_closedir(int dir)
{
    return sys_ecall(XINU_CLOSEDIR, dir, 0, 0, 0, 0, 0);
}
static inline long sys_fflush(int fd)
{
    return sys_ecall(XINU_FFLUSH, fd, 0, 0, 0, 0, 0);
}
static inline long sys_fgetpos(int fd, unsigned *pos)
{
    return sys_ecall(XINU_FGETPOS, fd, (long)pos, 0, 0, 0, 0);
}

/* ---- LVGL/Touch ---- */
static inline long sys_lvgl_init(void)
{
    return sys_ecall(XINU_LVGL_INIT, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_screen(void)
{
    return sys_ecall(XINU_LVGL_SCREEN, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_clear(int color)
{
    return sys_ecall(XINU_LVGL_CLEAR, color, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_label(int parent, const char *text, int x, int y)
{
    return sys_ecall(XINU_LVGL_LABEL, parent, (long)text, x, y, 0, 0);
}
static inline long sys_lvgl_button(int parent, const char *text, int x, int y, int w, int h)
{
    return sys_ecall(XINU_LVGL_BUTTON, parent, (long)text, x, y, w, h);
}
static inline long sys_lvgl_rect(int parent, int x, int y, int w, int h, int color)
{
    return sys_ecall(XINU_LVGL_RECT, parent, x, y, w, h, color);
}
static inline long sys_lvgl_set_text(int w, const char *text)
{
    return sys_ecall(XINU_LVGL_SET_TEXT, w, (long)text, 0, 0, 0, 0);
}
static inline long sys_lvgl_set_text_fmt(int w, const char *fmt, int a, int b)
{
    return sys_ecall(XINU_LVGL_SET_TEXT_FMT, w, (long)fmt, a, b, 0, 0);
}
static inline long sys_lvgl_set_pos(int w, int x, int y)
{
    return sys_ecall(XINU_LVGL_SET_POS, w, x, y, 0, 0, 0);
}
static inline long sys_lvgl_set_size(int w, int width, int height)
{
    return sys_ecall(XINU_LVGL_SET_SIZE, w, width, height, 0, 0, 0);
}
static inline long sys_lvgl_set_color(int w, int fg, int bg)
{
    return sys_ecall(XINU_LVGL_SET_COLOR, w, fg, bg, 0, 0, 0);
}
static inline long sys_lvgl_tick(void)
{
    return sys_ecall(XINU_LVGL_TICK, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_task(void)
{
    return sys_ecall(XINU_LVGL_TASK, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_poll_event(void)
{
    return sys_ecall(XINU_LVGL_POLL_EVENT, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_event_widget(void)
{
    return sys_ecall(XINU_LVGL_EVENT_WIDGET, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_del(int w)
{
    return sys_ecall(XINU_LVGL_DEL, w, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_get_width(int w)
{
    return sys_ecall(XINU_LVGL_GET_WIDTH, w, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_get_height(int w)
{
    return sys_ecall(XINU_LVGL_GET_HEIGHT, w, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_screen_width(void)
{
    return sys_ecall(XINU_LVGL_SCREEN_WIDTH, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_screen_height(void)
{
    return sys_ecall(XINU_LVGL_SCREEN_HEIGHT, 0, 0, 0, 0, 0, 0);
}
static inline long sys_lvgl_img(int parent, const char *path, int x, int y, int w, int h)
{
    return sys_ecall(XINU_LVGL_IMG, parent, (long)path, x, y, w, h);
}
static inline long sys_touch_read(void *point)
{
    return sys_ecall(XINU_TOUCH_READ, (long)point, 0, 0, 0, 0, 0);
}

/* ---- Network ---- */
static inline long sys_net_ping_start(const char *host, int count)
{
    return sys_ecall(XINU_NET_PING_START, (long)host, count, 0, 0, 0, 0);
}
static inline long sys_net_ping_poll(void *out, int cap)
{
    return sys_ecall(XINU_NET_PING_POLL, (long)out, cap, 0, 0, 0, 0);
}
static inline long sys_net_ping_done(void)
{
    return sys_ecall(XINU_NET_PING_DONE, 0, 0, 0, 0, 0, 0);
}
static inline long sys_net_has_ip(void)
{
    return sys_ecall(XINU_NET_HAS_IP, 0, 0, 0, 0, 0, 0);
}
static inline long sys_net_ip_str(char *buf, int len)
{
    return sys_ecall(XINU_NET_IP_STR, (long)buf, len, 0, 0, 0, 0);
}
static inline long sys_net_ip_u32(void)
{
    return sys_ecall(XINU_NET_IP_U32, 0, 0, 0, 0, 0, 0);
}

/* ---- Inter-Thread Communication ---- */
static inline long sys_send(tid_typ tid, int msg)
{
    return sys_ecall(XINU_SEND, (long)tid, msg, 0, 0, 0, 0);
}
static inline long sys_receive(void)
{
    return sys_ecall(XINU_RECEIVE, 0, 0, 0, 0, 0, 0);
}
static inline long sys_recvclr(void)
{
    return sys_ecall(XINU_RECEIVECLR, 0, 0, 0, 0, 0, 0);
}
static inline long sys_recvtime(int ms)
{
    return sys_ecall(XINU_RECEIVETIME, ms, 0, 0, 0, 0, 0);
}

/* ---- Thread management ---- */
static inline long sys_create(void *procaddr, uint ssize, int prio,
                              const char *name, int nargs, ...)
{
    return sys_ecall(XINU_CREATE, (long)procaddr, ssize, prio,
                     (long)name, nargs, 0);
}
static inline long sys_gettid(void)
{
    return sys_ecall(XINU_GETTID, 0, 0, 0, 0, 0, 0);
}
static inline long sys_getprio(tid_typ tid)
{
    return sys_ecall(XINU_GETPRIO, (long)tid, 0, 0, 0, 0, 0);
}
static inline long sys_ready(tid_typ tid, bool resched)
{
    return sys_ecall(XINU_READY, (long)tid, resched, 0, 0, 0, 0);
}
static inline long sys_resched(void)
{
    return sys_ecall(XINU_RESCHED, 0, 0, 0, 0, 0, 0);
}
static inline long sys_unsleep(tid_typ tid)
{
    return sys_ecall(XINU_UNSLEEP, (long)tid, 0, 0, 0, 0, 0);
}

/* ---- Dispositivos / divers ---- */
static inline long sys_getdev(const char *name)
{
    return sys_ecall(XINU_GETDEV, (long)name, 0, 0, 0, 0, 0);
}
static inline long sys_control(int dev, int cmd, long a1, long a2)
{
    return sys_ecall(XINU_CONTROL, dev, cmd, a1, a2, 0, 0);
}
static inline long sys_heap_free(void)
{
    return sys_ecall(XINU_FREE_HEAP, 0, 0, 0, 0, 0, 0);
}
static inline long sys_num_task(void)
{
    return sys_ecall(XINU_NUM_TASK, 0, 0, 0, 0, 0, 0);
}

#endif /* _SYSCALL_H_ */