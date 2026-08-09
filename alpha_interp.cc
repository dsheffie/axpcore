/* user-mode alpha interpreter : EV4 integer subset + BWX (+ CIX
 * bit-counts, they're free).  no floating point, no PALmode - CALL_PAL
 * 0x83 (callsys) proxies linux/alpha syscalls so the same binary runs
 * under qemu-alpha for diff-testing. */
#include "alpha_interp.hh"
#include <cstdio>
#include <cstdlib>
#include <cfenv>
#include <cmath>
#include <unistd.h>

static const bool g_trace = getenv("AXP_TRACE") != nullptr;

/* linux/alpha inherits OSF/1 syscall numbering.  return convention :
 * v0 = result (or positive errno), a3 = error flag.  enough of the set
 * to run static glibc binaries - the list came from
 * `qemu-alpha -strace` on a static hello world. */
static void handle_callsys(alpha_state_t *s) {
  int64_t num = s->gpr[0];
  int64_t a0 = s->gpr[16], a1 = s->gpr[17], a2 = s->gpr[18], a3 = s->gpr[19];
  s->gpr[19] = 0;
  switch(num)
    {
    case 1: /* exit */
    case 405: /* exit_group */
      s->brk = 1;
      s->exit_code = static_cast<int>(a0);
      break;
    case 3: /* read */
      s->gpr[0] = read(a0, s->mem + a1, a2);
      break;
    case 4: /* write */
      s->gpr[0] = write(a0, s->mem + a1, a2);
      break;
    case 17: /* brk : linux returns the new break */
      if(a0 != 0) {
	s->brk_addr = a0;
      }
      s->gpr[0] = s->brk_addr;
      break;
    case 54: /* ioctl : nothing is a tty */
      s->gpr[0] = 25; /* ENOTTY */
      s->gpr[19] = 1;
      break;
    case 71: /* mmap : anonymous only, bump allocator */
      if((a0 == 0) && ((s->gpr[20] & 0x10 /* MAP_ANONYMOUS on alpha */) != 0)) {
	uint64_t len = (a1 + 8191UL) & ~8191UL;
	s->gpr[0] = s->mmap_addr;
	s->mmap_addr += len;
      }
      else {
	fprintf(stderr, "alpha_interp: unhandled mmap(%lx, %lx, flags %lx) at pc %lx\n",
		a0, a1, s->gpr[20], s->pc);
	exit(-1);
      }
      break;
    case 73: /* munmap */
    case 74: /* mprotect */
    case 352: /* rt_sigaction */
    case 353: /* rt_sigprocmask */
      s->gpr[0] = 0;
      break;
    case 121: { /* writev : glibc stdio flush path */
      int64_t r = 0;
      for(int64_t i = 0; i < a2; i++) {
	uint64_t base = s->load64(a1 + 16*i);
	uint64_t len = s->load64(a1 + 16*i + 8);
	r += write(a0, s->mem + base, len);
      }
      s->gpr[0] = r;
      break;
    }
    case 256: /* osf_getsysinfo : fp control, none here */
      s->gpr[0] = 22; /* EINVAL */
      s->gpr[19] = 1;
      break;
    case 339: { /* uname : 6 fields x 65 bytes */
      static const char *f[6] = {"Linux", "axpcore", "5.4.0", "#1", "alpha", ""};
      for(int i = 0; i < 6; i++) {
	uint64_t p = a0 + 65*i;
	size_t n = strlen(f[i]);
	memcpy(s->mem + p, f[i], n + 1);
      }
      s->gpr[0] = 0;
      break;
    }
    case 378: /* gettid */
    case 411: /* set_tid_address */
      s->gpr[0] = 42;
      break;
    case 427: /* fstat64 : deterministic character device for stdio */
      memset(s->mem + a1, 0, 136);
      s->store32(a1 + 40, 0020000 | 0620); /* st_mode */
      s->store32(a1 + 52, 8192); /* st_blksize */
      s->gpr[0] = 0;
      break;
    case 460: { /* readlinkat : only /proc/self/exe shows up */
      static const char path[] = "/alpha_bin";
      size_t n = sizeof(path) - 1;
      memcpy(s->mem + a2, path, n);
      s->gpr[0] = n;
      break;
    }
    case 466: /* set_robust_list : qemu returns ENOSYS too */
      s->gpr[0] = 78; /* ENOSYS */
      s->gpr[19] = 1;
      break;
    case 496: /* prlimit64(0, resource, NULL, old) */
      if(a3 != 0) {
	s->store64(a3, 8UL << 20); /* rlim_cur : 8MB stack */
	s->store64(a3 + 8, ~0UL);
      }
      s->gpr[0] = 0;
      break;
    case 511: /* getrandom : deterministic - only feeds the stack canary */
      for(int64_t i = 0; i < a1; i++) {
	s->store8(a0 + i, 0xa5 ^ i);
      }
      s->gpr[0] = a1;
      break;
    default:
      fprintf(stderr, "alpha_interp: unimplemented callsys %ld at pc %lx\n",
	      num, s->pc);
      exit(-1);
    }
}

