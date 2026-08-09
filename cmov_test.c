/* directed test for cmov.eqz / cmov.nez (custom-0, opcode 0x0b)
 *   cmov.eqz rd, rs1, rs2 : rd = (rs1 == 0) ? rs2 : rd
 *   cmov.nez rd, rs1, rs2 : rd = (rs1 != 0) ? rs2 : rd
 * rd is read as well as written - the RTL cracks these into two
 * 2-source uops.  freestanding, exits via htif tohost: 1 = pass,
 * (fails<<1)|1 = fail.  the real check is the co-sim ISS running in
 * lockstep; the self-check catches matching-but-wrong semantics.
 *
 * build:
 *  /opt/riscv64/bin/riscv64-unknown-elf-gcc -march=rv64im_zicsr -mabi=lp64 \
 *    -O2 -nostdlib -nostartfiles -Wl,-Ttext=0x80000000 -o cmov_test cmov_test.c
 */
#include <stdint.h>

volatile uint64_t tohost __attribute__((aligned(64)));
volatile uint64_t fromhost __attribute__((aligned(64)));

uint64_t stack[1024] __attribute__((aligned(16)));

static volatile uint64_t htif_buf[8] __attribute__((aligned(64)));

/* proxy syscall : tohost points at the magic-mem block, ecall raises
 * got_monitor in the harness */
static void htif_write(const char *buf, uint64_t len) {
  htif_buf[0] = 64; /* SYS_write */
  htif_buf[1] = 1;
  htif_buf[2] = (uint64_t)buf;
  htif_buf[3] = len;
  tohost = (uint64_t)htif_buf;
  asm volatile("fence\n\tecall" ::: "memory");
}

static void terminate(uint64_t code) {
  tohost = (code << 1) | 1;
  asm volatile("fence\n\tecall" ::: "memory");
  for(;;);
}

static inline uint64_t cmov_eqz(uint64_t old, uint64_t c, uint64_t v) {
  uint64_t rd = old;
  asm volatile(".insn r 0x0b, 0, 0, %0, %1, %2" : "+r"(rd) : "r"(c), "r"(v));
  return rd;
}

static inline uint64_t cmov_nez(uint64_t old, uint64_t c, uint64_t v) {
  uint64_t rd = old;
  asm volatile(".insn r 0x0b, 1, 0, %0, %1, %2" : "+r"(rd) : "r"(c), "r"(v));
  return rd;
}

/* back-to-back dependent pair - stresses predicate + value forwarding */
static inline uint64_t cmov_chain(uint64_t x, uint64_t c1, uint64_t v1,
				  uint64_t c2, uint64_t v2) {
  asm volatile(".insn r 0x0b, 0, 0, %0, %1, %2\n\t"
	       ".insn r 0x0b, 1, 0, %0, %3, %4"
	       : "+r"(x) : "r"(c1), "r"(v1), "r"(c2), "r"(v2));
  return x;
}

/* rs1 == rd : condition is the old destination itself */
static inline uint64_t cmov_eqz_self(uint64_t x, uint64_t v) {
  asm volatile(".insn r 0x0b, 0, 0, %0, %0, %1" : "+r"(x) : "r"(v));
  return x;
}

/* rs2 == rd : value is the old destination itself (result never changes) */
static inline uint64_t cmov_nez_selfval(uint64_t x, uint64_t c) {
  asm volatile(".insn r 0x0b, 1, 0, %0, %1, %0" : "+r"(x) : "r"(c));
  return x;
}

static inline uint64_t xorshift64(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

uint64_t table[64];

int main(void) {
  uint64_t fails = 0, r = 0x123456789abcdef1UL;
  uint64_t acc = 0xdeadbeefcafebabeUL;

  for(int i = 0; i < 64; i++) {
    r = xorshift64(r);
    table[i] = r;
  }

  /* basic eqz/nez, both polarities of the condition, accumulator
   * carried through so old-rd comes from the previous cmov */
  for(int i = 0; i < 2000; i++) {
    r = xorshift64(r);
    uint64_t c = (i & 3) == 0 ? 0 : (r & 0xf);
    uint64_t v = r ^ acc;
    uint64_t ref = (c == 0) ? v : acc;
    acc = cmov_eqz(acc, c, v);
    if(acc != ref) {
      fails++;
    }
    r = xorshift64(r);
    c = (i & 7) == 0 ? 0 : (r >> 60);
    v = table[r & 63];
    ref = (c != 0) ? v : acc;
    acc = cmov_nez(acc, c, v);
    if(acc != ref) {
      fails++;
    }
  }

  /* dependent chains */
  for(int i = 0; i < 1000; i++) {
    r = xorshift64(r);
    uint64_t c1 = r & 1, v1 = r >> 1;
    uint64_t c2 = (r >> 32) & 1, v2 = r * 0x9e3779b97f4a7c15UL;
    uint64_t ref = acc;
    if(c1 == 0) {
      ref = v1;
    }
    if(c2 != 0) {
      ref = v2;
    }
    acc = cmov_chain(acc, c1, v1, c2, v2);
    if(acc != ref) {
      fails++;
    }
  }

  /* rs1==rd and rs2==rd corners */
  for(int i = 0; i < 500; i++) {
    r = xorshift64(r);
    uint64_t x = (i & 3) == 0 ? 0 : r;
    uint64_t v = r ^ 0x5555555555555555UL;
    uint64_t ref = (x == 0) ? v : x;
    uint64_t got = cmov_eqz_self(x, v);
    if(got != ref) {
      fails++;
    }
    got = cmov_nez_selfval(r, x);
    if(got != r) {
      fails++;
    }
  }

  /* cmov to x0 must be a nop and not wedge the machine */
  asm volatile(".insn r 0x0b, 0, 0, x0, %0, %1" :: "r"(r), "r"(acc));
  asm volatile(".insn r 0x0b, 1, 0, x0, %0, %1" :: "r"(acc), "r"(r));

  /* data-dependent branch right before a cmov - mispredict + flush
   * interaction with the cracked pair */
  for(int i = 0; i < 1000; i++) {
    r = xorshift64(r);
    if(r & 0x10) {
      uint64_t ref = ((r & 1) == 0) ? r : acc;
      acc = cmov_eqz(acc, r & 1, r);
      if(acc != ref) {
	fails++;
      }
    }
    else {
      acc = cmov_nez(acc, r & 2, table[r & 63]);
    }
  }

  if(fails == 0) {
    htif_write("cmov pass\n", 10);
  }
  else {
    char msg[] = "cmov FAIL 0000\n";
    for(int i = 0; i < 4; i++) {
      uint64_t nib = (fails >> (4*(3-i))) & 0xf;
      msg[10+i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    htif_write(msg, 15);
  }
  terminate(fails);
  return 0;
}

__attribute__((naked, section(".text.startup"))) void _start(void) {
  asm volatile(".option push\n\t"
	       ".option norelax\n\t"
	       "la gp, __global_pointer$\n\t"
	       ".option pop\n\t"
	       "la sp, stack\n\t"
	       "li t0, 8192\n\t"
	       "add sp, sp, t0\n\t"
	       "call main\n\t"
	       "1: j 1b");
}
