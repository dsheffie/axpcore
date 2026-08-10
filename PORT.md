# axpcore : alpha (EV4 integer subset + BWX) port of rv64core

Fork of rv64core; branch `alpha`. No floating point. Target ISA:
EV4 base integer + BWX byte/word extension (+ CIX bit counts in the
ISS because they were free).

## status (2026-08-09)

**done**
- baseline carried over: rv64core @ hacky_fp32_for_quake + uncommitted
  fp32 ISS work + the cracked-cmov proof (custom-0 cmov.eqz/nez cracked
  into two 2-source uops at DQ push, predicate bit beside the int PRF
  bank, silent retire of the low uop; validated in rv64 co-sim - this
  machinery is the model for alpha CMOVxx)
- toolchain: `alpha-linux-gnu-gcc-10` + binutils (ubuntu cross
  packages), `qemu-alpha` (qemu-user) as reference model
- **alpha ISS**: `alpha_interp.{hh,cc}` + standalone driver
  `alpha_iss.cc`.  user-mode, flat 8GB image (alpha static images link
  at 0x120000000), linux/alpha callsys (exit/read/write) so the same
  binary runs under qemu-alpha for diff-testing
- first directed test `alpha_test.c` + `alpha_start.S` (freestanding
  crt).  three codegen variants all produce output **identical to
  qemu-alpha**:
  - `-mcpu=ev4` : pure byte-zapper codegen (full msk/ins/ext bwlq x l/h,
    zap/zapnot, ldq_u/stq_u)
  - `-mcpu=ev4 -mbwx` : mixed
  - `-mcpu=ev56` : BWX codegen (ldbu/ldwu/stb, sextb/sextw)
  plus cmov, mulq/mull/umulh, compares, scaled add/sub, all branches,
  jsr/ret

build:
```
g++ -O2 -std=c++17 -o alpha_iss alpha_iss.cc alpha_interp.cc -lboost_program_options
alpha-linux-gnu-gcc-10 -mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles -static \
    alpha_start.S alpha_test.c -o alpha_test
qemu-alpha ./alpha_test > a.txt ; ./alpha_iss -f alpha_test > b.txt ; diff a.txt b.txt
```

## csmith diff-testing (running)

- static glibc binaries work under the ISS: linux/alpha process ABI
  (argc/argv/auxv stack block) + the syscall set a static hello needs
  (list captured with `qemu-alpha -strace`): brk, mmap(anon), mprotect,
  fstat64, writev, uname, set_tid_address, prlimit64, readlinkat,
  getrandom(deterministic), exit_group, ...
- **scoping discovery: alpha integer division goes through the FPU.**
  libgcc's `__divqu`/`__remqu` convert to T-float, divide, convert
  back, and integer-correct.  the ISS grew a minimal fp subset for
  this (ldt/lds/stt/sts, cpys*, fcmov*, cvtqt/cvttq/cvtts/cvtqs,
  add/sub/mul/div s/t, cmpt*, fp branches, fpcr w/ rounding modes).
  consequence for the no-FP RTL core: glibc-compiled code cannot run
  without either this fp-divide subset in hardware or a soft-float
  userland (`-msoft-float` + soft-float libgcc; freestanding tests are
  fine either way).
- `./csmith_diff.sh N seed0` : csmith -> alpha gcc (ev4+bwx, -O2,
  static glibc) -> run under qemu-alpha and alpha_iss -> diff stdout +
  exit codes.  first batches passing.

## decode_alpha.sv (done, not yet wired into core.sv)

