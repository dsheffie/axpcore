# PALcode support for axpcore : step-by-step implementation guide

Source of truth : `palcode_dsgn_gde.pdf` (DEC "PALcode for Alpha
Microprocessors System Design Guide", May 1996, EBSDK V2.1 - covers
the 21064 family, i.e. our EV4 target) plus the Alpha ARM for the
OSF/1 personality details.  This doc distills the manual into an
axpcore plan.

## what the manual pins down (the substrate contract)

**PALmode environment** (manual 1.2, table 1-1) - three properties:
1. I-stream memory mapping disabled (PAL fetches physically)
2. interrupts disabled
3. five reserved opcodes enabled (physical ld/st, IPR moves, return
   to native mode).  executing them outside PALmode = OPCDEC fault.

**Invocation** (2.1) : on any invoking event the chip (1) drains the
pipeline, (2) saves current PC in EXC_ADDR, (3) dispatches to
PAL_BASE + fixed offset, entering PALmode.  events: reset, machine
check, arithmetic trap, interrupts, TB miss / MM faults, unaligned,
OPCDEC, FEN, CALL_PAL.

**EV4-family entry-point map** (2.2.1, figure 2-1 - we adopt this
verbatim as axpcore's map):

| offset | entry                              |
|--------|------------------------------------|
| 0x0000 | reset                              |
| 0x0020 | machine check                      |
| 0x0060 | arithmetic exception               |
| 0x00E0 | interrupts                         |
| 0x01E0 | D-stream errors (MM faults)        |
| 0x03E0 | ITB miss                           |
| 0x07E0 | I-stream access violation          |
| 0x08E0 | DTB miss (native mode)             |
| 0x09E0 | DTB miss (PALmode)                 |
| 0x11E0 | unaligned                          |
| 0x13E0 | reserved/privileged opcode (OPCDEC)|
| 0x17E0 | floating-point (FEN)               |
| 0x2000 | privileged CALL_PAL region         |
| 0x3000 | unprivileged CALL_PAL region       |

**CALL_PAL vectoring** (2.3/2.4) : opcode 0 + 26-bit function; bit 7
of the function = unprivileged.  64 privileged + 64 unprivileged
functions, 64-byte slots :
`entry = PAL_BASE + (func<7> ? 0x3000 : 0x2000) + (func<5:0> << 6)`.
privileged CALL_PAL from user mode -> OPCDEC entry.  >128 functions
overflow into the OPCDEC handler by convention.

**The five reserved PAL opcodes - CONFIRMED against the real 21064
HRM** (Digital Semiconductor 21064/21064A HRM, section 2.11.1 table
2-5 + section 4.8; text extracted from the bitsavers/open-watcom
scan).  axpcore's opcode assignments are the real chip's, verbatim:

| opcode | mnemonic | 21064 operation |
|--------|----------|-----------------|
| 0x19 (PAL19) | HW_MFPR | Ra <- IPR[index] ; **Ra and Rb must be identical** |
| 0x1B (PAL1B) | HW_LD | Ra <- mem[Rb + sext(disp)] |
| 0x1D (PAL1D) | HW_MTPR | IPR[index] <- Ra (=Rb) |
| 0x1E (PAL1E) | HW_REI | VPC <- EXC_ADDR & ~3 ; PALmode <- EXC_ADDR<0> |
| 0x1F (PAL1F) | HW_ST | mem[Rb + sext(disp)] <- Ra |

these produce OPCDEC outside PALmode - matches our gate exactly.
field layouts we now follow (section 4.8, tables 4-6/4-8/4-9):
- **HW_MFPR/HW_MTPR** : Ra[25:21], Rb[20:16] (must equal Ra), then
  box-routing (PAL/ABX/IBX) + INDEX low bits.  axpcore keeps a FLAT
  chip-private IPR index (the real chip's box-routing is per-chip
  anyway - decision 4) and sets Rb=Ra for encode-correctness.
- **HW_LD/HW_ST** : Ra[25:21] data, Rb[20:16] base, control H/L/W
  [15:13], **Q bit [12]** (1=quadword/0=longword), **12-bit signed
  disp [11:0]**; ea = (Rb + sext(disp)) & ~(Q?7:3) (natural align).
  axpcore now implements exactly this (was a 16-bit-disp shortcut).
- **HW_REI** : bits[15:14] = branch-prediction hint; bit[15] set pops
  the JSR return stack.  VPC/PALmode from EXC_ADDR as above.

bonus: the same opcode grid (table 2-4) confirms the VAX FP groups
FLTV=0x15 / FLTI=0x16 / FLTL=0x17 for the eventual FPU port.

sources : Digital Semiconductor Alpha 21064/21064A HRM
(open-watcom.github.io/.../21064_64a_hrm.pdf) ; PALcode System Design
Guide (archive.org dec-palcode_dsgn_gde, our in-repo pdf).

**The real 21064 IPR set** (HRM chapter 5 + access table 4-7).  MFPR/
MTPR select an IPR by a box bit (PAL / ABX=Abox / IBX=Ibox) plus an
INDEX; axpcore keeps a flat chip-private index instead.  the full
list, and what we actually need :

Ibox (IBX) : EXC_ADDR(4), PS(9), PAL_BASE(11) - we already have these;
EXC_SUM(10) arith-trap summary, SIRR(13)/HIRR(12)/ASTRR(14) interrupt
requests + HIER(16)/SIER(17)/ASTER(18) enables (phase 5); TB_TAG(0)/
ITB_PTE(1)/ITB_PTE_TEMP(3) ITB fill + ITBZAP(6)/ITBASM(7)/ITBIS(8)
invalidates (phase 4); ICCSR(2) icache+perf, SL_RCV(5)/SL_CLR(19)/
SL_XMIT(22) console UART (our htif subs).

Abox (ABX) : DTB_PTE(2)/DTB_PTE_TEMP(3) DTB fill, MM_CSR(4) fault
status (opcode/RW/type), VA(5) fault address, TB_CTL(0) page-size,
DTBZAP(6)/DTBASM(7)/DTBIS(8) invalidates (all phase 4); CC(16)/
CC_CTL(17) cycle counter (RPCC backing - maps to our icnt);
ALT_MODE(15) HW_LD/ST access mode; ABOX_CTL(14)/BIU_CTL(18) +
BIU_ADDR/BIU_STAT/DC_STAT/FILL_ADDR/FILL_SYNDROME/BC_TAG (chip glue /
machine-check only); FLUSH_IC(21)/FLUSH_IC_ASM(23) icache flush (our
FENCEI subs).

PAL : PAL_TEMP[31:0] (INDEX 31-0) - 32 scratch registers, the handler
working set.  Lock registers (5.5) back LDx_L/STx_C - already modeled
(lock_valid/lock_addr in ISS, r_link_reg in RTL).

**IPR-design correction found here** : the 21064 has NO PTBR, NO
VPTPTR, NO WHAMI IPR.  page-table base + virtual-page-table pointer
are SOFTWARE conventions kept in PAL_TEMP by the OSF PALcode (TB fill
is pure PAL software: physical HW_LD the PTE, then write TB_TAG +
xTB_PTE); WHAMI is a system/chip-specific value, not a 21064 IPR.
axpcore's phase-1 enum invented PTBR/VPTPTR/WHAMI - **drop them in
phase 4** and let PAL park those values in PAL_TEMP, matching real
OSF PAL.

**IPRs** (2.7) : reached only from PALmode via the reserved opcodes,
plus PAL_TEMP scratch registers.  the manual's issue-rule chapter
(2.5/2.6, the pvc tool) exists because real chips had *uninterlocked*
n-cycle IPR hazards - **axpcore explicitly opts out: our hw_mtpr/
hw_mfpr will be serializing/interlocked ops through the existing
serialize machinery, so EBSDK-style wait-cycle rules and pvc do not
apply to us.**  (slow is fine; PAL paths are rare.)

**Boot pearl** (4.8.2) : the EBSDK "physical mode" trick - identity
mapping without page tables by fabricating a PTE on every TB miss:
PFN = VA >> 13, KRE/KWE/URE/UWE set, **GH=3 (512-page granularity
hint)**, ASM set, valid.  our tlb.sv already supports 4 page sizes,
which maps directly onto GH - so pre-VM bring-up needs no walker and
no page tables at all.

**Console services** (4.9/4.10) : EBSDK PAL carries jtopal / ldqp /
stqp / putc / rd_impure / wr_int.  our htif monitor path substitutes
for the serial port initially.

## design decisions (settled here)

1. **Personality: OSF/1 first** (Linux boots on it; ~30 PALcalls,
   all documented in the Alpha ARM appendix C + arch/alpha).  VMS
   personality (4 modes, CHMx, IPLs, queue ops) after.  qemu-palcode
   (open source, OSF-flavor, C+asm) is the reference implementation -
   note it targets EV6-style vectors/opcodes, so we adapt its logic
   to our EV4 map rather than porting verbatim.
2. **Entry map: EV4 (figure 2-1) verbatim** - the in-repo manual then
   doubles as our documented spec.
3. **HW opcodes: EV4 (21064) encodings** - because gas already
   assembles them (`-m21064`: hw_mfpr, hw_mtpr, hw_ld, hw_st,
   hw_rei).  free toolchain support; encodings verifiable by
   assembling + objdumping.
4. **IPR numbering: ours.**  substrate is chip-private by design.
   initial set: PS (mode + IPL), PAL_BASE, EXC_ADDR, PTBR, VPTPTR,
   TB write ports (ITB_PTE/DTB_PTE + tag), TBIA/TBIS/TBISI/TBISD,
   ICNT/CYCLE (rpcc backing), WHAMI, SIRR (software interrupts),
   PAL_TEMP[0..31] scratch.  extend as PAL needs.
5. **PS format: OSF/1** - current-mode bit + 3-bit IPL (exact layout
   per ARM when phase 1 lands).
6. **dual syscall story preserved** : `syscall_emu=1` keeps today's
   call_pal 0xb0 -> MONITOR htif fast path (user-mode co-sim stays
   exactly as-is).  `syscall_emu=0` = real dispatch to PAL_BASE
   vectors.  same knob the harness already has.

## the step-by-step plan

### phase 1 - ISS grows PALmode - **DONE**
- alpha_state_t grew: pal_loaded, palmode, ipr[64] (IPR_* enum in
  alpha_interp.hh); enter_pal() helper does the save-EXC_ADDR /
  set-palmode / redirect dance
- the five reserved PAL opcodes execute (OPCDEC if not palmode):
  0x19 hw_mfpr, 0x1d hw_mtpr, 0x1b hw_ldq, 0x1f hw_stq, 0x1e hw_rei.
  hw_rei restores pc from EXC_ADDR, palmode from EXC_ADDR<0>
- CALL_PAL vectors when pal_loaded (func<7> -> region, func<5:0> ->
  64B slot); OPCDEC/unimplemented ops vector to PAL_OPCDEC.  without a
  PAL image every path is byte-for-byte the old behavior
- alpha_iss: `-p/--palcode <elf>` loads the image; PAL_BASE = e_entry
  (the PT_LOAD vaddr is 0 since it covers the ELF headers - use the
  entry, not the segment base)
- pal/: pal_macros.h (hw_* as .long, IPR/offset .equ constants),
  toy_pal.S (reset/OPCDEC/pal_add-0x91/pal_memtest-0x92), phase1_test.c
- exit criteria MET: phase1_test built -DPAL_MODE=1 (results via PAL)
  vs =0 (native) produce byte-identical output; all existing tests +
  RTL cosim + csmith unchanged (pal_loaded defaults false)

**phase-1 gotchas (paid for, recorded):**
- this binutils gates the named hw_* mnemonics off entirely - emit the
  reserved PAL opcodes as .long; objdump still prints pal19/1b/1d/1e/1f
- gas-alpha reserves `.set` for mode directives (noat/reorder) - use
  `.equ NAME, val` for symbol constants
- **0xb0 (htif) stays inline even under PAL dispatch** - it's the
  substrate's own console/co-sim transport (the manual's putc console
  service), not a guest-visible PAL call
