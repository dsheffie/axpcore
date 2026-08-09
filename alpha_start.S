/* minimal crt for freestanding alpha tests : locate gp, call main,
 * exit with its return value.  under qemu-alpha the kernel loader
 * provides a valid sp; under alpha_iss the driver sets one. */
	.set noat
	.text
	.globl _start
	.ent _start
_start:
	br	$29, 1f
1:	ldgp	$29, 0($29)
	ldq	$27, main($29)!literal
	jsr	$26, ($27), main
	mov	$0, $16
	lda	$0, 1($31)
	call_pal 0x83
2:	br	2b
	.end _start