static inline double as_double(uint64_t x) {
  double d;
  memcpy(&d, &x, 8);
  return d;
}

static inline uint64_t as_u64(double d) {
  uint64_t x;
  memcpy(&x, &d, 8);
  return x;
}

/* S-format memory single -> register T-format (the MAP_S widening) */
static inline uint64_t map_s(uint32_t x) {
  uint64_t sign = (x >> 31) & 1;
  uint64_t exp = (x >> 23) & 0xff;
  uint64_t frac = x & 0x7fffff;
  uint64_t e11;
  if(exp == 0xff) {
    e11 = 0x7ff;
  }
  else if(exp == 0) {
    e11 = 0;
  }
  else {
    e11 = exp + (1023 - 127);
  }
  return (sign << 63) | (e11 << 52) | (frac << 29);
}

/* alpha fp rounding qualifier (func bits 7:6) -> host rounding mode */
static inline int alpha_rnd(uint32_t rnd, uint64_t fpcr) {
  switch(rnd)
    {
    case 0: return FE_TOWARDZERO; /* /c chopped */
    case 1: return FE_DOWNWARD; /* /m minus */
    case 2: return FE_TONEAREST; /* normal */
    default: /* /d dynamic : FPCR<59:58> */
      switch((fpcr >> 58) & 3)
	{
	case 0: return FE_TOWARDZERO;
	case 1: return FE_DOWNWARD;
	case 2: return FE_TONEAREST;
	default: return FE_UPWARD;
	}
    }
}

