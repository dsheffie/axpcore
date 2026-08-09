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

## next

1. more ISS validation: random diff-testing vs qemu-alpha (csmith with
   cross glibc if libc6.1-alpha-cross installs, else generated
   freestanding tests); directed stw/ldl_l/stl_c tests
2. RTL decode: `decode_riscv.sv` -> `decode_alpha.sv` (6-bit opcode +
   function field), uop.vh opcode relabel.  reuse the cmov crack for
   register-form CMOVxx; literal form is a single 2-source uop
3. exec.sv ALU relabel + byte-zapper unit; delete divider/AMO paths;
   STx_C success polarity flip (alpha writes 1) in nu_l1d.sv
4. fetch: branch displacement = sext(disp21)<<2 (replaces B/J-type
   scrambles); predecode from opcode + jmp hint bits (pd taxonomy maps
   1:1, see scoping discussion)
5. harness: top.cc checker to alpha (retire compare loop is reusable),
   loadelf EM_ALPHA, hand-rolled disassembler (capstone has no alpha)
6. deferred: privilege model decision (PALcode vs retargeted hardware
   walker) - only after user-mode co-sim is clean

## conventions

- misaligned access: EV4 traps on any misalignment; ISS currently does
  host-native access (user-mode binaries from gcc avoid misalignment;
  revisit when RTL alignment traps are modeled)
- /V overflow-trap variants execute non-trapping (gcc doesn't emit them)
- IMPLVER returns 1, AMASK advertises BWX only
