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

### the open bug - status of the hunt

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

## next

1. FST wave window -> root-cause the phys-reg-reads-0 bug
2. cheaper monitor path (skip the L2 walk for syscalls?)
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
