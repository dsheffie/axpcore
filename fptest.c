/* FP correctness test : real IEEE S/T arithmetic, sqrt, converts, and
 * FIX register moves, printed as hex bit patterns.  runs under both
 * qemu-alpha (reference) and alpha_iss (softfloat) - diff the output.
 * built -mfp-rounding-mode=d etc. to exercise rounding, but the C is
 * plain; gcc emits the FP ops.
 *
 * build:
 *  alpha-linux-gnu-gcc-10 -mcpu=ev6 -O2 -static fptest.c -o fptest
 *  (ev6 so gcc emits sqrt / ftoit / itofs directly)
 */
#include <stdint.h>
#include <stdio.h>
#include <math.h>

static uint64_t db(double d) {
  uint64_t u;
  __builtin_memcpy(&u, &d, 8);
  return u;
}
static uint32_t sb(float f) {
  uint32_t u;
  __builtin_memcpy(&u, &f, 4);
  return u;
}

int main(void) {
  volatile double a = 3.14159265358979, b = 2.71828182845905;
  volatile float fa = 1.4142135f, fb = 0.5772156f;

  /* T arithmetic */
  printf("addt %016lx\n", db(a + b));
  printf("subt %016lx\n", db(a - b));
  printf("mult %016lx\n", db(a * b));
  printf("divt %016lx\n", db(a / b));
  printf("sqrtt %016lx\n", db(sqrt(a)));

  /* S arithmetic */
  printf("adds %08x\n", sb(fa + fb));
  printf("muls %08x\n", sb(fa * fb));
  printf("divs %08x\n", sb(fa / fb));
  printf("sqrts %08x\n", sb(sqrtf(fa)));

  /* converts */
  printf("cvtts %08x\n", sb((float)a));
  printf("cvtst %016lx\n", db((double)fa));
  printf("cvttq %ld\n", (long)(a * 1000.0));
  printf("cvtqt %016lx\n", db((double)1234567));
  printf("cvtqs %08x\n", sb((float)987654));

  /* FIX register moves : reinterpret bits (ftoit/itoft) */
  volatile uint64_t raw = db(a);
  double back;
  __builtin_memcpy(&back, (void*)&raw, 8);
  printf("itoft/ftoit %016lx\n", db(back));

  /* compares + branches */
  printf("cmp %d %d %d\n", a < b, a == a, b <= a);

  /* rounding-sensitive : a value that rounds differently per mode */
  double x = 1.0;
  for(int i = 0; i < 60; i++) {
    x = x / 3.0 * 3.0;
  }
  printf("roundchain %016lx\n", db(x));

  /* accumulate a chain so a single wrong rounding shows */
  double acc = 0.0;
  for(int i = 1; i <= 5000; i++) {
    acc += 1.0 / (double)i;
    acc = sqrt(acc * acc + 1e-9);
  }
  printf("harmonic %016lx\n", db(acc));

  return 0;
}
