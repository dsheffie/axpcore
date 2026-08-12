#ifndef __ALPHA_FPU_HH__
#define __ALPHA_FPU_HH__

/* alpha FP via Berkeley SoftFloat : deterministic, host-independent,
 * explicit per-op rounding.  the FP register file holds values in
 * "register format" :
 *   T (double)  : register bits == IEEE double, used directly
 *   S (single)  : the single's VALUE held as a double (LDS expands
 *                 mem-single -> register-double; STS narrows back).
 *                 f32_to_f64 / f64_to_f32 are exactly that expand /
 *                 narrow, so S ops read the operand with f64_to_f32
 *                 and write the result with f32_to_f64.
 *   Q (integer) : two's-complement in the low 64 bits (our convention,
 *                 CVTTQ/CVTQx agree; longword uses the scrambled
 *                 <63:62>,<58:29> layout handled inline for CVTLQ/QL).
 *
 * rounding qualifier is instruction bits <12:11> (func<7:6>) :
 *   0 /C chopped  1 /M minus-inf  2 normal(nearest-even)  3 /D dynamic
 * dynamic reads FPCR<59:58>.  VAX rounding (ties-away) lands in the
 * VAX path when 0x15 is filled. */

extern "C" {
#include "softfloat.h"
}

static inline float64_t sf_f64(uint64_t b) {
  float64_t x;
  x.v = b;
  return x;
}
static inline uint64_t sf_bits64(float64_t x) {
  return x.v;
}
static inline float32_t sf_f32(uint32_t b) {
  float32_t x;
  x.v = b;
  return x;
}

/* decode the Alpha rounding qualifier (func<7:6>) into a softfloat
 * rounding mode, resolving dynamic (/D) via FPCR<59:58>. */
static inline uint_fast8_t alpha_sf_round(uint32_t func, uint64_t fpcr) {
  uint32_t rq = (func >> 6) & 3;
  if(rq == 3) {
    rq = (fpcr >> 58) & 3;
    /* dynamic : 0=chop 1=minus 2=normal 3=plus */
    switch(rq)
      {
      case 0: return softfloat_round_minMag;
      case 1: return softfloat_round_min;
      case 2: return softfloat_round_near_even;
      default: return softfloat_round_max;
      }
  }
  /* static : 0=chop 1=minus 2=normal (3 handled above) */
  switch(rq)
    {
    case 0: return softfloat_round_minMag;
    case 1: return softfloat_round_min;
    default: return softfloat_round_near_even;
    }
}

/* read an S operand out of a register (register holds the value as a
 * double; narrow to the single it represents) */
static inline float32_t sf_reg_to_s(uint64_t regbits) {
  return f64_to_f32(sf_f64(regbits));
}
/* write an S result back to register format (expand single->double) */
static inline uint64_t sf_s_to_reg(float32_t x) {
  return sf_bits64(f32_to_f64(x));
}

#endif