- **PAL must save/restore every guest register it touches** through
  PAL_TEMP - the first toy OPCDEC handler clobbered r1 and corrupted
  the guest's .text via a mis-based store; the ISS modeled the clobber
  faithfully and the corruption pointed straight at the bug

build the toy PAL + test:
```
alpha-linux-gnu-gcc-10 -c -x assembler-with-cpp pal/toy_pal.S -o pal/toy_pal.o
alpha-linux-gnu-ld -Ttext=0x10000 pal/toy_pal.o -o pal/toy_pal
AF="-mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles -static -Wl,-Ttext-segment=0x20000000"
alpha-linux-gnu-gcc-10 $AF -DPAL_MODE=1 alpha_start.S pal/phase1_test.c -o pal/phase1_pal
./alpha_iss -f pal/phase1_pal -p pal/toy_pal
```

### phase 2 - real PAL software, ISS-only
- write axp-pal.S/C in-repo (structure cribbed from qemu-palcode,
  vectors per our EV4 map): reset, OPCDEC, unaligned, arith stubs;
  OSF CALL_PAL set Linux needs (callsys, swpctx, rti/rtsys, swpipl,
  rdps, wrusp, rdusp, whami, wrvptptr, tbi, imb, rduniq/wruniq, halt)
- console: putc via htif monitor block (EBSDK's serial-port role)
- pal image linked at a fixed physical base; loader places it and
  sets PAL_BASE before releasing the "cpu"
- exit criteria: freestanding tests run *unmodified* under
  ISS+real-PAL with syscall_emu=0, byte-identical output to the
  syscall_emu=1 path.  csmith batch too.

### phase 3 - RTL substrate (this replaces sweep stage 3)
- the riscv CSR block's *shape* is exactly what the IPR file needs:
  serializing read/write of a side register file + a trap-entry
  redirect + a return-from-trap redirect.  rebuild it as IPRs:
  - decode: hw_mfpr/hw_mtpr/hw_ld/hw_st/hw_rei (gated on palmode ->
    else OPCDEC), CALL_PAL -> vector dispatch when syscall_emu=0
  - exec: IPR read mux / write path (replacing CSR read mux / write
    always_ff); hw_rei = the MRET pattern (restart at EXC_ADDR,
    palmode <- EXC_ADDR<0> convention)
  - core: ARCH_FAULT state already drains + redirects - point it at
    PAL_BASE + offset(cause) and set palmode instead of mtvec math;
    cause_t becomes the EV4 offset table
  - fetch: palmode bypasses ITLB (currently moot, matters phase 4)
- exit criteria: RTL+PAL lockstep vs ISS+PAL (the checker needs zero
  changes - the ISS executes the same PAL instructions), all
  regressions green in both syscall_emu modes

### phase 4 - memory management (the architected way)
- TB-miss entries go live: ITB/DTB miss vector to PAL, fill via
  hw_mtpr TB_PTE/TB_TAG into tlb.sv (add a PAL fill port + GH
  support mapping to the existing 4 page sizes; ASN deferred - flush
  on swpctx initially)
- first: EBSDK physical-mode identity fill (4.8.2 fabricated-PTE
  algorithm, GH=512 pages) - boots everything with zero page tables
- then: OSF VM per the ARM - VPTPTR single-load fast path in the
  DTB-miss handler, 3-level walk in the slow path
- **delete mmu.sv/mmu_cache.sv here** (the hardware walker's
  retirement party)
- exit criteria: a paging test (map/unmap/protection-fault) passing
  in lockstep; csmith unchanged

### phase 5 - interrupts
- timer -> interrupt entry 0x00E0, PS.IPL masking, swpipl/rti flows,
  a0-a2 describe the interrupt to the OS per EBSDK convention
- exit criteria: preemptive-tick test in lockstep

### phase 6 - the OS ladder
- minimal console (putc/getc/jtopal over htif), then arch/alpha
  Linux early boot as the OSF-personality proof
- qemu-system-alpha (+qemu-palcode) as behavioral cross-check
- VMS personality afterward : CHMx, 4 modes, IPL model, queue-op
  PALcalls, and an SRM-ish console - the long game.  VMS or bust.

## validation strategy throughout
- the ISS executes PAL instructions natively, so lockstep co-sim
  covers PAL flows with no checker changes
- keep both syscall_emu modes green at every phase (the fast htif
  path is also the bring-up debugger)
- formal: extend the decode proof - hw_* opcodes must decode to II
  when palmode=0 (the OPCDEC guarantee, provable the same way the
  zero-register invariant was)
