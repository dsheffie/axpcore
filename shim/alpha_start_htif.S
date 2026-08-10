/* crt for shim-based binaries (csmith) : gp, own stack, argc=0, call
 * main, then exit(main's return) through the htif path in shim.c */
	.set noat
	.text
	.globl _start
	.ent _start
_start:
	br	$29, 1f
1:	ldgp	$29, 0($29)
	ldq	$30, stack_end($29)!literal
	bis	$31, $31, $16
	bis	$31, $31, $17
	ldq	$27, main($29)!literal
	jsr	$26, ($27), main
	bis	$0, $31, $16
	ldq	$27, exit($29)!literal
	jsr	$26, ($27), exit
2:	br	2b
	.end _start
