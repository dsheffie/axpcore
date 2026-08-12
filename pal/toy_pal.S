/* phase-1 toy PAL image : exercises the full CALL_PAL -> hw_* ->
 * hw_rei chain.  linked at PAL_BASE (0x10000) with handlers placed at
 * their architected EV4 offsets via .org.
 *
 * implements one custom unprivileged call, func 0x91 (unpriv region,
 * slot 0x11) = pal_add : v0 <- a0 + a1, routed through a PAL_TEMP so
 * hw_mtpr/hw_mfpr are on the path.  reset and OPCDEC get minimal
 * handlers.
 */
#include "pal_macros.h"

	.text

	.org	PAL_RESET
reset:
	br	$31, reset		/* not exercised in phase 1 */

	.org	PAL_OPCDEC
opcdec:
	/* toy : skip the faulting instruction and return.  EXC_ADDR
	 * holds (faulting_pc | old_mode); advance the pc field by 4,
	 * preserving the mode bit, then rei.  r1 is a guest register -
	 * save/restore it through a PAL_TEMP (PAL must not clobber
	 * guest state). */
	hw_mtpr	1, IPR_PAL_TEMP + 1	/* stash r1 */
	hw_mfpr	1, IPR_EXC_ADDR		/* r1 <- EXC_ADDR */
	lda	$1, 4($1)		/* pc field += 4 (mode bit safe: pc is 4-aligned) */
	hw_mtpr	1, IPR_EXC_ADDR
	hw_mfpr	1, IPR_PAL_TEMP + 1	/* restore r1 */
	hw_rei

	.org	PAL_CALLPAL_UNPRIV + (0x11 << 6)
pal_add:
	hw_mtpr	16, IPR_PAL_TEMP	/* PAL_TEMP0 <- a0 */
	hw_mfpr	0,  IPR_PAL_TEMP	/* v0 <- PAL_TEMP0 */
	addq	$0, $17, $0		/* v0 <- v0 + a1 */
	hw_rei

	.org	PAL_CALLPAL_UNPRIV + (0x12 << 6)
pal_memtest:
	/* func 0x92 : store a1 to phys addr a0, load it back into v0.
	 * exercises hw_stq + hw_ldq (physical, no translation). */
	hw_stq	17, 0, 16		/* phys[a0] <- a1 */
	hw_ldq	0, 0, 16		/* v0 <- phys[a0] */
	hw_rei
