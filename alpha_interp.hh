#ifndef __ALPHA_INTERP_HH__
#define __ALPHA_INTERP_HH__

#include <cstdint>
#include <cstring>

/* implementation-private IPR numbers (substrate is chip-private by
 * design - see PALCODE.md decision 4).  reached only via hw_mfpr/
 * hw_mtpr in palmode.  PAL_TEMP scratch is a contiguous block. */
enum {
  IPR_PS        = 0,  /* processor status : mode bit + IPL */
  IPR_EXC_ADDR  = 1,  /* saved PC on PAL entry ; low bit = old palmode */
  IPR_PAL_BASE  = 2,  /* base of the PAL entry-point table */
  IPR_PTBR      = 3,  /* page table base (phase 4) */
  IPR_VPTPTR    = 4,  /* virtual page table pointer (phase 4) */
  IPR_WHAMI     = 6,  /* processor id */
  IPR_SIRR      = 7,  /* software interrupt request (phase 5) */
  IPR_EXC_SUM   = 8,  /* arithmetic/exception summary */
  IPR_PAL_TEMP  = 16, /* IPR_PAL_TEMP .. IPR_PAL_TEMP+31 : scratch */
};

/* EV4-family PAL entry offsets from PAL_BASE (manual figure 2-1) */
enum {
  PAL_RESET        = 0x0000,
  PAL_MCHK         = 0x0020,
  PAL_ARITH        = 0x0060,
  PAL_INTERRUPT    = 0x00E0,
  PAL_DSTREAM_ERR  = 0x01E0,
  PAL_ITB_MISS     = 0x03E0,
  PAL_IACCVIO      = 0x07E0,
  PAL_DTB_MISS_N   = 0x08E0,
  PAL_DTB_MISS_P   = 0x09E0,
  PAL_UNALIGN      = 0x11E0,
  PAL_OPCDEC       = 0x13E0,
  PAL_FEN          = 0x17E0,
  PAL_CALLPAL_PRIV = 0x2000,
  PAL_CALLPAL_UNPRIV = 0x3000,
};

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
  uint64_t brk_addr; /* program break for the brk syscall */
  uint64_t mmap_addr; /* bump allocator for anonymous mmap */
  uint64_t tohost_addr; /* htif magic-mem (call_pal 0xb0 convention) */
  uint64_t fromhost_addr;
  bool cosim_driven; /* checker mode : skip host i/o, arch effects only */
  bool did_rpcc; /* last insn read the cycle counter - checker accepts the RTL value */
  /* co-sim store log hook : null when standalone */
  void (*log_store)(uint64_t pc, uint64_t addr, uint64_t data);

  /* PALmode (phase 1).  pal_loaded gates real PAL dispatch : when a
   * PAL image has been loaded, CALL_PAL and fatal events vector to
   * PAL_BASE + offset instead of the inline htif/host fast path, and
   * the five reserved PAL opcodes (0x19/0x1b/0x1d/0x1e/0x1f) are
   * legal in palmode.  when it is false the ISS behaves exactly as
   * the user-mode interpreter always has. */
  bool pal_loaded;
  bool palmode;
  uint64_t ipr[64]; /* implementation-private IPR file (see IPR_* below) */

  /* system mode : booting a real kernel.  CALL_PAL is serviced in C
   * (PAL-in-C, the scoping path), and load/store/fetch go through
   * xlate() : OSF kseg (va >= 0xfffffc0000000000 -> pa = va - kseg)
   * direct-mapped, low addresses identity (early boot / HWRPB), the
   * general page-table walk via ptbr is a TODO. */
  bool system_mode;
  uint64_t ptbr;   /* page table base (physical), from swpctx */
  uint64_t ksp, usp; /* kernel/user stack pointers (swpctx) */

  uint64_t xlate(uint64_t va) const {
    if(!system_mode) {
      return va;
    }
    if(va >= 0xfffffc0000000000UL) {
      return va - 0xfffffc0000000000UL;   /* kseg direct map */
    }
    return va;   /* identity : early boot + HWRPB ; page walk TODO */
  }

  /* minimal fp state : alpha has no integer divide - libgcc's __divqu
   * and friends do int division THROUGH the fpu, so running any
   * compiled code with division needs this subset even on a "no fp"
   * machine.  f31 reads zero. */
  uint64_t fpr[32];
  uint64_t fpcr;

  /* raw physical accessors (hw_ld/hw_st, HWRPB build, page walk) */
  uint64_t phys_load64(uint64_t pa) const { return *reinterpret_cast<uint64_t*>(mem + pa); }
  uint32_t phys_load32(uint64_t pa) const { return *reinterpret_cast<uint32_t*>(mem + pa); }
  void phys_store64(uint64_t pa, uint64_t x) { *reinterpret_cast<uint64_t*>(mem + pa) = x; }
  void phys_store32(uint64_t pa, uint32_t x) { *reinterpret_cast<uint32_t*>(mem + pa) = x; }

  /* program accessors : translate in system mode, identity otherwise */
  uint64_t load64(uint64_t a) const {
    return *reinterpret_cast<uint64_t*>(mem + xlate(a));
  }
  uint32_t load32(uint64_t a) const {
    return *reinterpret_cast<uint32_t*>(mem + xlate(a));
  }
  uint16_t load16(uint64_t a) const {
    return *reinterpret_cast<uint16_t*>(mem + xlate(a));
  }
  uint8_t load8(uint64_t a) const {
    return mem[xlate(a)];
  }
  void store64(uint64_t a, uint64_t x) {
    *reinterpret_cast<uint64_t*>(mem + xlate(a)) = x;
  }
  void store32(uint64_t a, uint32_t x) {
    *reinterpret_cast<uint32_t*>(mem + xlate(a)) = x;
  }
  void store16(uint64_t a, uint16_t x) {
    *reinterpret_cast<uint16_t*>(mem + xlate(a)) = x;
  }
  void store8(uint64_t a, uint8_t x) {
    mem[xlate(a)] = x;
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
  struct fp_t { /* fp operate format : 11-bit function */
    uint32_t rc : 5;
    uint32_t func : 11;
    uint32_t rb : 5;
    uint32_t ra : 5;
    uint32_t opcode : 6;
  } f;
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
