/**
 * @file irqentry.S
 *
 * Hardware-vectored CLIC interrupt entry for the ESP32-P4.
 *
 * The CLIC jumps straight to InterruptVectorTable[irq] (loaded in mtvt), so
 * every table entry points here.  This entry must save/restore mepc and
 * mstatus around the C handler: ctxsw() switches threads with mepc + mret,
 * so a handler that contexts-switches clobbers both CSRs.  Without this
 * save/restore the handler's final mret would return to whatever mepc
 * ctxsw left behind (the new thread's entry PC) and to whatever privilege
 * the last mret left in mstatus.MPP (U), which faults.
 *
 * The exception stub in start.S keeps the same discipline.
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

	.text
	.align 4
	.globl p4_irq_entry

p4_irq_entry:
	addi	sp, sp, -208

	/* Caller-saved integer registers (the interrupted thread owns them). */
	sw	ra,   0(sp)
	sw	t0,   4(sp)
	sw	t1,   8(sp)
	sw	t2,  12(sp)
	sw	t3,  16(sp)
	sw	t4,  20(sp)
	sw	t5,  24(sp)
	sw	t6,  28(sp)
	sw	a0,  32(sp)
	sw	a1,  36(sp)
	sw	a2,  40(sp)
	sw	a3,  44(sp)
	sw	a4,  48(sp)
	sw	a5,  52(sp)
	sw	a6,  56(sp)
	sw	a7,  60(sp)

	/* Trap return state: global CSRs, clobbered by ctxsw(). */
	csrr	t0, mepc
	sw	t0,  64(sp)
	csrr	t0, mstatus
	sw	t0,  68(sp)

	/* FP state (f0-f31 relevantes + fcsr): preserva FPU ante preempcion/ctxsw. */
	csrr	t0, fcsr
	sw	t0,  72(sp)
	fsw	ft0,  76(sp)
	fsw	ft1,  80(sp)
	fsw	ft2,  84(sp)
	fsw	ft3,  88(sp)
	fsw	ft4,  92(sp)
	fsw	ft5,  96(sp)
	fsw	ft6, 100(sp)
	fsw	ft7, 104(sp)
	fsw	fa0, 108(sp)
	fsw	fa1, 112(sp)
	fsw	fa2, 116(sp)
	fsw	fa3, 120(sp)
	fsw	fa4, 124(sp)
	fsw	fa5, 128(sp)
	fsw	fa6, 132(sp)
	fsw	fa7, 136(sp)
	fsw	ft8, 140(sp)
	fsw	ft9, 144(sp)
	fsw	ft10, 148(sp)
	fsw	ft11, 152(sp)
	fsw	fs0, 156(sp)
	fsw	fs0, 156(sp)
	fsw	fs1, 160(sp)
	fsw	fs2, 164(sp)
	fsw	fs3, 168(sp)
	fsw	fs4, 172(sp)
	fsw	fs5, 176(sp)
	fsw	fs6, 180(sp)
	fsw	fs7, 184(sp)
	fsw	fs8, 188(sp)
	fsw	fs9, 192(sp)
	fsw	fs10, 196(sp)
	fsw	fs11, 200(sp)

	/* Dispatch: irq number is the low byte of mcause in CLIC mode. */
	csrr	t0, mcause
	andi	t0, t0, 0xff
	slli	t0, t0, 2
	la	t1, interruptVector
	add	t1, t1, t0
	lw	t1, 0(t1)
	bnez	t1, 1f
	/* TEMPORARY (bring-up): a line with no handler would jump to 0. */
	csrr	a0, mcause
	call	irq_missing
1:
	jalr	t1

	/* Restore the CSRs ctxsw() may have overwritten while we were away. */
	lw	t0,  64(sp)
	csrw	mepc, t0
	lw	t0,  68(sp)
	csrw	mstatus, t0
	lw	t0,  72(sp)
	csrw	fcsr, t0

	flw	ft0,  76(sp)
	flw	ft1,  80(sp)
	flw	ft2,  84(sp)
	flw	ft3,  88(sp)
	flw	ft4,  92(sp)
	flw	ft5,  96(sp)
	flw	ft6, 100(sp)
	flw	ft7, 104(sp)
	flw	fa0, 108(sp)
	flw	fa1, 112(sp)
	flw	fa2, 116(sp)
	flw	fa3, 120(sp)
	flw	fa4, 124(sp)
	flw	fa5, 128(sp)
	flw	fa6, 132(sp)
	flw	fa7, 136(sp)
	flw	ft8, 140(sp)
	flw	ft9, 144(sp)
	flw	ft10, 148(sp)
	flw	ft11, 152(sp)
	flw	fs0, 156(sp)
	flw	fs1, 160(sp)
	flw	fs2, 164(sp)
	flw	fs3, 168(sp)
	flw	fs4, 172(sp)
	flw	fs5, 176(sp)
	flw	fs6, 180(sp)
	flw	fs7, 184(sp)
	flw	fs8, 188(sp)
	flw	fs9, 192(sp)
	flw	fs10, 196(sp)
	flw	fs11, 200(sp)

	lw	ra,   0(sp)
	lw	t1,   8(sp)
	lw	t2,  12(sp)
	lw	t3,  16(sp)
	lw	t4,  20(sp)
	lw	t5,  24(sp)
	lw	t6,  28(sp)
	lw	a0,  32(sp)
	lw	a1,  36(sp)
	lw	a2,  40(sp)
	lw	a3,  44(sp)
	lw	a4,  48(sp)
	lw	a5,  52(sp)
	lw	a6,  56(sp)
	lw	a7,  60(sp)
	lw	t0,   4(sp)

	addi	sp, sp, 208
	mret