- drop-in port shape of decode_riscv; lint-clean.  key conventions
  (documented in the file header):
  - one opcode_t per alpha instruction : lit8 form clears srcB_valid,
    literal rides in rvimm, exec will mux opB = srcB_valid ? srcB : rvimm
  - reuses semantically-identical rv64 enums (ADDU=addq ADDW=addl
    SH2ADD=s4addq ANDN=bic ORN=ornot XNOR=eqv MUL/MULW/MULHU
    SLT/SLTU LB../SD LRW/SCW JAL/JALR/JR/RET ...); ~45 new enums in
    uop.vh (scaled l/sub, cmpeq/cmple/cmpule/cmpbge, ble/bgt/blbc/blbs,
    23 zapper ops, CMOV_LO/CMOV_LIT, LDQU/STQU)
  - alpha branches get their own test-vs-zero enums
    (BEQZ/BNEZ/BLTZ/BGEZ/BLEZ/BGTZ/BLBC/BLBS) so exec supports both
    ISAs additively - the rv64 build keeps working as a regression
    baseline until the switchover
  - r31 = zero reg : dst_valid=(r!=31), r31-dest ops fold to NOP
  - cmov condition in imm[2:0] (0=eq 1=ne 2=lt 3=ge 4=le 5=gt 6=lbs
    7=lbc); register form emits the CMOV_LO crack marker, literal form
    is single-uop CMOV_LIT (srcB = old rc)
  - fault/irq slots park on unallocated opcode 0x01 (0x00 is CALL_PAL!)
  - call_pal 0x83 -> MONITOR (syscall_emu), 0x86 imb -> FENCEI
- unit test : tb_decode_alpha.{sv,cc} runs every .text word of the
  three compiled test binaries through the decoder, checks against an
  independent opcode-map model.  924 words, 0 unexpected II, 0
  classification mismatches.
  build: verilator -Mdir tb_obj --cc --exe --build tb_decode_alpha.sv
  tb_decode_alpha.cc (link needs LDFLAGS="-flto -O2" - the installed
  verilated.mk compiles with -flto)

## exec.sv alpha support (done, additive - rv64 regressions still green)

- opB idiom wired through both pipes : addsub B mux, comparator wires,
  reg-form shift amounts, multiplier srcB all take
  `w_opB = srcB_valid ? t_srcB : rvimm` (riscv-safe : riscv reg ops
  always have srcB_valid=1)
- new arms both pipes : Z-branches (single-source, outside the
  TWO_SRC_CHEAP guard on pipe1), CMPEQ/CMPLE/CMPULE, scaled l/sub
  variants via extended w_srcA_shl muxes
- alpha_zapper.sv : ext/ins/msk x bwlq x l/h, zap/zapnot, cmpbge in
  one unit on pipe 0 (byte-granular shifts = cheap muxes), one grouped
  case arm consumes it
- CMOV_LO/CMOV_LIT arms + alpha_cmov_cond() (8 conditions from
  imm[2:0]); predicate write and the core.sv crack now also key on
  CMOV_LO
- agu : LDQU/STQU arms (addr low 3 bits cleared, plain MEM_LD/MEM_SD)
- nu_l1d stx_c success polarity behind `ALPHA (stub define in
  machine.vh, off until switchover)
- validated : full build + rv64 cmov_test (crack regression) + csmith
  test-0 co-sim all clean with the alpha machinery compiled in
- gotcha : don't build tb_decode_alpha into obj_dir (the main Makefile
  globs obj_dir/*.o) - use -Mdir tb_obj

## THE SWITCHOVER IS DONE : alpha RTL runs compiled code in co-sim

`make` with `` `define ALPHA `` (machine.vh, now on) builds the alpha
core.  first co-sim milestone: `./rv64_core -f alpha_cosim_test` runs
42302 instructions in lockstep with execAlpha - all four checksums
identical to the ISS/qemu reference, **zero checker mismatches,
final 4GB memory-image compare clean**.

