/* integer division helpers in the alpha division ABI : dividend $24,
 * divisor $25, result $27 (pv on entry), return via $23, $28 scratch.
 * libgcc's versions route through the FPU - these are pure-integer
 * shift-subtract so no-FP cores (and the ISS/RTL co-sim) can divide.
 * ~64x10 insns per divide : slow but deterministic everywhere.
 * divide-by-zero yields quotient 0 / remainder = dividend, no trap. */
	.set noat
	.set noreorder
	.text

/* core restoring-division loop.  in: $24 dividend, $25 divisor.
 * out: $27 quotient, $0 remainder.  clobbers $1, $2, $28. */
.macro	DIV_CORE
	bis	$31, $31, $27
	bis	$31, $31, $0
	lda	$1, 64($31)
1:	srl	$0, 63, $28		/* carry out of remainder shift */
	sll	$0, 1, $0
	srl	$24, 63, $2
	bis	$0, $2, $0
	sll	$24, 1, $24
	sll	$27, 1, $27
	cmpule	$25, $0, $2
	bis	$2, $28, $2		/* shifted-out carry forces subtract */
	beq	$2, 2f
	subq	$0, $25, $0
	bis	$27, 1, $27
2:	subq	$1, 1, $1
	bne	$1, 1b
.endm

.macro	SAVE_TMPS
	lda	$30, -32($30)
	stq	$0, 0($30)
	stq	$1, 8($30)
	stq	$2, 16($30)
.endm

.macro	RESTORE_TMPS_RET
	ldq	$0, 0($30)
	ldq	$1, 8($30)
	ldq	$2, 16($30)
	lda	$30, 32($30)
	ret	$31, ($23), 1
.endm

	.globl __divqu
	.ent __divqu
__divqu:
	SAVE_TMPS
	DIV_CORE
	RESTORE_TMPS_RET
	.end __divqu

	.globl __remqu
	.ent __remqu
__remqu:
	SAVE_TMPS
	DIV_CORE
	bis	$0, $31, $27
	RESTORE_TMPS_RET
	.end __remqu

	.globl __divq
	.ent __divq
__divq:
	SAVE_TMPS
	stq	$3, 24($30)
	xor	$24, $25, $3		/* quotient sign in bit 63 */
	subq	$31, $24, $28
	cmovlt	$24, $28, $24
	subq	$31, $25, $28
	cmovlt	$25, $28, $25
	DIV_CORE
	subq	$31, $27, $28
	cmovlt	$3, $28, $27
	ldq	$3, 24($30)
	RESTORE_TMPS_RET
	.end __divq

	.globl __remq
	.ent __remq
__remq:
	SAVE_TMPS
	stq	$3, 24($30)
	bis	$24, $31, $3		/* remainder takes dividend sign */
	subq	$31, $24, $28
	cmovlt	$24, $28, $24
	subq	$31, $25, $28
	cmovlt	$25, $28, $25
	DIV_CORE
	subq	$31, $0, $28
	cmovlt	$3, $28, $0
	bis	$0, $31, $27
	ldq	$3, 24($30)
	RESTORE_TMPS_RET
	.end __remq

	.globl __divlu
	.ent __divlu
__divlu:
	SAVE_TMPS
	zapnot	$24, 0x0f, $24
	zapnot	$25, 0x0f, $25
	DIV_CORE
	addl	$27, 0, $27
	RESTORE_TMPS_RET
	.end __divlu

	.globl __remlu
	.ent __remlu
__remlu:
	SAVE_TMPS
	zapnot	$24, 0x0f, $24
	zapnot	$25, 0x0f, $25
	DIV_CORE
	addl	$0, 0, $27
	RESTORE_TMPS_RET
	.end __remlu

	.globl __divl
	.ent __divl
__divl:
	SAVE_TMPS
	stq	$3, 24($30)
	addl	$24, 0, $24
	addl	$25, 0, $25
	xor	$24, $25, $3
	subq	$31, $24, $28
	cmovlt	$24, $28, $24
	subq	$31, $25, $28
	cmovlt	$25, $28, $25
	DIV_CORE
	subq	$31, $27, $28
	cmovlt	$3, $28, $27
	addl	$27, 0, $27
	ldq	$3, 24($30)
	RESTORE_TMPS_RET
	.end __divl

	.globl __reml
	.ent __reml
__reml:
	SAVE_TMPS
	stq	$3, 24($30)
	addl	$24, 0, $24
	addl	$25, 0, $25
	bis	$24, $31, $3
	subq	$31, $24, $28
	cmovlt	$24, $28, $24
	subq	$31, $25, $28
	cmovlt	$25, $28, $25
	DIV_CORE
	subq	$31, $0, $28
	cmovlt	$3, $28, $0
	addl	$0, 0, $27
	ldq	$3, 24($30)
	RESTORE_TMPS_RET
	.end __reml
