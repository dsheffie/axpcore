/* minimal crt for freestanding alpha tests : locate gp, set up a
 * self-contained stack (works on bare RTL where no loader provides
 * one - the C file must define stack_end), call main, exit via
 * callsys with main's return value. */
	.set noat
	.text
	.globl _start
	.ent _start
_start:
	br	$29, 1f
1:	ldgp	$29, 0($29)
	ldq	$30, stack_end($29)!literal
	ldq	$27, main($29)!literal
	jsr	$26, ($27), main
	mov	$0, $16
	lda	$0, 1($31)
	call_pal 0x83
2:	br	2b
	.end _start
