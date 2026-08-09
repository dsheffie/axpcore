#ifndef __ALPHA_INTERP_HH__
#define __ALPHA_INTERP_HH__

#include <cstdint>
#include <cstring>

/* user-mode alpha (EV4 integer subset + BWX) interpreter state.
 * mirrors the shape of the rv64 state_t so the co-sim harness port is
 * mechanical : pc + 32 gprs (r31 reads as zero) + flat memory image. */
struct alpha_state_t {
  uint64_t pc;
  uint64_t last_pc;
  int64_t gpr[32];
  uint64_t icnt;
  uint64_t maxicnt;
  uint64_t unique; /* rdunique/wrunique thread pointer */
  uint8_t *mem;
  int brk;
  int exit_code;
  bool lock_valid; /* ldx_l / stx_c lock flag */
  uint64_t lock_addr;

  uint64_t load64(uint64_t pa) const {
    return *reinterpret_cast<uint64_t*>(mem + pa);
  }
  uint32_t load32(uint64_t pa) const {
    return *reinterpret_cast<uint32_t*>(mem + pa);
  }
  uint16_t load16(uint64_t pa) const {
    return *reinterpret_cast<uint16_t*>(mem + pa);
  }
  uint8_t load8(uint64_t pa) const {
    return mem[pa];
  }
  void store64(uint64_t pa, uint64_t x) {
    *reinterpret_cast<uint64_t*>(mem + pa) = x;
  }
  void store32(uint64_t pa, uint32_t x) {
    *reinterpret_cast<uint32_t*>(mem + pa) = x;
  }
  void store16(uint64_t pa, uint16_t x) {
    *reinterpret_cast<uint16_t*>(mem + pa) = x;
  }
  void store8(uint64_t pa, uint8_t x) {
    mem[pa] = x;
  }
};

/* alpha instruction formats */
union alpha_t {
  struct mem_t { /* memory format : lda/ldah/loads/stores */
    uint32_t disp : 16;
    uint32_t rb : 5;
    uint32_t ra : 5;
    uint32_t opcode : 6;
  } m;
  struct br_t { /* branch format */
    uint32_t disp : 21;
    uint32_t ra : 5;
    uint32_t opcode : 6;
  } b;
  struct op_t { /* operate format, register rb */
    uint32_t rc : 5;
    uint32_t func : 7;
    uint32_t is_lit : 1;
    uint32_t sbz : 3;
    uint32_t rb : 5;
    uint32_t ra : 5;
    uint32_t opcode : 6;
  } o;
  struct opl_t { /* operate format, 8-bit literal */
    uint32_t rc : 5;
    uint32_t func : 7;
    uint32_t is_lit : 1;
    uint32_t lit : 8;
    uint32_t ra : 5;
    uint32_t opcode : 6;
  } ol;
  struct pal_t {
    uint32_t func : 26;
    uint32_t opcode : 6;
  } p;
  uint32_t raw;
};
static_assert(sizeof(alpha_t) == 4, "alpha_t must be 4 bytes");

static inline int64_t sext32(uint64_t x) {
  return static_cast<int64_t>(static_cast<int32_t>(x));
}

static inline int64_t sext16(uint64_t x) {
  return static_cast<int64_t>(static_cast<int16_t>(x));
}

static inline int64_t sext8(uint64_t x) {
  return static_cast<int64_t>(static_cast<int8_t>(x));
}

static inline int64_t sext21(uint32_t x) {
  return (static_cast<int64_t>(x) << 43) >> 43;
}

/* 8-bit byte-select -> 64-bit byte mask, the BYTE_ZAP primitive */
static inline uint64_t byte_mask(uint32_t m) {
  uint64_t r = 0;
  for(int i = 0; i < 8; i++) {
    if((m >> i) & 1) {
      r |= 0xffUL << (8*i);
    }
  }
  return r;
}

void execAlpha(alpha_state_t *s);
void runAlpha(alpha_state_t *s);

#endif
