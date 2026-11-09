/*
 * native_fat.c — natives de esp32p4.FAT en el ESP32-P4 (FAT32 sobre fl_*).
 *
 * La API sigue el estilo de java.io (RandomAccessFile / File), adaptada a
 * esta VM: sin clase File (no hay class loader) se usa un handle entero
 * devuelto por open().  En el host lo implementa jvm/esp32p4_fat.c sobre
 * ./sdcard; aquí se apoya en fatfs/ (fl_fopen, fl_fread, ...).
 *
 * Rutas absolutas desde la raíz de la tarjeta, p.ej. "/data/a.txt".
 * Convención de pila: el ÚLTIMO argumento está en la cima.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "simple_jvm.h"
#include "java_lib.h"
#include <fat_filelib.h>

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#define FAT_MAXFD 16

static void *fd_tab[FAT_MAXFD];          /* 0 = libre; slot+1 = handle */

static void pop_text(StackFrame *stack, SimpleConstantPool *p, char *out, int max)
{
    jvm_pop_text(stack, p, out, max);
}

static int slot_of(int h)
{
    return (h > 0 && h <= FAT_MAXFD) ? (h - 1) : -1;
}

/* "." o "" -> raíz "/", para poder listar el directorio raíz. */
static const char *norm_path(const char *path, char *tmp, int max)
{
    if (path == 0 || path[0] == 0)
    {
        return "/";
    }
    if (strcmp(path, ".") == 0 || strcmp(path, "./") == 0)
    {
        return "/";
    }
    (void) tmp;
    (void) max;
    return path;
}

/* ---- RandomAccessFile --------------------------------------------- */

int esp32p4_fat_open(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256], mode[8];
    void *f;
    int i;

    (void) type;
    pop_text(stack, p, mode, sizeof(mode));        /* el último empujado */
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    f = fl_fopen(full, mode);
    if (f == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    for (i = 0; i < FAT_MAXFD; i++)
    {
        if (fd_tab[i] == 0)
        {
            fd_tab[i] = f;
            pushInt(stack, i + 1);
            return 0;
        }
    }
    fl_fclose(f);
    pushInt(stack, 0);
    return 0;
}

int esp32p4_fat_close(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    int s = slot_of(h);

    (void) p;
    (void) type;
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    fl_fclose(fd_tab[s]);
    fd_tab[s] = 0;
    pushInt(stack, 1);
    return 0;
}

int esp32p4_fat_read(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char tmp[512];
    int len, bufref, h, n, i, s;

    (void) p;
    (void) type;
    len = popInt(stack);
    bufref = popInt(stack);
    h = popInt(stack);
    s = slot_of(h);
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    if (len > (int) sizeof(tmp))
    {
        len = (int) sizeof(tmp);
    }
    if (len < 0)
    {
        len = 0;
    }
    n = fl_fread(tmp, 1, len, fd_tab[s]);
    if (n < 0)
    {
        n = 0;
    }
    for (i = 0; i < n; i++)
    {
        heap_array_store(bufref, i, (unsigned char) tmp[i]);
    }
    pushInt(stack, n);
    return 0;
}

int esp32p4_fat_write(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char tmp[512];
    int len, bufref, h, n, i, s;

    (void) p;
    (void) type;
    len = popInt(stack);
    bufref = popInt(stack);
    h = popInt(stack);
    s = slot_of(h);
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    if (len > (int) sizeof(tmp))
    {
        len = (int) sizeof(tmp);
    }
    if (len < 0)
    {
        len = 0;
    }
    for (i = 0; i < len; i++)
    {
        tmp[i] = (char) heap_array_load(bufref, i);
    }
    n = fl_fwrite(tmp, 1, len, fd_tab[s]);
    if (n < 0)
    {
        n = 0;
    }
    pushInt(stack, n);
    return 0;
}

int esp32p4_fat_seek(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int off = popInt(stack);
    int h = popInt(stack);
    int s = slot_of(h);

    (void) p;
    (void) type;
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    pushInt(stack, fl_fseek(fd_tab[s], (long) off, SEEK_SET) == 0 ? 1 : 0);
    return 0;
}

int esp32p4_fat_tell(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    int s = slot_of(h);

    (void) p;
    (void) type;
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, -1);
        return 0;
    }
    pushInt(stack, (int) fl_ftell(fd_tab[s]));
    return 0;
}

int esp32p4_fat_eof(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    int h = popInt(stack);
    int s = slot_of(h);

    (void) p;
    (void) type;
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 1);
        return 0;
    }
    pushInt(stack, fl_feof(fd_tab[s]) ? 1 : 0);
    return 0;
}

