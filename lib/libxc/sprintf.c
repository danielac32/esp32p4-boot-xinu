/**
 * @file sprintf.c
 */
/* Embedded Xinu, Copyright (C) 2009, 2013.  All rights reserved. */

#include <stdarg.h>
#include <stdio.h>

static int sprntf(int, uintptr_t);
extern int _doprnt(const char *fmt, va_list ap,
                   int (*putc_func)(int, uintptr_t), uintptr_t putc_arg);

/**
 * @ingroup libxc
 *
 * Write a formatted message to a null-terminated string.
 *
 * @param str
 *      The output string, which must be large enough to store the formatted
 *      result, including the null terminator.
 * @param format
 *      The format string.  Not all standard conversion specifications are
 *      supported by this implementation.  See _doprnt() for a description of
 *      supported conversion specifications.
 * @param ...
 *      Arguments matching those in the format string.
 *
 * @return
 *      the number of characters written, excluding the null terminator.
 */
int sprintf(char *str, const char *format, ...)
{
    va_list ap;
    char *s;

    s = str;
    va_start(ap, format);
    _doprnt(format, ap, sprntf, (uintptr_t) &s);
    va_end(ap);
    *s = '\0';

    return s - str;
}

/*
 * Routine called by _doprnt() to output each character.
 */
static int sprntf(int c, uintptr_t _sptr)
{
    char **sptr = (char **)_sptr;
    char *s = *sptr;

    *s++ = c;
    *sptr = s;
    return (unsigned char)c;
}
/*
 * Estado del escritor acotado que usa snprintf(): cursor y primer byte que YA
 * no se escribe.  Si la trama no cabe, el caracter se cuenta pero no se guarda,
 * que es exactamente lo que pide C99: snprintf devuelve la longitud que la
 * cadena HABRIA tenido, y el buffer queda truncado con su terminador.
 */
struct snprint_state
{
    char *s;
    char *end;
};

static int snprntf(int c, uintptr_t arg)
{
    struct snprint_state *st = (struct snprint_state *)arg;

    if (st->s < st->end)
    {
        *st->s++ = (char)c;
    }
    return (unsigned char)c;
}

/**
 * @ingroup libxc
 *
 * Like sprintf(), but writes at most @c size - 1 characters and always
 * null-terminates when size > 0.
 *
 * @return
 *      the number of characters that the formatted result WOULD have needed,
 *      so a return >= size means the string was truncated.
 */
int snprintf(char *str, int size, char *fmt, ...)
{
    struct snprint_state st;
    va_list ap;
    int n;

    if (str == NULL || size < 0)
    {
        return -1;
    }

    st.s   = str;
    st.end = str + (size > 0 ? size - 1 : 0);

    va_start(ap, fmt);
    n = _doprnt(fmt, ap, snprntf, (uintptr_t)&st);
    va_end(ap);

    if (size > 0)
    {
        *st.s = '\0';
    }

    return n;
}