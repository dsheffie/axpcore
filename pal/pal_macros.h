/* axpcore PAL assembler support.
 *
 * this binutils gates the named hw_* mnemonics off, and the PAL
 * reserved-opcode substrate is ours to define anyway (PALCODE.md
 * decision 4), so the five reserved PAL opcodes are emitted as .long
 * with explicit encodings.  objdump renders them as pal19/pal1b/
 * pal1d/pal1e/pal1f, which is handy in traces.
 *
 * operands are register NUMBERS and integer IPR indices (not
 * $-prefixed) so the macros assemble on stock gas.
 *
 *   0x19 pal19 hw_mfpr  Ra <- IPR[idx]
 *   0x1d pal1d hw_mtpr  IPR[idx] <- Ra
 *   0x1b pal1b hw_ld    Ra <- phys_qword[Rb + disp]
 *   0x1f pal1f hw_st    phys_qword[Rb + disp] <- Ra
 *   0x1e pal1e hw_rei   PC <- EXC_ADDR ; mode <- EXC_ADDR<0>
 */

/* hw_mfpr/hw_mtpr : 21064 requires Ra == Rb, so both fields carry the
 * register.  idx is our chip-private IPR number (the real chip splits
 * this into PAL/ABX/IBX box-routing + INDEX; axpcore uses a flat IPR
 * space per PALCODE.md decision 4). */
.macro	hw_mfpr ra, idx
	.long	0x64000000 | ((\ra) << 21) | ((\ra) << 16) | ((\idx) & 0x3f)
.endm

.macro	hw_mtpr ra, idx
	.long	0x74000000 | ((\ra) << 21) | ((\ra) << 16) | ((\idx) & 0x3f)
.endm

/* hw_ld/hw_st : 21064 format - Q bit [12] (1=quadword), 12-bit signed
 * byte disp [11:0], address naturally aligned.  physical only. */
.macro	hw_ldq ra, disp, rb
	.long	0x6c001000 | ((\ra) << 21) | ((\rb) << 16) | ((\disp) & 0xfff)
.endm

.macro	hw_ldl ra, disp, rb
	.long	0x6c000000 | ((\ra) << 21) | ((\rb) << 16) | ((\disp) & 0xfff)
.endm

.macro	hw_stq ra, disp, rb
	.long	0x7c001000 | ((\ra) << 21) | ((\rb) << 16) | ((\disp) & 0xfff)
.endm

.macro	hw_stl ra, disp, rb
	.long	0x7c000000 | ((\ra) << 21) | ((\rb) << 16) | ((\disp) & 0xfff)
.endm

/* hw_rei : VPC <- EXC_ADDR & ~3 ; PALmode <- EXC_ADDR<0>.  bit [15]
 * set = pop the JSR prediction stack (the architected return hint). */
.macro	hw_rei
	.long	0x78008000
.endm

/* IPR indices - must match the enum in alpha_interp.hh */
.equ	IPR_PS,        0
.equ	IPR_EXC_ADDR,  1
.equ	IPR_PAL_BASE,  2
.equ	IPR_PTBR,      3
.equ	IPR_VPTPTR,    4
.equ	IPR_WHAMI,     6
.equ	IPR_SIRR,      7
.equ	IPR_EXC_SUM,   8
.equ	IPR_PAL_TEMP,  16

/* EV4 PAL entry offsets - must match the enum in alpha_interp.hh */
.equ	PAL_RESET,          0x0000
.equ	PAL_MCHK,           0x0020
.equ	PAL_ARITH,          0x0060
.equ	PAL_INTERRUPT,      0x00E0
.equ	PAL_DSTREAM_ERR,    0x01E0
.equ	PAL_ITB_MISS,       0x03E0
.equ	PAL_IACCVIO,        0x07E0
.equ	PAL_DTB_MISS_N,     0x08E0
.equ	PAL_DTB_MISS_P,     0x09E0
.equ	PAL_UNALIGN,        0x11E0
.equ	PAL_OPCDEC,         0x13E0
.equ	PAL_FEN,            0x17E0
.equ	PAL_CALLPAL_PRIV,   0x2000
.equ	PAL_CALLPAL_UNPRIV, 0x3000
