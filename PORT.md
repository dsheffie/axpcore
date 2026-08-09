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

## next : the switchover (first alpha co-sim)

1. fetch/predecode : sext21<<2 displacement extraction in l1i_2way,
   predecode from opcode + jmp hint bits (pd taxonomy maps 1:1)
2. core.sv : instantiate decode_alpha instead of decode_riscv; define
   `ALPHA; delete div/AMO paths at this point
3. harness : loadelf EM_ALPHA + alpha reset path, top.cc checker
   drives execAlpha (retire-compare loop is reusable), hand-rolled
   disassembler (capstone has no alpha backend)
4. keep widening ISS csmith coverage (more seeds, -O0/-Os, ev4-only /
   ev56 variants); directed stw/ldl_l/stl_c tests
5. call_pal rduniq/wruniq (0x9e/0x9f) currently II - needed for TLS
   (static glibc errno) when the RTL runs glibc binaries
6. deferred : privilege model decision (PALcode vs retargeted hardware
   walker) - only after user-mode co-sim is clean

## conventions

- misaligned access: EV4 traps on any misalignment; ISS currently does
  host-native access (user-mode binaries from gcc avoid misalignment;
  revisit when RTL alignment traps are modeled)
- /V overflow-trap variants execute non-trapping (gcc doesn't emit them)
- IMPLVER returns 1, AMASK advertises BWX only
