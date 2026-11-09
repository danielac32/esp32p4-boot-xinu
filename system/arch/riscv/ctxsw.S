/**
 * @file ctxsw.S
 */
/* Embedded Xinu, Copyright (C) 2013.  All rights reserved. */

#include <riscv.h>

/* RV32 uses 32-bit load/store; RV64 uses 64-bit.  REGBYTES already accounts
 * for the register size, so only the instruction mnemonic changes. */
#if __riscv_xlen == 64
# define STORE_REG sd
# define LOAD_REG  ld
#else
# define STORE_REG sw
# define LOAD_REG  lw
#endif

.globl ctxsw

/*------------------------------------------------------------------------
 *  ctxsw  -  Switch from one thread context to another.
 *------------------------------------------------------------------------
 *
 * This is the RISCV version.  How it works: we have to save s0-s11
 * ra, and sp since s0-s11 are saved registers, ra  needs to be loaded into the
 * pc when this context is switched to again. Registers a0-a7 are
 * caller-save so they do not need not be saved, but they are pushed anyway
 * since they are part of the context constructed by create() to pass thread arguments.
 *
 * We do not need to do anything about the CSRs here, since:
 *
 *   - We do all our context switches to/from the same mode (namely, Machine mode).
 *   - The RISC-V ABI does not use CSRs
 *   - resched() takes care of saving/restoring whether interrupts are enabled
 *         or not when resuming a thread that has been switched out.
 *   - Xinu never makes changes to the CSRs not already covered above
 *
 * However, interrupts are disabled when ctxsw() is called from resched(), but
 * we want interrupts to be enabled when starting a *new* thread, which
 * resched() does not take care of.  We solve this by including the machine status
 * register value into the context and restoring it when we see that we're
 * switching to the newly created thread
 */

/* Contexto: 23 palabras enteras (pc/ra/mstatus/s0-s11/a0-a7) +
 * 32 registros F (f0-f31) + fcsr = 56 palabras.  Guardar/restaurar FPU evita
 * corromper decodificadores en float (p. ej. minimp3) al cambiar de hilo. */
