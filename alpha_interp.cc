/* user-mode alpha interpreter : EV4 integer subset + BWX (+ CIX
 * bit-counts, they're free).  no floating point, no PALmode - CALL_PAL
 * 0x83 (callsys) proxies linux/alpha syscalls so the same binary runs
 * under qemu-alpha for diff-testing. */
#include "alpha_interp.hh"
#include "alpha_fpu.hh"
#include <cstdio>
#include <cstdlib>
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

/* the FP zero-value bit test used by FCMOVxx and the FP branches :
 * +0.0 and -0.0 both count as zero (only sign differs). */
static inline bool fp_is_zero(uint64_t b) {
  return (b & ~(1UL << 63)) == 0;
}

/* call_pal 0xb0 : the htif escape used by RTL co-sim binaries.
 * magic-mem convention matching the rv64 harness (syscall.cc) : odd
 * tohost = exit, else tohost points at buf[8] with buf[0] = syscall
 * number.  arch effects (result in buf[0], tohost cleared, fromhost
 * set) are computed deterministically so the checker stays in sync;
 * host i/o is skipped when cosim_driven (the harness does it). */
static void handle_htif_monitor(alpha_state_t *s) {
  if(s->tohost_addr == 0) {
    fprintf(stderr, "alpha_interp: call_pal 0xb0 with no tohost symbol\n");
    exit(-1);
  }
  uint64_t th = s->load64(s->tohost_addr);
  if(th == 0) {
    return;
  }
  if(th & 1) {
    s->brk = 1;
    s->exit_code = static_cast<int>(th >> 1);
    return;
  }
  uint64_t buf = th & ((1UL << 32) - 1);
  int64_t n = s->load64(buf);
  switch(n)
    {
    case 64: { /* SYS_write */
      uint64_t fd = s->load64(buf + 8);
      uint64_t ptr = s->load64(buf + 16);
      uint64_t len = s->load64(buf + 24);
      if(!s->cosim_driven) {
	ssize_t rc = write(fd, s->mem + ptr, len);
	(void)rc;
      }
      s->store64(buf, len);
      break;
    }
    default:
      fprintf(stderr, "alpha_interp: unimplemented htif syscall %ld at pc %lx\n",
	      n, s->pc);
      exit(-1);
    }
  s->store64(s->tohost_addr, 0);
  s->store64(s->fromhost_addr, 1);
}

/* enter PALmode at PAL_BASE + off : save return PC (low bit carries
 * the mode being left, per the architected HW_REI convention), switch
 * to palmode, redirect fetch.  returns the new pc. */
static uint64_t enter_pal(alpha_state_t *s, uint64_t pc, uint64_t off) {
  s->ipr[IPR_EXC_ADDR] = (pc & ~3UL) | (s->palmode ? 1 : 0);
  s->palmode = true;
  return s->ipr[IPR_PAL_BASE] + off;
}