what the switchover added:
- l1i_2way : branch/jal displacement = sext21<<2 (+4 folded), behind
  `ALPHA; predecode.sv alpha variant classifies from opcode + jmp
  hint bits (pd taxonomy 1:1, no abi inference)
- core.sv : decode_alpha instantiated under `ALPHA
- harness : is_alpha_elf/load_alpha_elf (segments + tohost symtab
  scan, no trampoline, entry direct; binaries must link
  -Wl,-Ttext-segment=0x20000000 to fit the 4GB flat image), top.cc
  alpha checker branch (pc + 32-gpr lockstep vs execAlpha, port a+b),
  wr_log store-compare bypassed for now
- co-sim syscall convention : call_pal 0xb0 -> MONITOR; magic-mem
  htif block + tohost, results via memory (the existing ISA-neutral
  handle_syscall in syscall.cc serves it untouched).  ISS implements
  0xb0 with deterministic arch effects (cosim_driven skips host i/o)
- alpha_start.S sets its own gp/sp (stack_end symbol from the C file)
  so the same binary runs on bare RTL, alpha_iss, and qemu
- bug found by first light : the AND/OR/XOR-family arms had not been
  retrofitted to w_opB (literal forms read garbage) - the checker
  caught it at the first literal AND, 2228 instructions in

## regressions

