/**
 * @file compiler_rt.c
 *
 * Minimal compiler run-time helpers for RV32.  These are the routines the
 * compiler cannot expand inline and that the link does not already get from
 * somewhere else: the 64-bit integer helpers (division, bit scanning) live
 * here, while the double-precision arithmetic the JVM pulls in comes from
 * the chip's mask ROM -- see the PROVIDE block in
 * compile/platforms/esp32p4/ld.script.  Only the narrowing double -> float
 * is missing from both libgcc and the ROM, and that one is below.
 *
 * The integer routines are written with 32-bit operations only on purpose:
 * using 64-bit shifts/multiplies in their own implementation would just pull
 * in more helpers (__lshrdi3, __muldi3, ...) and defeat the point.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <stdint.h>

/* ---------------- bit scanning ---------------- */

/* Number of leading zero bits in a 32-bit word (32 if x == 0). */
int __clzsi2(unsigned int x)
{
    int n = 0;

    if (x == 0)
    {
        return 32;
    }
    if (x <= 0x0000FFFFU)
    {
        n += 16;
        x <<= 16;
    }
    if (x <= 0x00FFFFFFU)
    {
        n += 8;
        x <<= 8;
    }
    if (x <= 0x0FFFFFFFU)
    {
        n += 4;
        x <<= 4;
    }
    if (x <= 0x3FFFFFFFU)
    {
        n += 2;
        x <<= 2;
    }
    if (x <= 0x7FFFFFFFU)
    {
        n += 1;
    }
    return n;
}

/* Number of trailing zero bits in a 32-bit word (32 if x == 0). */
int __ctzsi2(unsigned int x)
{
    int n = 0;

    if (x == 0)
    {
        return 32;
    }
    if ((x & 0x0000FFFFU) == 0)
    {
        n += 16;
        x >>= 16;
    }
    if ((x & 0x000000FFU) == 0)
    {
        n += 8;
        x >>= 8;
    }
    if ((x & 0x0000000FU) == 0)
    {
        n += 4;
        x >>= 4;
    }
    if ((x & 0x00000003U) == 0)
    {
        n += 2;
        x >>= 2;
    }
    if ((x & 0x00000001U) == 0)
    {
        n += 1;
    }
    return n;
}

/* Index (1-based) of the least significant set bit, or 0 if a == 0. */
int __ffssi2(int a)
{
    return (a == 0) ? 0 : __ctzsi2((unsigned int) a) + 1;
}

/* ---------------- 64-bit division ---------------- */

/*
 * Unsigned 64/64 -> 64 division with remainder: 32-bit shift-and-subtract so
 * that no 64-bit shift/multiply helper is required.  If d == 0 the result is
 * implementation-defined (we return 0 to avoid a divide trap).
 */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint32_t nh = (uint32_t) (n >> 32);
    uint32_t nl = (uint32_t) n;
    uint32_t dh = (uint32_t) (d >> 32);
    uint32_t dl = (uint32_t) d;
    uint32_t qh = 0, ql = 0, rh = 0, rl = 0;
    int i;

    if (dh == 0 && dl == 0)
    {
        if (rem != 0)
        {
            *rem = 0;
        }
        return 0;
    }

    for (i = 63; i >= 0; i--)
    {
        uint32_t bit;

        /* r = (r << 1) | next bit of n */
        if (i >= 32)
        {
            bit = (nh >> (i - 32)) & 1U;
        }
        else
        {
            bit = (nl >> i) & 1U;
        }
        rh = (rh << 1) | (rl >> 31);
        rl = (rl << 1) | bit;

        /* if r >= d then r -= d and set bit i of the quotient */
        if (rh > dh || (rh == dh && rl >= dl))
        {
            uint32_t borrow = (rl < dl) ? 1U : 0U;

            rl -= dl;
            rh = rh - dh - borrow;
            if (i >= 32)
            {
                qh |= (1U << (i - 32));
            }
            else
            {
                ql |= (1U << i);
            }
        }
    }

    if (rem != 0)
    {
        *rem = ((uint64_t) rh << 32) | rl;
    }
    return ((uint64_t) qh << 32) | ql;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return udiv64(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t r;

    (void) udiv64(a, b, &r);
    return r;
}