int esp32p4_fat_readLine(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char line[256];
    int h = popInt(stack);
    int s = slot_of(h);
    int n = 0, c;

    (void) p;
    (void) type;
    line[0] = 0;
    if (s >= 0 && fd_tab[s] != 0)
    {
        while (n < (int) sizeof(line) - 1)
        {
            c = fl_fgetc(fd_tab[s]);
            if (c < 0)
            {
                break;
            }
            if (c == '\n')
            {
                break;
            }
            if (c == '\r')
            {
                continue;
            }
            line[n++] = (char) c;
        }
        line[n] = 0;
    }
    pushObj(stack, heap_new_string(line));
    return 0;
}

int esp32p4_fat_writeString(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char text[512];
    int h, s, n;

    (void) type;
    pop_text(stack, p, text, sizeof(text));
    h = popInt(stack);
    s = slot_of(h);
    if (s < 0 || fd_tab[s] == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    n = (int) strlen(text);
    n = fl_fwrite(text, 1, n, fd_tab[s]);
    if (n < 0)
    {
        n = 0;
    }
    pushInt(stack, n);
    return 0;
}

/* ---- atajos por ruta ---------------------------------------------- */

int esp32p4_fat_readAll(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];
    void *f;
    char *buf;
    long sz;
    int n;

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    f = fl_fopen(full, "r");
    if (f == 0)
    {
        pushObj(stack, heap_new_string(""));
        return 0;
    }
    fl_fseek(f, 0, SEEK_END);
    sz = fl_ftell(f);
    fl_fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 65535)
    {
        sz = 65535;
    }
    buf = (char *) jvm_alloc((int) sz + 1);
    if (buf == 0)
    {
        fl_fclose(f);
        pushObj(stack, heap_new_string(""));
        return 0;
    }
    n = fl_fread(buf, 1, (int) sz, f);
    fl_fclose(f);
    if (n < 0)
    {
        n = 0;
    }
    buf[n] = 0;
    pushObj(stack, heap_new_string(buf));
    jvm_free(buf);
    return 0;
}

int esp32p4_fat_writeAll(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256], text[512];
    void *f;

    (void) type;
    pop_text(stack, p, text, sizeof(text));
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    f = fl_fopen(full, "w");
    if (f == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    fl_fwrite(text, 1, (int) strlen(text), f);
    fl_fclose(f);
    pushInt(stack, 1);
    return 0;
}

int esp32p4_fat_appendString(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256], text[512];
    void *f;

    (void) type;
    pop_text(stack, p, text, sizeof(text));
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    f = fl_fopen(full, "a");
    if (f == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    fl_fwrite(text, 1, (int) strlen(text), f);
    fl_fclose(f);
    pushInt(stack, 1);
    return 0;
}

/* ---- estilo File -------------------------------------------------- */

int esp32p4_fat_exists(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];
    void *f;

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    if (fl_is_dir(full))
    {
        pushInt(stack, 1);
        return 0;
    }
    f = fl_fopen(full, "r");
    if (f != 0)
    {
        fl_fclose(f);
        pushInt(stack, 1);
        return 0;
    }
    pushInt(stack, 0);
    return 0;
}

int esp32p4_fat_isDirectory(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));
    pushInt(stack, fl_is_dir(full) ? 1 : 0);
    return 0;
}

int esp32p4_fat_length(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];
    void *f;
    long sz;

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    f = fl_fopen(full, "r");
    if (f == 0)
    {
        pushInt(stack, 0);
        return 0;
    }
    fl_fseek(f, 0, SEEK_END);
    sz = fl_ftell(f);
    fl_fclose(f);
    pushInt(stack, (int) sz);
    return 0;
}

int esp32p4_fat_delete(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));
    pushInt(stack, fl_remove(full) ? 1 : 0);
    return 0;
}

int esp32p4_fat_mkdir(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));
    pushInt(stack, fl_createdirectory(full) ? 1 : 0);
    return 0;
}

int esp32p4_fat_rmdir(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));
    pushInt(stack, fl_removedirectory(full) ? 1 : 0);
    return 0;
}

int esp32p4_fat_list(StackFrame *stack, SimpleConstantPool *p, char *type)
{
    char path[256], full[256];
    FL_DIR dir;
    fl_dirent e;
    int n = 0, i, arr;

    (void) type;
    pop_text(stack, p, path, sizeof(path));
    strlcpy(full, norm_path(path, full, sizeof(full)), sizeof(full));

    if (fl_opendir(full, &dir) == 0)
    {
        pushObj(stack, heap_new_ref_array(0));
        return 0;
    }
    while (fl_readdir(&dir, &e) == 0)
    {
        n++;
    }
    fl_closedir(&dir);

    arr = heap_new_ref_array(n);
    i = 0;
    if (fl_opendir(full, &dir) != 0)
    {
        while (fl_readdir(&dir, &e) == 0 && i < n)
        {
            heap_ref_array_store(arr, i, heap_new_string(e.filename),
                                 STACK_ENTRY_OBJ);
            i++;
        }
        fl_closedir(&dir);
    }
    pushObj(stack, arr);
    return 0;
}
