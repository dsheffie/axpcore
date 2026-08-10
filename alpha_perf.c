/* print-light IPC profile : phases isolating one behavior each, timed
 * with rpcc, results buffered and printed once at the end so the
 * cache-flushing monitor path stays out of the measurement.
 *
 * run with -c false : rpcc legitimately diverges from the ISS (cycles
 * vs icnt).
 *
 * build:
 *  alpha-linux-gnu-gcc-10 -mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles \
 *    -static -Wl,-Ttext-segment=0x20000000 alpha_start.S alpha_perf.c \
 *    -o alpha_perf
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
  char buf[40];
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

static inline uint64_t rpcc(void) {
  uint64_t v;
  asm volatile("rpcc %0" : "=r"(v));
  return v;
}

static inline uint64_t xorshift64(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

#define ITERS 20000

uint64_t data[1024];
uint64_t cycles[16];
uint64_t sink;

int main(void) {
  uint64_t r = 0x123456789abcdef1UL;
  uint64_t t0, a, b, c, d;

  for(int i = 0; i < 1024; i++) {
    r = xorshift64(r);
    data[i] = r;
  }
  /* pointer-chase ring, L1 resident, stride shuffled */
  for(int i = 0; i < 1024; i++) {
    data[i] = (uint64_t)&data[(i * 41 + 7) & 1023];
  }

  /* phase 0 : independent int ops - machine width */
  a = 1; b = 2; c = 3; d = 4;
  t0 = rpcc();
  for(int i = 0; i < ITERS; i++) {
    asm volatile("addq %0, 1, %0\n\t"
		 "addq %1, 2, %1\n\t"
		 "xor %2, 5, %2\n\t"
		 "addq %3, 3, %3\n\t"
		 "addq %0, 7, %0\n\t"
		 "xor %1, 9, %1\n\t"
		 "addq %2, 4, %2\n\t"
		 "addq %3, 8, %3"
		 : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
  }
  cycles[0] = rpcc() - t0;
  sink += a + b + c + d;

  /* phase 1 : one dependent add chain - forwarding latency */
  a = 1;
  t0 = rpcc();
  for(int i = 0; i < ITERS; i++) {
    asm volatile("addq %0, 1, %0\n\t"
		 "addq %0, 2, %0\n\t"
		 "addq %0, 3, %0\n\t"
		 "addq %0, 4, %0\n\t"
		 "addq %0, 5, %0\n\t"
		 "addq %0, 6, %0\n\t"
		 "addq %0, 7, %0\n\t"
		 "addq %0, 8, %0"
		 : "+r"(a));
  }
  cycles[1] = rpcc() - t0;
  sink += a;

  /* phase 2 : predictable branches (period-2 pattern) */
  a = 0;
  t0 = rpcc();
  for(int i = 0; i < ITERS*4; i++) {
    if(i & 1) {
      a += 3;
    }
    else {
      a ^= 5;
    }
  }
  cycles[2] = rpcc() - t0;
  sink += a;

  /* phase 3 : data-random branches - mispredict cost */
  a = 0;
  r = 0x9e3779b97f4a7c15UL;
  t0 = rpcc();
  for(int i = 0; i < ITERS*2; i++) {
    r = xorshift64(r);
    if(r & 1) {
      a += r;
    }
    else {
      a ^= r;
    }
  }
  cycles[3] = rpcc() - t0;
  sink += a;

  /* phase 4 : independent register-form cmov - crack throughput */
  a = 1; b = 2; c = 3; d = 4;
  t0 = rpcc();
  for(int i = 0; i < ITERS; i++) {
    asm volatile("cmoveq %0, %1, %1\n\t"
		 "cmovne %2, %3, %3\n\t"
		 "cmovge %1, %0, %0\n\t"
		 "cmovlt %3, %2, %2\n\t"
		 "cmoveq %0, %1, %1\n\t"
		 "cmovne %2, %3, %3\n\t"
		 "cmovge %1, %0, %0\n\t"
		 "cmovlt %3, %2, %2"
		 : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
  }
  cycles[4] = rpcc() - t0;
  sink += a + b + c + d;

  /* phase 5 : zapper chain - extbl/insbl mix on pipe 0 */
  a = 0x1122334455667788UL; b = 3;
  t0 = rpcc();
  for(int i = 0; i < ITERS; i++) {
    asm volatile("extbl %0, %1, %0\n\t"
		 "insbl %0, 2, %0\n\t"
		 "extwl %0, 1, %0\n\t"
		 "zapnot %0, 0x3f, %0\n\t"
		 "extbl %0, %1, %0\n\t"
		 "insbl %0, 2, %0\n\t"
		 "extwl %0, 1, %0\n\t"
		 "zapnot %0, 0x3f, %0"
		 : "+r"(a) : "r"(b));
  }
  cycles[5] = rpcc() - t0;
  sink += a;

  /* phase 6 : L1-resident pointer chase - load-to-use latency */
  {
    uint64_t *p = &data[0];
    t0 = rpcc();
    for(int i = 0; i < ITERS*4; i++) {
      p = (uint64_t*)*p;
    }
    cycles[6] = rpcc() - t0;
    sink += (uint64_t)p;
  }

  /* phase 7 : streaming stores */
  t0 = rpcc();
  for(int i = 0; i < ITERS*4; i++) {
    sink += i;
    ((volatile uint64_t*)data)[i & 1023] = i;
  }
  cycles[7] = rpcc() - t0;

  /* phase 8 : dependent mulq chain - multiplier latency */
  a = 3;
  t0 = rpcc();
  for(int i = 0; i < ITERS; i++) {
    asm volatile("mulq %0, 3, %0\n\t"
		 "mulq %0, 5, %0\n\t"
		 "mulq %0, 7, %0\n\t"
		 "mulq %0, 9, %0"
		 : "+r"(a));
  }
  cycles[8] = rpcc() - t0;
  sink += a;

  print_hex("p0_ilp   ", cycles[0]);
  print_hex("p1_dep   ", cycles[1]);
  print_hex("p2_brpred", cycles[2]);
  print_hex("p3_brrand", cycles[3]);
  print_hex("p4_cmov  ", cycles[4]);
  print_hex("p5_zapper", cycles[5]);
  print_hex("p6_chase ", cycles[6]);
  print_hex("p7_store ", cycles[7]);
  print_hex("p8_mulq  ", cycles[8]);
  print_hex("sink     ", sink);
  terminate(0);
  return 0;
}
