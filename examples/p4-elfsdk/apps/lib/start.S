/* start.S -- arranque reglamentario de una app ELF del P4.
 *
 * Un ELF parcialmente enlazado ('ld -r') no tiene carga ni stack propio: el
 * cargador le pone direcciones, resuelve sus simbolos contra system/exports.c,
 * aplica las relocaciones y salta a '_start'.  El app corre en la hebra que
 * llama a elf_run() --la del comando 'elf'--, asi que SP ya es valido y el
 * marco del llamador queda debajo; por eso solo se salvan ra y s0.
 *
 * 'exit' mata la hebra: es elf_exit() del cargador y su cuerpo es
 * kill(gettid()); el kill() del kernel suelta la PSRAM del app y NO devuelve,
 * asi que el 'ret' de aqui abajo solo correria si exit() regresara.  Sin
 * setjmp y sin segundo punto de salida.
 *
 * argc/argv: elf_run() del cargador salta aqui con argc en a0 y argv en a1, que
 * son exactamente los dos registros de la ABI para el primer y segundo argumento
 * de una funcion C.  Este arranque NO los toca antes del 'call main', asi que el
 * app es un 'int main(int argc, char *argv[])' estandar sin ni una linea de
 * ensamblador especial.
 */

	.section .text.start, "ax", @progbits
	.globl	_start
	.type	_start, @function
_start:
	addi	sp, sp, -16
	sw	ra, 12(sp)
	sw	s0, 8(sp)
	addi	s0, sp, 16

	call	main                    /* main(argc, argv) -> a0 = codigo de salida */

	sw	a0, -12(s0)
	lw	a0, -12(s0)
	call	exit                    /* elf_exit del cargador: kill(gettid())   */

	lw	ra, 12(sp)
	lw	s0, 8(sp)                 /* muerto si exit() mato la hebra: solo se  */
	addi	sp, sp, 16              /*   llega aqui con un exit que devuelve   */
	ret
	.size	_start, . - _start