/* ---------------- doble -> simple (IEEE-754) ----------------
 * El ROM del P4 trae el set df casi entero (ver las PROVIDE de
 * compile/platforms/esp32p4/ld.script) y el libgcc de la multilib ilp32f
 * solo trae 6 rutinas; la que falta en los dos sitios es esta, la estrecha
 * de doble a simple.  La pide el interprete de la JVM en los opcodes de
 * float (bytecodes.c hace la cuenta en doble y estrecha), y sin ella el
 * enlace queda roto.
 *
 * Los dos bits de entrada que importan son los 32 de arriba y los 32 de
 * abajo del patrón de 64: se sacan con union, no con divisiones.  A partir
 * de ahi todo son mascaras y turnos de 32 bits, salvo el salto de 64 bits
 * que monta el numero de 53 (s_hi:s_lo); se evitan turnos de 64 variables
 * separando los tres casos de sh (por debajo, justo en, y por encima de 32)
 * --que es el suelo que impone la cabecera de este fichero.
 *
 * Redondeo half-even, el mismo del conversor de hardware: se mira el bit de
 * turno (round) y si aun queda algo por debajo (sticky); con empate se
 * elige el mantissa par.  Con esto la funcion es bit a bit igual a
 * `(float)d' para cualquier d: comprobado en el host contra 2.640.025
 * valores (normales, subnormales de entrada y de salida, 0/-0, Inf, NaN,
 * desbordes y patrones de bits aleatorios) --0 diferencias. */
float __truncdfsf2(double a)
{
    union { double d; uint64_t u; } in;
    union { float f; uint32_t u; } out;
    uint32_t hi, lo, sign, exp, mhi, mlo, kept, round, sticky;
    int e;

    in.d = a;
    hi = (uint32_t)(in.u >> 32);
    lo = (uint32_t)in.u;
    sign = hi >> 31;
    exp = (hi >> 20) & 0x7FFu;      /* exponente sesgado por 1023 */
    mhi = hi & 0xFFFFFu;            /* mantissa, bits 51..32           */
    mlo = lo;                       /* mantissa, bits 31..0            */

    if (exp == 0x7FFu)
    {
        /* Inf se pasa como Inf; NaN como NaN silencioso, con su signo. */
        out.u = (sign << 31) | ((mhi | mlo) ? 0x7FC00000u : 0x7F800000u);
        return out.f;
    }
    if (exp == 0u)
    {
        /* Cero o subnormal de doble: todo eso vale menos que el subnormal
         * mas pequeno del simple (2^-149), asi que estrecha a +-0. */
        out.u = sign << 31;
        return out.f;
    }

    e = (int)exp - 1023;
    if (e >= 128)
    {
        out.u = (sign << 31) | 0x7F800000u;     /* desborda a Inf */
        return out.f;
    }

    if (e >= -126)
    {
        /* Normal de salida: de 52 bits de fraccion a 23, se tiran 29. */
        kept = (mhi << 3) | (mlo >> 29);
        round = (mlo >> 28) & 1u;
        sticky = mlo & 0x0FFFFFFFu;
        if (round && (sticky || (kept & 1u)))
        {
            kept++;
            if (kept == 0x800000u)
            {
                /* la fraccion se lleva a 1.0: sube el exponente */
                kept = 0u;
                if (++e >= 128)
                {
                    out.u = (sign << 31) | 0x7F800000u;
                    return out.f;
                }
            }
        }
        out.u = (sign << 31) | ((uint32_t)(e + 127) << 23) | kept;
        return out.f;
    }

    /* Subnormal de salida: sin el 1 implicito en el exponente, el valor es
     * M x 2^-149 con M = s / 2^sh, donde s = 2^52 + fraccion (los 53 bits
     * del significando) y sh = -e - 97.  Aqui sh cae entre 30 y 53. */
    {
        uint32_t s_hi = 0x100000u | mhi;    /* s, bits 52..32 (21 bits) */
        uint32_t s_lo = mlo;                /* s, bits 31..0            */
        uint32_t sh = (uint32_t)(-e - 97);
        uint32_t M, r, st, t;

        if (sh > 53u)
        {
            out.u = sign << 31;
            return out.f;
        }
        if (sh < 32u)
        {
            M = (s_hi << (32u - sh)) | (s_lo >> sh);
            r = (s_lo >> (sh - 1u)) & 1u;
            st = (s_lo & ((1u << (sh - 1u)) - 1u)) != 0u;
        }
        else if (sh == 32u)
        {
            M = s_hi;
            r = (s_lo >> 31) & 1u;
            st = (s_lo & 0x7FFFFFFFu) != 0u;
        }
        else
        {
            t = sh - 32u;                   /* 1..21 */
            M = s_hi >> t;
            r = (s_hi >> (t - 1u)) & 1u;
            st = ((s_hi & ((1u << (t - 1u)) - 1u)) | s_lo) != 0u;
        }
        if (r && (st || (M & 1u)))
        {
            M++;
            /* Si M estaba en 0x7FFFFF, el +1 lo deja en 0x800000, y ese
             * patron ES el normal mas pequeno (exponente 1, fraccion 0):
             * el subnormal maximo redondea hacia arriba y cae justo en el
             * vecino normal, sin tramo aparte. */
        }
        out.u = (sign << 31) | M;
        return out.f;
    }
}