void execAlpha(alpha_state_t *s) {
  alpha_t m;
  uint64_t pc = s->pc;
  uint64_t npc = pc + 4;
  m.raw = s->load32(pc);
  s->last_pc = pc;
  s->did_rpcc = false;
  uint32_t opcode = m.raw >> 26;

  if(g_trace) {
    fprintf(stderr, "%lx : %08x\n", pc, m.raw);
  }

  switch(opcode)
    {
    case 0x00: /* CALL_PAL */
      /* 0xb0 is the substrate's own console / co-sim transport (the
       * manual's putc console-service role), not a guest-visible PAL
       * call - it stays inline even under real PAL dispatch. */
      if(s->pal_loaded && (m.p.func != 0xb0)) {
	/* real dispatch : func<7> selects priv/unpriv region, func<5:0>
	 * indexes 64-byte slots.  privileged call from non-pal user
	 * mode would OPCDEC on hardware; PS mode checks land in phase 2. */
	uint32_t func = m.p.func;
	uint64_t region = (func & 0x80) ? PAL_CALLPAL_UNPRIV : PAL_CALLPAL_PRIV;
	npc = enter_pal(s, npc, region + ((func & 0x3f) << 6));
	break;
      }
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
	case 0xb0: /* htif escape (co-sim convention) */
	  handle_htif_monitor(s);
	  break;
	default:
	  fprintf(stderr, "alpha_interp: unimplemented call_pal %x at pc %lx\n",
		  m.p.func, pc);
	  exit(-1);
	}
      break;

    case 0x19: /* hw_mfpr (pal19) : Ra <- IPR[disp<7:0>] */
      if(!s->palmode) {
	goto opcdec;
      }
      s->gpr[m.m.ra] = s->ipr[m.m.disp & 0x3f];
      break;
    case 0x1d: /* hw_mtpr (pal1d) : IPR[disp<7:0>] <- Ra */
      if(!s->palmode) {
	goto opcdec;
      }
      s->ipr[m.m.disp & 0x3f] = s->gpr[m.m.ra];
      break;
    case 0x1b: { /* hw_ld (pal1b) : Ra <- phys[Rb + sext12(disp)]
		  * 21064 format : Q bit [12] = quadword/longword, disp
		  * [11:0], address naturally aligned (mask low 2/3 bits).
		  * physical only in axpcore (no translation yet). */
      if(!s->palmode) {
	goto opcdec;
      }
      bool qw = (m.raw >> 12) & 1;
      int64_t disp = (static_cast<int64_t>(m.raw & 0xfff) << 52) >> 52;
      uint64_t ea = (s->gpr[m.m.rb] + disp) & ~(qw ? 7UL : 3UL);
      s->gpr[m.m.ra] = qw ? s->load64(ea) : sext32(s->load32(ea));
      break;
    }
    case 0x1f: { /* hw_st (pal1f) : phys[Rb + sext12(disp)] <- Ra */
      if(!s->palmode) {
	goto opcdec;
      }
      bool qw = (m.raw >> 12) & 1;
      int64_t disp = (static_cast<int64_t>(m.raw & 0xfff) << 52) >> 52;
      uint64_t ea = (s->gpr[m.m.rb] + disp) & ~(qw ? 7UL : 3UL);
      if(qw) {
	s->store64(ea, s->gpr[m.m.ra]);
      }
      else {
	s->store32(ea, s->gpr[m.m.ra]);
      }
      break;
    }
    case 0x1e: /* hw_rei (pal1e) : return from PAL */
      if(!s->palmode) {
	goto opcdec;
      }
      npc = s->ipr[IPR_EXC_ADDR] & ~3UL;
      s->palmode = (s->ipr[IPR_EXC_ADDR] & 1) != 0;
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
      if(s->log_store) {
	s->log_store(pc, s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      }
      s->store16(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x0e: /* stb (BWX) */
      if(s->log_store) {
	s->log_store(pc, s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      }
      s->store8(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x0f: /* stq_u */
      if(s->log_store) {
	s->log_store(pc, (s->gpr[m.m.rb] + sext16(m.m.disp)) & ~7UL, s->gpr[m.m.ra]);
      }
      s->store64((s->gpr[m.m.rb] + sext16(m.m.disp)) & ~7UL, s->gpr[m.m.ra]);
      break;

    case 0x22: /* lds : S memory single -> register format (expand) */
      s->fpr[m.m.ra] = sf_s_to_reg(sf_f32(s->load32(s->gpr[m.m.rb] + sext16(m.m.disp))));
      s->fpr[31] = 0;
      break;
    case 0x23: /* ldt */
      s->fpr[m.m.ra] = s->load64(s->gpr[m.m.rb] + sext16(m.m.disp));
      s->fpr[31] = 0;
      break;
    case 0x26: /* sts : register format -> S memory single (narrow) */
      s->store32(s->gpr[m.m.rb] + sext16(m.m.disp), sf_reg_to_s(s->fpr[m.m.ra]).v);
      break;
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

    case 0x14: { /* ITFP : integer->fp moves (FIX) + sqrt */
      softfloat_exceptionFlags = 0;
      softfloat_roundingMode = alpha_sf_round(m.f.func, s->fpcr);
      uint32_t fn = m.f.func & 0x3f;
      uint64_t rc = 0;
      bool ok = true;
      switch(fn)
	{
	case 0x04: /* itofs : int reg<31:0> as S memory single -> fp reg */
	  rc = sf_s_to_reg(sf_f32(static_cast<uint32_t>(s->gpr[m.f.ra])));
	  break;
	case 0x24: /* itoft : int reg -> fp reg (raw copy) */
	  rc = s->gpr[m.f.ra];
	  break;
	case 0x0b: /* sqrts */
	  rc = sf_s_to_reg(f32_sqrt(sf_reg_to_s(s->fpr[m.f.rb])));
	  break;
	case 0x2b: /* sqrtt */
	  rc = sf_bits64(f64_sqrt(sf_f64(s->fpr[m.f.rb])));
	  break;
	default:
	  ok = false;
	  break;
	}
      if(!ok) {
	goto report_unimplemented;
      }
      s->fpr[m.f.rc] = rc;
      s->fpr[31] = 0;
      break;
    }

    case 0x15: /* FLTV : VAX F/G/D floating - not yet implemented.
		* softfloat has no VAX formats; the path is convert
		* VAX<->IEEE (rebias, PDP-11 word swap, ties-away
		* rounding, reserved-operand traps) around softfloat.
		* deferred - Linux userland is IEEE (0x16). */
      goto report_unimplemented;

    case 0x16: { /* FLTI : IEEE S/T arithmetic, compares, converts */
      softfloat_exceptionFlags = 0;
      softfloat_roundingMode = alpha_sf_round(m.f.func, s->fpcr);
      float64_t fa = sf_f64(s->fpr[m.f.ra]);
      float64_t fb = sf_f64(s->fpr[m.f.rb]);
      float32_t sa = sf_reg_to_s(s->fpr[m.f.ra]);
      float32_t sb = sf_reg_to_s(s->fpr[m.f.rb]);
      int64_t vb = static_cast<int64_t>(s->fpr[m.f.rb]);
      uint32_t fn = m.f.func & 0x3f;
      uint64_t rc = 0;
      bool ok = true;
      switch(fn)
	{
	case 0x00: /* adds */
	  rc = sf_s_to_reg(f32_add(sa, sb));
	  break;
	case 0x01: /* subs */
	  rc = sf_s_to_reg(f32_sub(sa, sb));
	  break;
	case 0x02: /* muls */
	  rc = sf_s_to_reg(f32_mul(sa, sb));
	  break;
	case 0x03: /* divs */
	  rc = sf_s_to_reg(f32_div(sa, sb));
	  break;
	case 0x20: /* addt */
	  rc = sf_bits64(f64_add(fa, fb));
	  break;
	case 0x21: /* subt */
	  rc = sf_bits64(f64_sub(fa, fb));
	  break;
	case 0x22: /* mult */
	  rc = sf_bits64(f64_mul(fa, fb));
	  break;
	case 0x23: /* divt */
	  rc = sf_bits64(f64_div(fa, fb));
	  break;
	case 0x24: /* cmptun : true if either is NaN */
	  rc = (f64_eq(fa, fa) && f64_eq(fb, fb)) ? 0 : 0x4000000000000000UL;
	  break;
	case 0x25: /* cmpteq */
	  rc = f64_eq(fa, fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x26: /* cmptlt */
	  rc = f64_lt(fa, fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x27: /* cmptle */
	  rc = f64_le(fa, fb) ? 0x4000000000000000UL : 0;
	  break;
	case 0x2c: /* cvtts : T -> S */
	  rc = sf_s_to_reg(f64_to_f32(fb));
	  break;
	case 0x2f: /* cvttq : T -> quadword integer */
	  rc = static_cast<uint64_t>(f64_to_i64(fb, softfloat_roundingMode, true));
	  break;
	case 0x3c: /* cvtqs : quadword integer -> S */
	  rc = sf_s_to_reg(i64_to_f32(vb));
	  break;
	case 0x3e: /* cvtqt : quadword integer -> T */
	  rc = sf_bits64(i64_to_f64(vb));
	  break;
	default:
	  ok = false;
	  break;
	}
      if(!ok) {
	goto report_unimplemented;
      }
      s->fpr[m.f.rc] = rc;
      s->fpr[31] = 0;
      break;
    }

    case 0x17: { /* FLTL : copies, fp cmov, fpcr, longword converts.
		  * cmov tests interpret Fa as a T-value; softfloat
		  * compares against zero handle NaN correctly. */
      uint64_t va = s->fpr[m.f.ra];
      uint64_t vb = s->fpr[m.f.rb];
      float64_t fa = sf_f64(va);
      float64_t zero = sf_f64(0);
      bool a_eq0 = fp_is_zero(va);
      bool a_lt0 = f64_lt(fa, zero);
      bool a_le0 = f64_le(fa, zero);
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
	  if(a_eq0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02b: /* fcmovne */
	  if(!a_eq0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02c: /* fcmovlt */
	  if(a_lt0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02d: /* fcmovge */
	  if(!a_lt0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02e: /* fcmovle */
	  if(a_le0) {
	    s->fpr[m.f.rc] = vb;
	  }
	  break;
	case 0x02f: /* fcmovgt */
	  if(!a_le0) {
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
	  s->did_rpcc = true;
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
	case 0x70: /* ftoit (FIX) : fp reg Fa -> int reg (raw copy) */
	  s->gpr[m.o.rc] = s->fpr[m.o.ra];
	  break;
	case 0x78: /* ftois (FIX) : fp reg Fa -> int reg in S memory format */
	  s->gpr[m.o.rc] = sext32(sf_reg_to_s(s->fpr[m.o.ra]).v);
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
      if(s->log_store) {
	s->log_store(pc, s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      }
      s->store32(s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      break;
    case 0x2d: /* stq */
      if(s->log_store) {
	s->log_store(pc, s->gpr[m.m.rb] + sext16(m.m.disp), s->gpr[m.m.ra]);
      }
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
    case 0x31: /* fbeq : sign/zero bit-test of the fp register */
      if(fp_is_zero(s->fpr[m.b.ra])) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x32: /* fblt : negative and nonzero */
      if(((s->fpr[m.b.ra] >> 63) & 1) && !fp_is_zero(s->fpr[m.b.ra])) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x33: /* fble : negative or zero */
      if(((s->fpr[m.b.ra] >> 63) & 1) || fp_is_zero(s->fpr[m.b.ra])) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x34: /* bsr */
      s->gpr[m.b.ra] = npc;
      npc = npc + (sext21(m.b.disp) << 2);
      break;
    case 0x35: /* fbne : nonzero */
      if(!fp_is_zero(s->fpr[m.b.ra])) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x36: /* fbge : positive or zero */
      if((((s->fpr[m.b.ra] >> 63) & 1) == 0) || fp_is_zero(s->fpr[m.b.ra])) {
	npc = npc + (sext21(m.b.disp) << 2);
      }
      break;
    case 0x37: /* fbgt : positive and nonzero */
      if((((s->fpr[m.b.ra] >> 63) & 1) == 0) && !fp_is_zero(s->fpr[m.b.ra])) {
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
    opcdec:
    report_unimplemented:
      /* reserved/illegal opcode.  with a PAL image loaded this is the
       * architected OPCDEC fault - vector to PAL; otherwise it is the
       * old fatal path. */
      if(s->pal_loaded) {
	npc = enter_pal(s, pc, PAL_OPCDEC);
	break;
      }
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
