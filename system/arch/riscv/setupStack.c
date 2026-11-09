#include <platform.h>
#include <riscv.h>
#include <stdint.h>

/* 23 (ra/mstatus/s0-s11/a0-a7 + pc) + 32 F regs (f0-f31) + fcsr = 56 */
#define CONTEXT_WORDS 56
#define MAX_REG_ARGS 8

void *setupStack(void *stackaddr, void *procaddr,
                 void *retaddr, uint nargs, va_list ap)
{
    uint spilled_nargs =
        (nargs > MAX_REG_ARGS) ? nargs - MAX_REG_ARGS : 0;

    /*
     * Reservar espacio para los argumentos pasados por pila.
     * Alinear hacia abajo la pila que verá la tarea al entrar.
     *
     * Se conserva la convención original de stackaddr como
     * límite superior disponible.
     */
    uintptr_t entry_sp = (uintptr_t)stackaddr;

    entry_sp -= (uintptr_t)spilled_nargs * sizeof(ulong);
    entry_sp &= ~(uintptr_t)0x0f;

    ulong *stack_args = (ulong *)entry_sp;
    ulong *context = stack_args - CONTEXT_WORDS;

    /* Inicializar todos los campos del contexto. */
    for (uint i = 0; i < CONTEXT_WORDS; ++i) {
        context[i] = 0;
    }

    context[0] = (ulong)procaddr;
    context[1] = (ulong)retaddr;

    context[2] =
        MSTATUS_MIE |
        MSTATUS_MPP |
        MSTATUS_MPIE |
        MSTATUS_FS;

    /*
     * context[3..14]  = s0..s11
     * context[15..22] = a0..a7
     * stack_args[0]  = noveno argumento
     */
    for (uint j = 0; j < nargs; ++j) {
        ulong arg = va_arg(ap, ulong);

        if (j < MAX_REG_ARGS) {
            context[15 + j] = arg;
        } else {
            stack_args[j - MAX_REG_ARGS] = arg;
        }
    }

    return context;
}