void execAlpha(alpha_state_t *s) {
  alpha_t m;
  uint64_t pc = s->pc;
  uint64_t npc = pc + 4;
  m.raw = s->load32(pc);
  s->last_pc = pc;
  uint32_t opcode = m.raw >> 26;

  if(g_trace) {
    fprintf(stderr, "%lx : %08x\n", pc, m.raw);
  }

  switch(opcode)
    {
    case 0x00: /* CALL_PAL */
      switch(m.p.func)
	{
	case 0x00: /* halt */
	  s->brk = 1;
	  break;
	case 0x83: /* callsys */
	  handle_callsys(s);
	  break;
	case 0x86: /* imb */
	  break;
	case 0x9e: /* rdunique */
	  s->gpr[0] = s->unique;
	  break;
	case 0x9f: /* wrunique */
	  s->unique = s->gpr[16];
	  break;
	default:
	  fprintf(stderr, "alpha_interp: unimplemented call_pal %x at pc %lx\n",
		  m.p.func, pc);
	  exit(-1);
	}
      break;

    case 0x08: /* lda */
      s->gpr[m.m.ra] = s->gpr[m.m.rb] + sext16(m.m.disp);
      break;
    case 0x09: /* ldah */
      s->gpr[m.m.ra] = s->gpr[m.m.rb] + (sext16(m.m.disp) << 16);
      break;
    case 0x0a: /* ldbu (BWX) */
      s->gpr[m.m.ra] = s->load8(s->gpr[m.m.rb] + sext16(m.m.disp));
      break;
    case 0x0b: /* ldq_u */
      s->gpr[m.m.ra] = s->load64((s->gpr[m.m.rb] + sext16(m.m.disp)) & ~7UL);
      break;
    case 0x0c: /* ldwu (BWX) */
      s->gpr[m.m.ra] = s->load16(s->gpr[m.m.rb] + sext16(m.m.disp));
      break;
    case 0x0d: /* stw (BWX) */
      s->store16(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x0e: /* stb (BWX) */
      s->store8(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x0f: /* stq_u */
      s->store64((s->gpr[m.m.rb] + sext16(m.m.disp)) & ~7UL, s->gpr[m.m.ra]);
      break;

    case 0x22: /* lds : S memory format widens to register T format */
      s->fpr[m.m.ra] = map_s(s->load32(s->gpr[m.m.rb] + sext16(m.m.disp)));
      s->fpr[31] = 0;
      break;
    case 0x23: /* ldt */
      s->fpr[m.m.ra] = s->load64(s->gpr[m.m.rb] + sext16(m.m.disp));
      s->fpr[31] = 0;
      break;
    case 0x26: { /* sts */
      uint64_t v = s->fpr[m.m.ra];
      s->store32(s->gpr[m.m.rb] + sext16(m.m.disp),
		 ((v >> 63) << 31) | ((v >> 29) & 0x7fffffffUL));
      break;
    }
    case 0x27: /* stt */
      s->store64(s->gpr[m.m.rb] + sext16(m.m.disp), s->fpr[m.m.ra]);
      break;

    case 0x10: { /* INTA */
      int64_t rav = s->gpr[m.o.ra];
      int64_t rbv = m.o.is_lit ? static_cast<int64_t>(m.ol.lit) : s->gpr[m.o.rb];
      int64_t rc = 0;
      switch(m.o.func)
	{
	case 0x00: /* addl */
	case 0x40: /* addl/v : overflow traps not modeled */
	  rc = sext32(rav + rbv);
	  break;
	case 0x02: /* s4addl */
	  rc = sext32((rav << 2) + rbv);
	  break;
	case 0x09: /* subl */
	case 0x49: /* subl/v */
	  rc = sext32(rav - rbv);
	  break;
	case 0x0b: /* s4subl */
	  rc = sext32((rav << 2) - rbv);
	  break;
	case 0x0f: { /* cmpbge */
	  for(int i = 0; i < 8; i++) {
	    uint8_t ba = rav >> (8*i), bb = rbv >> (8*i);
	    if(ba >= bb) {
	      rc |= (1L << i);
	    }
	  }
	  break;
	}
	case 0x12: /* s8addl */
	  rc = sext32((rav << 3) + rbv);
	  break;
	case 0x1b: /* s8subl */
	  rc = sext32((rav << 3) - rbv);
	  break;
	case 0x1d: /* cmpult */
	  rc = (static_cast<uint64_t>(rav) < static_cast<uint64_t>(rbv)) ? 1 : 0;
	  break;
	case 0x20: /* addq */
	case 0x60: /* addq/v */
	  rc = rav + rbv;
	  break;
	case 0x22: /* s4addq */
	  rc = (rav << 2) + rbv;
	  break;
	case 0x29: /* subq */
	case 0x69: /* subq/v */
	  rc = rav - rbv;
	  break;
	case 0x2b: /* s4subq */
	  rc = (rav << 2) - rbv;
	  break;
	case 0x2d: /* cmpeq */
	  rc = (rav == rbv) ? 1 : 0;
	  break;
	case 0x32: /* s8addq */
	  rc = (rav << 3) + rbv;
	  break;
	case 0x3b: /* s8subq */
	  rc = (rav << 3) - rbv;
	  break;
	case 0x3d: /* cmpule */
	  rc = (static_cast<uint64_t>(rav) <= static_cast<uint64_t>(rbv)) ? 1 : 0;
	  break;
	case 0x4d: /* cmplt */
	  rc = (rav < rbv) ? 1 : 0;
	  break;
	case 0x6d: /* cmple */
	  rc = (rav <= rbv) ? 1 : 0;
	  break;
	default:
	  goto report_unimplemented;
	}
      s->gpr[m.o.rc] = rc;
      break;
    }

    case 0x11: { /* INTL : logic + cmov */
      int64_t rav = s->gpr[m.o.ra];
      int64_t rbv = m.o.is_lit ? static_cast<int64_t>(m.ol.lit) : s->gpr[m.o.rb];
      switch(m.o.func)
	{
	case 0x00: /* and */
	  s->gpr[m.o.rc] = rav & rbv;
	  break;
	case 0x08: /* bic */
	  s->gpr[m.o.rc] = rav & ~rbv;
	  break;
	case 0x14: /* cmovlbs */
	  if(rav & 1) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x16: /* cmovlbc */
	  if((rav & 1) == 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x20: /* bis */
	  s->gpr[m.o.rc] = rav | rbv;
	  break;
	case 0x24: /* cmoveq */
	  if(rav == 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x26: /* cmovne */
	  if(rav != 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x28: /* ornot */
	  s->gpr[m.o.rc] = rav | ~rbv;
	  break;
	case 0x40: /* xor */
	  s->gpr[m.o.rc] = rav ^ rbv;
	  break;
	case 0x44: /* cmovlt */
	  if(rav < 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x46: /* cmovge */
	  if(rav >= 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x48: /* eqv */
	  s->gpr[m.o.rc] = rav ^ ~rbv;
	  break;
	case 0x61: /* amask : bit0 (BWX) implemented */
	  s->gpr[m.o.rc] = rbv & ~1L;
	  break;
	case 0x64: /* cmovle */
	  if(rav <= 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x66: /* cmovgt */
	  if(rav > 0) {
	    s->gpr[m.o.rc] = rbv;
	  }
	  break;
	case 0x6c: /* implver : 1 = EV5 family (BWX-capable) */
	  s->gpr[m.o.rc] = 1;
	  break;
	default:
	  goto report_unimplemented;
	}
      break;
    }

    case 0x12: { /* INTS : shifts + byte zapper */
      uint64_t rav = s->gpr[m.o.ra];
      uint64_t rbv = m.o.is_lit ? static_cast<uint64_t>(m.ol.lit) : static_cast<uint64_t>(s->gpr[m.o.rb]);
      uint32_t bn = rbv & 7;
      uint64_t rc = 0;
      switch(m.o.func)
	{
	case 0x02: /* mskbl */
	  rc = rav & ~byte_mask((0x01 << bn) & 0xff);
	  break;
	case 0x06: /* extbl */
	  rc = (rav >> (8*bn)) & byte_mask(0x01);
	  break;
	case 0x0b: /* insbl */
	  rc = (rav << (8*bn)) & byte_mask((0x01 << bn) & 0xff);
	  break;
	case 0x12: /* mskwl */
	  rc = rav & ~byte_mask((0x03 << bn) & 0xff);
	  break;
	case 0x16: /* extwl */
	  rc = (rav >> (8*bn)) & byte_mask(0x03);
	  break;
	case 0x1b: /* inswl */
	  rc = (rav << (8*bn)) & byte_mask((0x03 << bn) & 0xff);
	  break;
	case 0x22: /* mskll */
	  rc = rav & ~byte_mask((0x0f << bn) & 0xff);
	  break;
	case 0x26: /* extll */
	  rc = (rav >> (8*bn)) & byte_mask(0x0f);
	  break;
	case 0x2b: /* insll */
	  rc = (rav << (8*bn)) & byte_mask((0x0f << bn) & 0xff);
	  break;
	case 0x30: /* zap */
	  rc = rav & ~byte_mask(rbv & 0xff);
	  break;
	case 0x31: /* zapnot */
	  rc = rav & byte_mask(rbv & 0xff);
	  break;
	case 0x32: /* mskql */
	  rc = rav & ~byte_mask((0xff << bn) & 0xff);
	  break;
	case 0x34: /* srl */
	  rc = rav >> (rbv & 63);
	  break;
	case 0x36: /* extql */
	  rc = rav >> (8*bn);
	  break;
	case 0x39: /* sll */
	  rc = rav << (rbv & 63);
	  break;
	case 0x3b: /* insql */
	  rc = (rav << (8*bn)) & byte_mask((0xff << bn) & 0xff);
	  break;
	case 0x3c: /* sra */
	  rc = static_cast<int64_t>(rav) >> (rbv & 63);
	  break;
	case 0x52: /* mskwh */
	  rc = rav & ~byte_mask((0x03 << bn) >> 8);
	  break;
	case 0x57: /* inswh */
	  rc = (rav >> ((64 - 8*bn) & 63)) & byte_mask((0x03 << bn) >> 8);
	  break;
	case 0x5a: /* extwh */
	  rc = (rav << ((64 - 8*bn) & 63)) & byte_mask(0x03);
	  break;
	case 0x62: /* msklh */
	  rc = rav & ~byte_mask((0x0f << bn) >> 8);
	  break;
	case 0x67: /* inslh */
	  rc = (rav >> ((64 - 8*bn) & 63)) & byte_mask((0x0f << bn) >> 8);
	  break;
	case 0x6a: /* extlh */
	  rc = (rav << ((64 - 8*bn) & 63)) & byte_mask(0x0f);
	  break;
	case 0x72: /* mskqh */
	  rc = rav & ~byte_mask((0xff << bn) >> 8);
	  break;
	case 0x77: /* insqh */
	  rc = (rav >> ((64 - 8*bn) & 63)) & byte_mask((0xff << bn) >> 8);
	  break;
	case 0x7a: /* extqh */
	  rc = (rav << ((64 - 8*bn) & 63)) & byte_mask(0xff);
	  break;
	default:
	  goto report_unimplemented;
	}
      s->gpr[m.o.rc] = rc;
      break;
    }

    case 0x13: { /* INTM */
      int64_t rav = s->gpr[m.o.ra];
      int64_t rbv = m.o.is_lit ? static_cast<int64_t>(m.ol.lit) : s->gpr[m.o.rb];
      switch(m.o.func)
	{
	case 0x00: /* mull */
	case 0x40: /* mull/v */
	  s->gpr[m.o.rc] = sext32(rav * rbv);
	  break;
	case 0x20: /* mulq */
	case 0x60: /* mulq/v */
	  s->gpr[m.o.rc] = rav * rbv;
	  break;
	case 0x30: /* umulh */
	  s->gpr[m.o.rc] = static_cast<uint64_t>((static_cast<unsigned __int128>(static_cast<uint64_t>(rav)) *
						  static_cast<unsigned __int128>(static_cast<uint64_t>(rbv))) >> 64);
	  break;
	default:
	  goto report_unimplemented;
	}
      break;
    }

    case 0x16: { /* FLTI : ieee arithmetic.  trap qualifiers (func<10:8>)
		  * are ignored, rounding qualifier honored via fenv */
      double fa = as_double(s->fpr[m.f.ra]);
      double fb = as_double(s->fpr[m.f.rb]);
      int64_t vb = static_cast<int64_t>(s->fpr[m.f.rb]);
      uint32_t fn = m.f.func & 0x3f;
      uint64_t rc = 0;
      fesetround(alpha_rnd((m.f.func >> 6) & 3, s->fpcr));
      switch(fn)
	{
	case 0x00: /* adds */
	  rc = as_u64(static_cast<double>(static_cast<float>(fa) + static_cast<float>(fb)));
	  break;
	case 0x01: /* subs */
	  rc = as_u64(static_cast<double>(static_cast<float>(fa) - static_cast<float>(fb)));
	  break;
	case 0x02: /* muls */
	  rc = as_u64(static_cast<double>(static_cast<float>(fa) * static_cast<float>(fb)));
	  break;
	case 0x03: /* divs */
	  rc = as_u64(static_cast<double>(static_cast<float>(fa) / static_cast<float>(fb)));
	  break;
	case 0x20: /* addt */
	  rc = as_u64(fa + fb);
	  break;
	case 0x21: /* subt */
	  rc = as_u64(fa - fb);
	  break;
	case 0x22: /* mult */
	  rc = as_u64(fa * fb);
	  break;
	case 0x23: /* divt */
	  rc = as_u64(fa / fb);
	  break;
	case 0x24: /* cmptun */
	  rc = (std::isnan(fa) || std::isnan(fb)) ? 0x4000000000000000UL : 0;
	  break;
	case 0x25: /* cmpteq */
	  rc = (fa == fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x26: /* cmptlt */
	  rc = (fa < fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x27: /* cmptle */
	  rc = (fa <= fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x2c: /* cvtts */
	  rc = as_u64(static_cast<double>(static_cast<float>(fb)));
	  break;
	case 0x2f: /* cvttq : rounding per qualifier (llrint follows fenv) */
	  rc = static_cast<uint64_t>(llrint(fb));
	  break;
	case 0x3c: /* cvtqs */
	  rc = as_u64(static_cast<double>(static_cast<float>(vb)));
	  break;
	case 0x3e: /* cvtqt */
	  rc = as_u64(static_cast<double>(vb));
	  break;
	default:
	  fesetround(FE_TONEAREST);
	  goto report_unimplemented;
	}
      fesetround(FE_TONEAREST);
      s->fpr[m.f.rc] = rc;
      s->fpr[31] = 0;
      break;
    }

    case 0x17: { /* FLTL : copies, fp cmov, fpcr, longword converts */
      uint64_t va = s->fpr[m.f.ra];
      uint64_t vb = s->fpr[m.f.rb];
      double db = as_double(s->fpr[m.f.rb]);
      double da = as_double(va);
      switch(m.f.func)
	{
	case 0x010: /* cvtlq */
	  s->fpr[m.f.rc] = static_cast<uint64_t>(sext32(((vb >> 32) & 0xc0000000UL) |
							((vb >> 29) & 0x3fffffffUL)));
	  break;
	case 0x020: /* cpys */
	  s->fpr[m.f.rc] = (va & (1UL << 63)) | (vb & ~(1UL << 63));
	  break;
	case 0x021: /* cpysn */
	  s->fpr[m.f.rc] = ((va & (1UL << 63)) ^ (1UL << 63)) | (vb & ~(1UL << 63));
	  break;
	case 0x022: /* cpyse */
	  s->fpr[m.f.rc] = (va & 0xfff0000000000000UL) | (vb & 0x000fffffffffffffUL);
	  break;
	case 0x024: /* mt_fpcr */
	  s->fpcr = va;
	  break;
	case 0x025: /* mf_fpcr */
	  s->fpr[m.f.ra] = s->fpcr;
	  break;
	case 0x02a: /* fcmoveq */
	  if(da == 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02b: /* fcmovne */
	  if(da != 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02c: /* fcmovlt */
	  if(da < 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02d: /* fcmovge */
	  if(da >= 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02e: /* fcmovle */
	  if(da <= 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02f: /* fcmovgt */
	  if(da > 0.0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x030: case 0x130: case 0x530: /* cvtql (+ /v /sv) */
	  s->fpr[m.f.rc] = ((vb & 0xc0000000UL) << 32) | ((vb & 0x3fffffffUL) << 29);
	  break;
	default:
	  goto report_unimplemented;
	}
      s->fpr[31] = 0;
      break;
    }

    case 0x18: /* MISC */
      switch(m.m.disp)
	{
	case 0x0000: /* trapb */
	case 0x0400: /* excb */
	case 0x4000: /* mb */
	case 0x4400: /* wmb */
	case 0x8000: /* fetch */
	case 0xa000: /* fetch_m */
	  break;
	case 0xc000: /* rpcc */
	  s->gpr[m.m.ra] = s->icnt;
	  break;
	case 0xe000: /* rc */
	case 0xf000: /* rs */
	  s->gpr[m.m.ra] = 0;
	  break;
	default:
	  goto report_unimplemented;
	}
      break;

    case 0x1a: { /* jmp/jsr/ret/jsr_coroutine - hint bits don't change semantics */
      uint64_t target = s->gpr[m.m.rb] & ~3UL;
      s->gpr[m.m.ra] = npc;
      npc = target;
      break;
    }

    case 0x1c: { /* SEXTB/SEXTW (BWX) + CIX bit counts */
      int64_t rbv = m.o.is_lit ? static_cast<int64_t>(m.ol.lit) : s->gpr[m.o.rb];
      switch(m.o.func)
	{
	case 0x00: /* sextb */
	  s->gpr[m.o.rc] = sext8(rbv);
	  break;
	case 0x01: /* sextw */
	  s->gpr[m.o.rc] = sext16(rbv);
	  break;
	case 0x30: /* ctpop */
	  s->gpr[m.o.rc] = __builtin_popcountll(rbv);
	  break;
	case 0x32: /* ctlz */
	  s->gpr[m.o.rc] = (rbv == 0) ? 64 : __builtin_clzll(rbv);
	  break;
	case 0x33: /* cttz */
	  s->gpr[m.o.rc] = (rbv == 0) ? 64 : __builtin_ctzll(rbv);
	  break;
	default:
	  goto report_unimplemented;
	}
      break;
    }

    case 0x28: /* ldl */
      s->gpr[m.m.ra] = sext32(s->load32(s->gpr[m.m.rb] + sext16(m.m.disp)));
      break;
    case 0x29: /* ldq */
      s->gpr[m.m.ra] = s->load64(s->gpr[m.m.rb] + sext16(m.m.disp));
      break;
    case 0x2a: { /* ldl_l */
      uint64_t ea = s->gpr[m.m.rb] + sext16(m.m.disp);
      s->gpr[m.m.ra] = sext32(s->load32(ea));
      s->lock_valid = true;
      s->lock_addr = ea & ~63UL;
      break;
    }
    case 0x2b: { /* ldq_l */
      uint64_t ea = s->gpr[m.m.rb] + sext16(m.m.disp);
      s->gpr[m.m.ra] = s->load64(ea);
      s->lock_valid = true;
      s->lock_addr = ea & ~63UL;
      break;
    }
    case 0x2c: /* stl */
      s->store32(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x2d: /* stq */
      s->store64(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x2e: /* stl_c : single-threaded, succeeds iff lock still held */
      if(s->lock_valid) {
	s->store32(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
	s->gpr[m.m.ra] = 1;
      }
      else {
	s->gpr[m.m.ra] = 0;
      }
      s->lock_valid = false;
      break;
    case 0x2f: /* stq_c */
      if(s->lock_valid) {
	s->store64(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
	s->gpr[m.m.ra] = 1;
      }
      else {
	s->gpr[m.m.ra] = 0;
      }
      s->lock_valid = false;
      break;

    case 0x30: /* br */
      s->gpr[m.b.ra] = npc;
      npc = npc + (sext21(m.b.disp) << 2);
      break;
    case 0x31: /* fbeq */
      if(as_double(s->fpr[m.b.ra]) == 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x32: /* fblt */
      if(as_double(s->fpr[m.b.ra]) < 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x33: /* fble */
      if(as_double(s->fpr[m.b.ra]) <= 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x34: /* bsr */
      s->gpr[m.b.ra] = npc;
      npc = npc + (sext21(m.b.disp) << 2);
      break;
    case 0x35: /* fbne */
      if(as_double(s->fpr[m.b.ra]) != 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x36: /* fbge */
      if(as_double(s->fpr[m.b.ra]) >= 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x37: /* fbgt */
      if(as_double(s->fpr[m.b.ra]) > 0.0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x38: /* blbc */
      if((s->gpr[m.b.ra] & 1) == 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x39: /* beq */
      if(s->gpr[m.b.ra] == 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3a: /* blt */
      if(s->gpr[m.b.ra] < 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3b: /* ble */
      if(s->gpr[m.b.ra] <= 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3c: /* blbs */
      if(s->gpr[m.b.ra] & 1) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3d: /* bne */
      if(s->gpr[m.b.ra] != 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3e: /* bge */
      if(s->gpr[m.b.ra] >= 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x3f: /* bgt */
      if(s->gpr[m.b.ra] > 0) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;

    default:
    report_unimplemented:
      fprintf(stderr, "alpha_interp: unimplemented insn %08x (opcode %02x, func %02x) at pc %lx, icnt %lu\n",
	      m.raw, opcode, m.o.func, pc, s->icnt);
      exit(-1);
    }

  s->gpr[31] = 0;
  s->icnt++;
  s->pc = npc;
}

void runAlpha(alpha_state_t *s) {
  while(s->brk == 0 && s->icnt < s->maxicnt) {
    execAlpha(s);
  }
}
