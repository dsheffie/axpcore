/* phase-1 PAL test : proves the CALL_PAL -> hw_* -> hw_rei chain and
 * OPCDEC vectoring.  output goes through the htif transport (call_pal
 * 0xb0), which stays inline even under PAL dispatch.
 *
 * run two ways, identical output expected:
 *   ./alpha_iss -f phase1_test                    (no pal : pal_add and
 *      the illegal opcode would be fatal - so this form is for the
 *      non-pal control that SKIPS them, see PAL_MODE)
 *   ./alpha_iss -f phase1_test -p pal/toy_pal     (real PAL dispatch)
 *
 * built twice: -DPAL_MODE=1 exercises the PAL path, =0 the native
 * control.  both print the same numbers, so the PAL-computed results
 * are checked against native arithmetic.
 */
#include <stdint.h>

volatile uint64_t tohost __attribute__((aligned(64)));
volatile uint64_t fromhost __attribute__((aligned(64)));
uint64_t stack[1024] __attribute__((aligned(16)));
asm(".globl stack_end\nstack_end = stack+8192");
static volatile uint64_t htif_buf[8] __attribute__((aligned(64)));

static void htif_write(const char *buf, uint64_t len) {
  htif_buf[0] = 64;
  htif_buf[1] = 1;
  htif_buf[2] = (uint64_t)buf;
  htif_buf[3] = len;
  tohost = (uint64_t)htif_buf;
  asm volatile("call_pal 0xb0" ::: "memory");
}

static void terminate(uint64_t code) {
  tohost = (code << 1) | 1;
  asm volatile("call_pal 0xb0" ::: "memory");
  for(;;);
}

static void print_hex(const char *tag, uint64_t v) {
  char buf[32];
  long n = 0;
  while(tag[n]) {
    buf[n] = tag[n];
    n++;
  }
  buf[n++] = '=';
  for(int i = 15; i >= 0; i--) {
    uint64_t nib = (v >> (4*i)) & 0xf;
    buf[n++] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
  }
  buf[n++] = '\n';
  htif_write(buf, n);
}

/* pal_add : v0 = a0 + a1, via call_pal 0x91 (unprivileged) */
static uint64_t pal_add(uint64_t a, uint64_t b) {
  register uint64_t v0 asm("$0");
  register uint64_t a0 asm("$16") = a;
  register uint64_t a1 asm("$17") = b;
  asm volatile("call_pal 0x91" : "=r"(v0) : "r"(a0), "r"(a1));
  return v0;
}

/* pal_memtest : store val to phys addr, read back, via call_pal 0x92.
 * proves hw_stq + hw_ldq. */
static uint64_t scratch_phys __attribute__((aligned(64)));
static uint64_t pal_memtest(uint64_t addr, uint64_t val) {
  register uint64_t v0 asm("$0");
  register uint64_t a0 asm("$16") = addr;
  register uint64_t a1 asm("$17") = val;
  asm volatile("call_pal 0x92" : "=r"(v0) : "r"(a0), "r"(a1) : "memory");
  return v0;
}

int main(void) {
  uint64_t acc = 0;
  for(uint64_t i = 1; i <= 200; i++) {
    uint64_t a = i * 0x1234, b = i ^ 0xdead;
#if PAL_MODE
    uint64_t s = pal_add(a, b);   /* through PAL */
#else
    uint64_t s = a + b;           /* native control */
#endif
    acc = (acc << 1 | acc >> 63) ^ s;
  }
  print_hex("acc", acc);

#if PAL_MODE
  uint64_t mt = pal_memtest((uint64_t)&scratch_phys, 0xfeedfacecafebabeUL);
#else
  scratch_phys = 0xfeedfacecafebabeUL;
  uint64_t mt = scratch_phys;
#endif
  print_hex("memtest", mt);

#if PAL_MODE
  /* trip an OPCDEC : opcode 0x03 is unallocated.  the toy PAL handler
   * skips it and returns, so execution continues. */
  asm volatile(".long 0x0c000000");
  htif_write("survived opcdec\n", 16);
#else
  htif_write("survived opcdec\n", 16);
#endif

  terminate(0);
  return 0;
}
