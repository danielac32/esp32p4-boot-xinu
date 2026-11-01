/**
 * @file start.S
 *
 * ESP32-P4 kernel entry point.
 *
 * The SBL jumps here at __kernel_entry_point (0x40005000) after setting up
 * the MMU, the cache and the PSRAM.  _start works out the CLIC (hardware
 * vectored interrupt controller), enables the FPU, sets up the C stack in
 * the internal L2MEM, copies .data / clears .bss and hands over to C
 * (nulluser).
 */
/* Embedded Xinu, Copyright (C) 2026.  All rights reserved. */

#include <riscv.h>

#define PANICSTK  16384
#define NULLSTK   16384

.section .init
	.globl _start
	.func _start
_start:
	j reset_handler
	.endfunc

/*
 * Exception vector table (CLIC mode: mtvec = ExceptionVectorTable | 3).
 * Interrupts never land here: they vector through mtvt (the CLIC hardware
 * table).  Only synchronous exceptions do, including ecall (mcause 11).
 *
 * Save the caller-saved registers plus mepc/mstatus into a trap_frame_t
 * (field order = the sw offsets below) and dispatch in C (trap_handler in
 * system/arch/riscv/trap.c).  Interrupts are already disabled by the
 * exception (mstatus.MIE -> MPIE), so the handler runs atomically.
 */
.section .init
.align 6
	.globl ExceptionVectorTable
ExceptionVectorTable:
	addi sp, sp, -80
	sw ra,  0(sp)
	sw t0,  4(sp)
	sw t1,  8(sp)
	sw t2, 12(sp)
	sw t3, 16(sp)
	sw t4, 20(sp)
	sw t5, 24(sp)
	sw t6, 28(sp)
	sw a0, 32(sp)
	sw a1, 36(sp)
	sw a2, 40(sp)
	sw a3, 44(sp)
	sw a4, 48(sp)
	sw a5, 52(sp)
	sw a6, 56(sp)
	sw a7, 60(sp)
	csrr t0, mepc
	sw t0, 64(sp)
	csrr t0, mstatus
	sw t0, 68(sp)
	mv a0, sp
	call trap_handler
	lw t0, 64(sp)
	csrw mepc, t0
	lw t0, 68(sp)
	csrw mstatus, t0
	lw ra,  0(sp)
	lw t1,  8(sp)
	lw t2, 12(sp)
	lw t3, 16(sp)
	lw t4, 20(sp)
	lw t5, 24(sp)
	lw t6, 28(sp)
	lw a0, 32(sp)
	lw a1, 36(sp)
	lw a2, 40(sp)
	lw a3, 44(sp)
	lw a4, 48(sp)
	lw a5, 52(sp)
	lw a6, 56(sp)
	lw a7, 60(sp)
	lw t0,  4(sp)
	addi sp, sp, 80
	mret

.section .text
	.globl reset_handler
	.func reset_handler
reset_handler:
/*
	 * Inicializar gp sin permitir que el enlazador
	 * transforme esta misma carga en un acceso relativo a gp.
	 */
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
	
	/* Only core 0 runs the kernel; park the rest. */
	csrr t0, mhartid
	bnez t0, park

	/* Disable global interrupts while we set things up. */
	li t0, MSTATUS_MIE
	csrc mstatus, t0

	/* Enable the FPU (mstatus.FS = 11) and clear fcsr. */
	li t0, 0x6000
	csrs mstatus, t0
	csrw fcsr, zero

	/* Copy .data from its flash load address into L2MEM. */
	la t0, _sdata
	la t1, _edata
	la t2, _data_loadaddr
1:
	bgeu t0, t1, 2f
	lw t3, 0(t2)
	sw t3, 0(t0)
	addi t0, t0, 4
	addi t2, t2, 4
	j 1b
2:
	/* Clear .bss. */
	la t0, _sbss
	la t1, _ebss
3:
	bgeu t0, t1, 4f
	sw zero, 0(t0)
	addi t0, t0, 4
	j 3b
4:
	/* CLIC hardware vector table (mtvt, CSR 0x307).  Written numerically:
	 * this assembler does not know the mtvt name. */
	la t0, InterruptVectorTable
	csrw 0x307, t0

	/* Exception base, CLIC mode (mtvec.MODE = 3). */
	la t0, ExceptionVectorTable
	ori t0, t0, 3
	csrw mtvec, t0

	/* Panic stack, then the null thread stack; memheap is its top. */
	la sp, _end
	li t1, PANICSTK
	add sp, sp, t1
	la t0, panicstk
	sw sp, 0(t0)
	li t1, NULLSTK
	add sp, sp, t1
	la t0, memheap
	sw sp, 0(t0)

	/* cpuid = 0 */
	la t0, cpuid
	sw zero, 0(t0)

	j nulluser

park:
	/* Xinu es uniprocessor: el core1 no ejecuta código del kernel.  Aquí se
	 * le da la pila que el SBL no le puso (core1 entra en _start con el sp
	 * que dejó el BootROM) y se queda en bucle.
	 *
	 * La pila importa aunque el bucle no la use: cualquier excepción
	 * síncrona cae en ExceptionVectorTable, que hace "addi sp,sp,-80" y 17
	 * sw; con sp basura eso es un fallo de almacenamiento que vuelve a
	 * trapar dentro del handler.  Con pila y mtvec propios, si trapara, se
	 * vería un panic normal en lugar de un reinicio silencioso por el ROM.
	 * No se habilitan interrupciones en este hart: las enrutadas al core1
	 * se gestionarían con el estado del kernel compartido. */
	la sp, _core1stk_end
	li t0, MSTATUS_MIE
	csrc mstatus, t0
	la t0, ExceptionVectorTable
	ori t0, t0, 3
	csrw mtvec, t0
	/* Testigo: sin JTAG es lo único que demuestra que el core1 llegó y sigue
	 * aquí.  Se lee con "memdump <_core1stk> 12" -> {0xC0DE1234, mhartid,
	 * sp}.  El borrado de .bss no lo toca (sección aparte en ld.script). */
	li t0, 0xC0DE1234
	la t1, _core1stk
	sw t0, 0(t1)
	csrr t0, mhartid
	sw t0, 4(t1)
	sw sp, 8(t1)
loop:
	/* WFI duerme el hart hasta una interrupción pendiente; MIE=0, así que
	 * vuelve a caer aquí.  El j loop cubre el despertar espurio. */
	wfi
	j loop
	.endfunc
