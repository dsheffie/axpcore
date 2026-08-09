/* first directed test for the alpha ISS : integer mix (arith, logic,
 * shifts, cmov, mul, byte zapper via unaligned access, ldq_u/stq_u,
 * bit ops) with results printed as hex over callsys write.  runs
 * identically under qemu-alpha - diff the outputs.
 *
 * build:
 *  alpha-linux-gnu-gcc-10 -mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles \
 *    -static alpha_start.S alpha_test.c -o alpha_test
 */
#include <stdint.h>

static long callsys3(long n, long a, long b, long c) {
  register long v0 asm("$0") = n;
  register long a0 asm("$16") = a;
  register long a1 asm("$17") = b;
  register long a2 asm("$18") = c;
  asm volatile("call_pal 0x83"
	       : "+r"(v0), "+r"(a0), "+r"(a1), "+r"(a2)
	       :
	       : "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$27", "$28", "memory");
  return v0;
}

static void print(const char *buf, long len) {
  callsys3(4, 1, (long)buf, len);
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
  print(buf, n);
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

  for(int i = 0; i < 256; i++) {
    r = xorshift64(r);
    bytes[i] = r;
  }
  for(int i = 0; i < 64; i++) {
    r = xorshift64(r);
    quads[i] = r;
  }

  /* arithmetic + compares + cmov (compiler emits cmovxx for ternaries) */
  for(int i = 0; i < 1000; i++) {
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

  /* unaligned loads/stores at every alignment - ldq_u/extxl/extxh or
   * bwx forms depending on what gcc picked */
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

  /* byte ops : sext + zext of loaded bytes/words */
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

  /* division-free integer idioms : umulh via __int128 */
  acc = 0;
  for(int i = 0; i < 64; i++) {
    unsigned __int128 p = (unsigned __int128)quads[i] * quads[(i + 1) & 63];
    acc ^= (uint64_t)(p >> 64);
    acc += (uint64_t)p;
  }
  print_hex("umulh", acc);

  print("alpha test done\n", 16);
  callsys3(1, 0, 0, 0); /* exit(0) */
  return 0;
}
