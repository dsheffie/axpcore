/* co-sim variant of alpha_test.c : same integer mix, but I/O and exit
 * go through the htif convention the RTL harness understands - magic
 * mem block + tohost + call_pal 0xb0 (decoded to MONITOR).  results
 * come back in memory, so the ISS checker stays in lockstep with no
 * register injection.
 *
 * build:
 *  alpha-linux-gnu-gcc-10 -mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles \
 *    -static -Wl,-Ttext-segment=0x20000000 alpha_start.S \
 *    alpha_cosim_test.c -o alpha_cosim_test
 */
#include <stdint.h>

volatile uint64_t tohost __attribute__((aligned(64)));
volatile uint64_t fromhost __attribute__((aligned(64)));

uint64_t stack[1024] __attribute__((aligned(16)));
asm(".globl stack_end\nstack_end = stack+8192");

static volatile uint64_t htif_buf[8] __attribute__((aligned(64)));

static void htif_write(const char *buf, uint64_t len) {
  htif_buf[0] = 64; /* SYS_write */
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

static inline uint64_t xorshift64(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

uint8_t bytes[256];
uint64_t quads[64];

int main(void) {
  uint64_t r = 0x123456789abcdef1UL;
  uint64_t acc = 0;

  for(int i = 0; i < 64; i++) {
    r = xorshift64(r);
    bytes[i] = r;
    bytes[64 + i] = r >> 8;
    bytes[128 + i] = r >> 16;
    bytes[192 + i] = r >> 24;
    quads[i] = r;
  }

  /* arithmetic + compares + cmov */
  for(int i = 0; i < 500; i++) {
    r = xorshift64(r);
    int64_t a = r, b = quads[i & 63];
    acc += a + b;
    acc += (int64_t)(int32_t)(a - b);
    acc ^= (a < b) ? a : b;
    acc += (uint64_t)a < (uint64_t)b ? 1 : 0;
    acc ^= a * b;
    acc += (a >> (b & 63)) ^ ((uint64_t)a << (b & 31));
    acc ^= (a & b) | (~a & 0x5555555555555555UL);
  }
  print_hex("arith", acc);

  /* unaligned loads/stores at every alignment - zapper or bwx */
  acc = 0;
  for(int i = 0; i < 240; i++) {
    uint16_t w;
    uint32_t l;
    uint64_t q;
    __builtin_memcpy(&w, &bytes[i], 2);
    __builtin_memcpy(&l, &bytes[i], 4);
    __builtin_memcpy(&q, &bytes[i], 8);
    acc += w + l + q;
    q = acc * 0x9e3779b97f4a7c15UL;
    __builtin_memcpy(&bytes[i], &q, 2);
  }
  print_hex("unalgn", acc);

  /* byte sign/zero extension */
  acc = 0;
  for(int i = 0; i < 256; i++) {
    int8_t sb = bytes[i];
    uint8_t ub = bytes[i];
    int16_t sw;
    __builtin_memcpy(&sw, &bytes[i], 2);
    acc += sb;
    acc += ub;
    acc ^= sw;
    acc = (acc << 1) | (acc >> 63);
  }
  print_hex("bytes", acc);

  /* umulh via __int128 */
  acc = 0;
  for(int i = 0; i < 64; i++) {
    unsigned __int128 p = (unsigned __int128)quads[i] * quads[(i + 1) & 63];
    acc ^= (uint64_t)(p >> 64);
    acc += (uint64_t)p;
  }
  print_hex("umulh", acc);

  htif_write("alpha cosim done\n", 17);
  terminate(0);
  return 0;
}