ctxsw:
	.func ctxsw
	// reserve space
	addi sp, sp, -REGBYTES*56
	// save registers
	STORE_REG a7, REGBYTES*22(sp)
	STORE_REG a6, REGBYTES*21(sp)
	STORE_REG a5, REGBYTES*20(sp)
	STORE_REG a4, REGBYTES*19(sp)
	STORE_REG a3, REGBYTES*18(sp)
	STORE_REG a2, REGBYTES*17(sp)
	STORE_REG a1, REGBYTES*16(sp)
	STORE_REG a0, REGBYTES*15(sp)
	STORE_REG s11, REGBYTES*14(sp)
	STORE_REG s10, REGBYTES*13(sp)
	STORE_REG s9, REGBYTES*12(sp)
	STORE_REG s8, REGBYTES*11(sp)
	STORE_REG s7, REGBYTES*10(sp)
	STORE_REG s6, REGBYTES*9(sp)
	STORE_REG s5, REGBYTES*8(sp)
	STORE_REG s4, REGBYTES*7(sp)
	STORE_REG s3, REGBYTES*6(sp)
	STORE_REG s2, REGBYTES*5(sp)
	STORE_REG s1, REGBYTES*4(sp)
	STORE_REG s0, REGBYTES*3(sp)
	// save mstatus
	csrr t2, mstatus
	STORE_REG t2, REGBYTES*2(sp)
	// we jump at end, jump address is different
	// from return address only when switching to newly
	// created context
	STORE_REG ra,REGBYTES*1(sp)
	STORE_REG ra,(sp)
	// save FPU state (f0-f31 + fcsr)
	csrr t0, fcsr
	STORE_REG t0, REGBYTES*55(sp)
	fsw f0,  REGBYTES*23(sp)
	fsw f1,  REGBYTES*24(sp)
	fsw f2,  REGBYTES*25(sp)
	fsw f3,  REGBYTES*26(sp)
	fsw f4,  REGBYTES*27(sp)
	fsw f5,  REGBYTES*28(sp)
	fsw f6,  REGBYTES*29(sp)
	fsw f7,  REGBYTES*30(sp)
	fsw f8,  REGBYTES*31(sp)
	fsw f9,  REGBYTES*32(sp)
	fsw f10, REGBYTES*33(sp)
	fsw f11, REGBYTES*34(sp)
	fsw f12, REGBYTES*35(sp)
	fsw f13, REGBYTES*36(sp)
	fsw f14, REGBYTES*37(sp)
	fsw f15, REGBYTES*38(sp)
	fsw f16, REGBYTES*39(sp)
	fsw f17, REGBYTES*40(sp)
	fsw f18, REGBYTES*41(sp)
	fsw f19, REGBYTES*42(sp)
	fsw f20, REGBYTES*43(sp)
	fsw f21, REGBYTES*44(sp)
	fsw f22, REGBYTES*45(sp)
	fsw f23, REGBYTES*46(sp)
	fsw f24, REGBYTES*47(sp)
	fsw f25, REGBYTES*48(sp)
	fsw f26, REGBYTES*49(sp)
	fsw f27, REGBYTES*50(sp)
	fsw f28, REGBYTES*51(sp)
	fsw f29, REGBYTES*52(sp)
	fsw f30, REGBYTES*53(sp)
	fsw f31, REGBYTES*54(sp)
	//save modified sp at oldthr->stkptr
	STORE_REG sp, (a0)
	// get saved sp from newthrd->stkptr
	LOAD_REG sp, (a1)

	// restore t0 <- saved jump address
	LOAD_REG t0, (sp)
	LOAD_REG ra, REGBYTES*1(sp)
	// t2 <- saved mstatus
	LOAD_REG t2, REGBYTES*2(sp)

	// restore FPU state
	flw f0,  REGBYTES*23(sp)
	flw f1,  REGBYTES*24(sp)
	flw f2,  REGBYTES*25(sp)
	flw f3,  REGBYTES*26(sp)
	flw f4,  REGBYTES*27(sp)
	flw f5,  REGBYTES*28(sp)
	flw f6,  REGBYTES*29(sp)
	flw f7,  REGBYTES*30(sp)
	flw f8,  REGBYTES*31(sp)
	flw f9,  REGBYTES*32(sp)
	flw f10, REGBYTES*33(sp)
	flw f11, REGBYTES*34(sp)
	flw f12, REGBYTES*35(sp)
	flw f13, REGBYTES*36(sp)
	flw f14, REGBYTES*37(sp)
	flw f15, REGBYTES*38(sp)
	flw f16, REGBYTES*39(sp)
	flw f17, REGBYTES*40(sp)
	flw f18, REGBYTES*41(sp)
	flw f19, REGBYTES*42(sp)
	flw f20, REGBYTES*43(sp)
	flw f21, REGBYTES*44(sp)
	flw f22, REGBYTES*45(sp)
	flw f23, REGBYTES*46(sp)
	flw f24, REGBYTES*47(sp)
	flw f25, REGBYTES*48(sp)
	flw f26, REGBYTES*49(sp)
	flw f27, REGBYTES*50(sp)
	flw f28, REGBYTES*51(sp)
	flw f29, REGBYTES*52(sp)
	flw f30, REGBYTES*53(sp)
	flw f31, REGBYTES*54(sp)
	LOAD_REG t3, REGBYTES*55(sp)
	csrw fcsr, t3

	// restore saved registers
	LOAD_REG s0, REGBYTES*3(sp)
	LOAD_REG s1, REGBYTES*4(sp)
	LOAD_REG s2, REGBYTES*5(sp)
	LOAD_REG s3, REGBYTES*6(sp)
	LOAD_REG s4, REGBYTES*7(sp)
	LOAD_REG s5, REGBYTES*8(sp)
	LOAD_REG s6, REGBYTES*9(sp)
	LOAD_REG s7, REGBYTES*10(sp)
	LOAD_REG s8, REGBYTES*11(sp)
	LOAD_REG s9, REGBYTES*12(sp)
	LOAD_REG s10, REGBYTES*13(sp)
	LOAD_REG s11, REGBYTES*14(sp)

	LOAD_REG a0, REGBYTES*15(sp)
	LOAD_REG a1, REGBYTES*16(sp)
	LOAD_REG a2, REGBYTES*17(sp)
	LOAD_REG a3, REGBYTES*18(sp)
	LOAD_REG a4, REGBYTES*19(sp)
	LOAD_REG a5, REGBYTES*20(sp)
	LOAD_REG a6, REGBYTES*21(sp)
	LOAD_REG a7, REGBYTES*22(sp)

	// restore sp
	addi sp, sp, REGBYTES*56

	// check if interrupts are disabled in saved mstatus
	andi t3, t2, MSTATUS_MIE
	beq t3,x0, end_ctxsw
	// interrupts enabled (new thread)
	// now tricky part: we restore mstatus as it was saved
	// in the context _except_ the interrupt enable bit:
	// t2 <- t2 & ~MSTATUS_MIE
	andi t2, t2, -9
	csrrw t2,  mstatus, t2
	// load jump address to mepc
	csrrw t1, mepc, t0
	// mret will enable interrupts and
	// jump to the newly created thread
	mret
end_ctxsw:
	// interrupts disabled: return to old thread
	// pc is restored, next address is put into
	// t1 and ignored
	jalr t1,t0,0
	.endfunc
