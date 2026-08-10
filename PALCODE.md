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

### phase 1 - ISS grows PALmode (no RTL, no PAL software yet)
- alpha_state_t: palmode bit, ipr[] file, PS/PAL_BASE/EXC_ADDR/...
- decode+execute the five hw_* opcodes (EV4 encodings, OPCDEC if not
  palmode); CALL_PAL + every currently-fatal event (unaligned,
  OPCDEC, arith) becomes: EXC_ADDR <- pc, palmode <- 1,
  pc <- PAL_BASE + offset (when a PAL image is loaded; without one,
  keep today's behavior)
- driver: `--palcode <image>` flag loads a PAL binary at PAL_BASE
- exit criteria: a toy PAL image (gas -m21064) that fields callsys ->
  htif print -> hw_rei, co-existing with all current tests

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
