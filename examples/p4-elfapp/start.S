/* start.S -- arranque del app cargable (p4-elfapp).
 *
 * Un ELF parcialmente enlazado (ld -r) NO tiene puntos de entrada de programa
 * ni cabecera ELF ejecutable: lo que hace el cargador es ponerle direcciones a
 * las secciones, aplicar las relocaciones y saltar a '_start'.  Aqui no hay que
 * montar el stack desde cero: el app corre dentro del hilo del comando de la
 * shell, asi que SP ya vale (el del proceso que llama) y el frame de la shell
 * sigue intacto debajo.  Se salva ra y s0 porque main() y exit() usan el marco
 * propio y el nuestro tiene que volver al cargador por la ruta normal.
 *
 * Dos cosas que hay que ver para que el ejemplo sirva de referencia:
 *
 *   1. 'call main' y 'call exit' salen como relocaciones sin resolver
 *      (R_RISCV_CALL / R_RISCV_CALL_PLT contra simbolos globales).  Son justo las
 *      que un volcado plano (objcopy -O binary) no puede tener, y por eso aqui se
 *      queda uno en ELF en vez de un binario.
 *   2. 'exit' no mata nada: es una funcion del cargador que guarda el codigo y
 *      DEVUELVE.  Al devolver, el control vuelve a la instruccion siguiente del
 *      _start, que restaura ra y hace 'ret' al sitio donde el cargador llamo.  Asi
 *      el app puede terminar con un valor sin necesitar setjmp ni un segundo
 *      punto de salida.
 */

	.section .text.start, "ax", @progbits
	.globl	_start
	.type	_start, @function
_start:
	addi	sp, sp, -16
	sw	ra, 12(sp)              /* guardamos ra y s0: sin coma flotante,  */
	sw	s0, 8(sp)               /* dos palabras bastan                    */
	addi	s0, sp, 16

	call	main                    /* main(void) -> a0 = codigo              */

	sw	a0, -12(s0)             /* guardamos el resultado                 */
	lw	a0, -12(s0)
	call	exit                    /* exit(int): lo exporta el cargador      */

	lw	ra, 12(sp)
	lw	s0, 8(sp)                 /* se devuelve TAMBIEN s0: 'addi s0, sp,   */
	addi	sp, sp, 16              /*   16' de arriba la pisa, y el cargador   */
	ret                             /*   lee s0 al volver (medido en placa).   */
	.size	_start, . - _start