- rv64 : comment out `` `define ALPHA `` in machine.vh and rebuild;
  cmov_test + csmith test-0 were green at the switchover commit
- alpha : ./rv64_core -f alpha_cosim_test (checker on by default)
- 0x83-convention binaries (alpha_test, csmith/glibc) are for
  ISS-vs-qemu only - on RTL the callsys registers are invisible to
  the monitor path

## IPC profile (alpha_perf.c, checker off - rpcc diverges by design)

per-phase IPC from one binary : ISS run gives icnt per phase (rpcc =
icnt there), RTL run gives cycles.  `sink` matches ISS<->RTL, a free
correctness check.

- independent ALU : **2.00** (full width) ; streaming stores : 2.00
- dependent add chain : 1.25 = theoretical bound (1-cycle forwarding)
- dependent zapper chain : 1.25 (single-cycle, fully forwarded)
- L1-hit pointer chase : 2-cycle load-to-use
- 50% random branches : ~3.7 extra cycles per mispredict
- register-form cmov : 0.71 insn-IPC / ~1.06 uop-IPC (2 uops by
  design + the slot-1 crack-defer bubble; literal forms are 1 uop)
- dependent mulq : ~5 cyc effective vs MUL_LAT=3 - scheduler wakeup
  gap for the mul unit, worth a look someday

conclusion : the 0.147 IPC on alpha_cosim_test was entirely the
serializing cache-flush in the MONITOR path (each htif print walks
all of L2).  the core itself is healthy.

## csmith-on-RTL fuzzing (LIVE BUG UNDER INVESTIGATION)

pipeline : csmith -> host gcc run (checksum oracle) -> alpha_iss
(golden) -> rv64_core co-sim (checker on).  needs the freestanding
shim (shim/ : mini printf/string/assert + htif crt) and the
**integer division helpers in the alpha division ABI**
(shim/alpha_div.S : dividend $24, divisor $25, result $27, ra $23 -
libgcc's versions use the FPU).  `./csmith_rtl.sh N seed0`.

**first 30-seed batch : 19 pass, 8 fail, 0 iss-vs-host fails.**
every failure is deterministic and sits in csmith's crc32 kernel.

### the bug : FOUND AND FIXED (all 8 seeds now pass, 27/27 batch)

root cause : **rf6r3w hardwires reads of phys reg 0 to zero** - the
rv64 x0 sink, safe there because arch x0 maps to phys 0 forever and
that tag is never freed.  alpha's r0 (v0) is a real register : its
reset mapping (phys 0) gets renamed away, freed, and eventually
REALLOCATED - the new owner's writes land in the array but every
read short-circuits to 0.  rare (only when the free list hands out
index 0), deterministic, value always 0 - matched every signature.

fix : under `ALPHA the reset RATs swap r31 <-> r0 mappings : arch
r31 (alpha's zero reg) -> phys 0, never renamed so never freed, and
the read shortcut becomes exactly the r31-zero implementation; arch
r0 -> phys 31, renames normally (no read shortcut on tag 31).

found with cycle-windowed $display instrumentation (operand tags +
values + fwd selects + all prf writes) : the log showed extbl
renamed to dst tag 0, result 0x2e computed, and the consumer reading
[0]=0 with no intervening writer - then the grep for ptr-0 special
cases in rf6r3w.

### the hunt (kept for methodology)

symptom (seed 2016, `csmith_alpha_rtl/rtlfail_2016.c`) : `extbl
a0,0x6,t3` retires 0x2e (RETIRE DATA PROVES THE EXEC RESULT AND PRF
WRITE VALUE WERE CORRECT).  19 cycles later - no flush between - `xor
t0,t3,t3` reads that phys reg as 0 (rtl result == t0 exactly).

exonerated by experiment :
- alpha_zapper unit (tb_zapper : 98k vectors vs C model, clean)
- the cmov crack (`-fno-if-conversion` build still fails)
- dual-issue (SECOND_EXEC_PORT off still fails -> shared path)
- stores (alpha ISS now feeds the wr_log store queue : every
  committed store matches (pc,addr,data) up to the divergence)
- the crc kernel in isolation co-sims clean (crc_repro) ; a
  store-after-mispredicted-loop micro test is clean too - the bug
  needs the fuller csmith context (deep dependent chains + loads +
  mispredict pressure)

next moves :
1. add FST wave dumping to top.cc (cycle-windowed), capture the
   19-cycle window around the f2016 divergence (cycle ~82726-82745),
   read the rf6r3w ports + forwarding selects for the consumer
2. suspects still standing : a wrong-path uop's PRF write landing
   after recovery reallocation, scheduler operand capture vs
   writeback race on pipe0, load-forwarding false hit
3. `creduce` caution : the loose "any mismatch" predicate drifted the
   reduction into UB (wild >4GB loads = a DIFFERENT divergence).
   pin the predicate to the original pc/symptom next time

### side findings
- TWO_SRC_CHEAP off wedges the alpha build (decode cheap-marking
  assumes it) ; PERFECT_L1_CACHES doesn't compile with alpha changes.
  both noted, neither blocking
- checker strengthened : every-retire 32-gpr compare on BOTH ports
  (was port-a + pc+4-only) ; rpcc divergence accepted via did_rpcc
  (perf binaries co-sim checked now) ; end-of-run memdiff dump (first
  16 differing qwords - beware post-abort artifacts)

## all-AXP tree + formal zero-register proof

- every `` `ALPHA `` ifdef is gone : the tree builds only the alpha
  core (decode_riscv.sv dropped from SV_SRC, riscv branches deleted
  from predecode/l1i/nu_l1d/core).  the rv64 regression flip-back no
  longer exists by design - VMS or bust
- the phys-0 fix is unconditional : reset RATs map r31 -> phys 0
  (never renamed/freed; rf6r3w's ptr-0 read shortcut IS the r31 zero)
  and r0 -> phys 31
- **formal/ (ported from r9999)** : formal_decode.sv +
  run_decode_formal.sh prove `dst_valid |-> dst[4:0] != 31` over all
  2^32 insns x all inputs (sv2v + yosys sat, UNSAT on `bad`, with a
  non-vacuity sanity check).  this is the decode invariant that keeps
  the zero register zero.  note : yosys needs `read_verilog -sv`
  (sv2v keeps size casts)

## dead-code sweep, stage 1 (structural)

gone : the divider (nu_divider/divider files, exec plumbing through
the scheduler ready/wakeup/writeback muxes, DIV*/REM* enums,
uses_div; MAX_LAT is now MUL_LAT+3), all fp32 (fp_*.sv files, mul.sv
rewritten pure-integer, SP_*/INT_TO_SP enums+arms, MULH too), the
AMO path (exec agu arms, nu_l1d rmw alu + response arms, amo_op
field, MEM_AMO* / AMOW/AMOD enums), decode_riscv.sv deleted.
divide_ready output tied 1 for core.sv's drain state.

lesson re-learned : the amo cut initially took the neighboring
link-reg defaults with it - verilator's LATCH warning caught it
immediately.  lint after every excision.

verified : build + cosim + perf + csmith + the formal proof all green
on the slimmed tree.  l1d.sv / perfect_l1d.sv (unused alternates) now
reference removed enums - they were already stale for alpha; left in
place, not in the build.

## dead-code sweep, stage 2 (riscv arms + enums)

gone : ~58 riscv-only opcode_t enums and ~95 exec arms across both
pipes (riscv 2-reg branches, zicond, CMOV_EQZ/NEZ - the crack keys on
CMOV_LO alone now, W-ops, rotates, min/max, rev8/orcb, UW/zba
leftovers incl SH1ADD, imm-form twins, auipc/lui, LB/LH/LWU), the
support wires they owned (w_srcB_is_zero, bswap generates, the
W-shift source muxes), and the MEM_LB/LH/LWU l1d response arms in
both cache ports.  SRL/SRA/SLL stay (alpha shifts), ADDI/ANDI stay
(lda/ldah/amask), SH2ADD/SH3ADD stay (s4addq/s8addq).

- divider question resolved : alpha has NO architected divide, so the
  restored-nu_divider idea was reverted mid-flight; the shim's
  shift-subtract helpers in the division ABI remain the story
- gotcha : when the RTL shrinks, verilator emits fewer split .cpp
  files - stale obj_dir objects then double-link (the Makefile globs
  obj_dir/*.o).  rm -rf obj_dir on big RTL diets

still riscv in the tree (stage 3) : the CSR/priv block in exec +
core's WRITE_CSRS/trap states + csr_t (needed until the alpha
privilege model lands - RDCYCLE/MONITOR/FENCEI thread through it),
BREAK/ECALL-family enums, interpret.cc/top.cc riscv paths, the stale
l1d.sv/l2.sv/perfect_* alternates.

## FPU roadmap note (from dsheffie)

the mips-project FPU blocks come in eventually.  VAX modes (opcode
0x15 FLTV group) over the same datapath : F/G/D formats with biases
128/1024/128, NO denorm/inf/nan, sign=1&exp=0 = reserved operand
trap, ties-away-from-zero rounding, PDP-11 16-bit word swap in
LDF/LDG/STF/STG (plus exponent remap into the register format), D
via conversion only (CVTDG/CVTGD, G-precision arithmetic).  G is
IEEE-double-shaped with bias off by one - real EVs shared the
datapath and muxed unpack/pack/rounding by mode, which is exactly
the plan for the mips FPU port.

## next

1. sweep stage 3 : the CSR/priv block (couples to the privilege-model
   decision - PALcode vs hardware), interpret.cc riscv paths
2. cheaper monitor path (skip the L2 walk for syscalls?)
3. r9999 also has formal_l1d_fwd - port when the l1d gets attention
2. wire the alpha ISS into the store queue so wr_log store compare
   works; directed ldl_l/stl_c + stq_u co-sim tests
3. bigger co-sim runs : csmith with a freestanding print shim (glibc
   needs fp-divide + rduniq), or teach the ISS+RTL the fp-divide
   subset
4. delete the riscv paths (decode_riscv, div, AMO, CSR file) once
   comfortable; hand-rolled disassembler for reports
5. call_pal rduniq/wruniq for TLS; then the privilege model decision
   (PALcode vs retargeted walker)

## conventions

- misaligned access: EV4 traps on any misalignment; ISS currently does
  host-native access (user-mode binaries from gcc avoid misalignment;
  revisit when RTL alignment traps are modeled)
- /V overflow-trap variants execute non-trapping (gcc doesn't emit them)
- IMPLVER returns 1, AMASK advertises BWX only
