/* standalone driver for the alpha ISS : loads a static alpha ELF and
 * runs it to completion.  diff-test against qemu-alpha with the same
 * binary. */
#include "alpha_interp.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <elf.h>
#include <string>
#include <iostream>
#include <boost/program_options.hpp>

#ifndef EM_ALPHA_UNOFFICIAL
#define EM_ALPHA_UNOFFICIAL 0x9026
#endif

/* alpha static images link at 0x120000000 - keep the default layout
 * (qemu parity) and back it with an 8GB sparse anonymous mapping */
static const uint64_t MEM_SZ = 8UL << 30;
static const uint64_t STACK_TOP = 0x140000000UL;

/* OSF kseg : kernel virtual - PAGE_OFFSET = physical */
static const uint64_t PAGE_OFFSET = 0xfffffc0000000000UL;
static const uint64_t HWRPB_PA   = 0x10000000UL;   /* INIT_HWRPB */
static const uint64_t INITRD_PA  = 0x08000000UL;   /* 128MB : below hwrpb */

/* system-mode boot : load a kernel ELF into its kseg-mapped physical
 * pages, build a minimal HWRPB the kernel can parse, and start at the
 * entry point.  PAL is serviced in C (see handle_pal_call).  drives
 * the die-on-unimplemented scoping loop. */
static bool boot_kernel(alpha_state_t *s, const char *kfn, const char *ifn) {
  int fd = open(kfn, O_RDONLY);
  if(fd < 0) {
    fprintf(stderr, "alpha_iss: cannot open kernel %s\n", kfn);
    return false;
  }
  struct stat st;
  fstat(fd, &st);
  char *buf = static_cast<char*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  const Elf64_Ehdr *eh = reinterpret_cast<const Elf64_Ehdr*>(buf);
  const Elf64_Phdr *ph = reinterpret_cast<const Elf64_Phdr*>(buf + eh->e_phoff);
  for(int i = 0; i < eh->e_phnum; i++) {
    if(ph[i].p_type != PT_LOAD) {
      continue;
    }
    uint64_t pa = ph[i].p_vaddr - PAGE_OFFSET;   /* kseg -> physical */
    memcpy(s->mem + pa, buf + ph[i].p_offset, ph[i].p_filesz);
  }
  s->pc = eh->e_entry;   /* kseg virtual entry */
  munmap(buf, st.st_size);

  uint64_t initrd_start = 0, initrd_size = 0;
  if(ifn) {
    int ifd = open(ifn, O_RDONLY);
    if(ifd < 0) {
      fprintf(stderr, "alpha_iss: cannot open initrd %s\n", ifn);
      return false;
    }
    struct stat is;
    fstat(ifd, &is);
    char *ib = static_cast<char*>(mmap(nullptr, is.st_size, PROT_READ, MAP_PRIVATE, ifd, 0));
    close(ifd);
    memcpy(s->mem + INITRD_PA, ib, is.st_size);
    munmap(ib, is.st_size);
    initrd_start = PAGE_OFFSET + INITRD_PA;   /* kseg va */
    initrd_size = is.st_size;
  }

  /* build the HWRPB at physical HWRPB_PA.  layout : HWRPB(320) then a
   * per-cpu slot then the memory descriptor table. */
  uint64_t H = HWRPB_PA;
  auto q = [&](uint64_t off, uint64_t v) { s->phys_store64(H + off, v); };
  memset(s->mem + H, 0, 0x400);
  q(0, HWRPB_PA);                        /* phys_addr */
  memcpy(s->mem + H + 8, "HWRPB\0\0", 8);/* id */
  q(16, 5);                             /* revision */
  q(24, 320);                           /* size */
  q(32, 0);                            /* cpuid */
  q(40, 8192);                         /* pagesize */
  q(48, 40);                           /* pa_bits */
  q(56, 255);                          /* max_asn */
  q(80, 13);                           /* sys_type = ST_DEC_2100_A50 (Avanti) */
  q(88, 0);                            /* sys_variation */
  q(96, 0);                            /* sys_revision */
  q(104, 1024UL * 4096);               /* intr_freq (HZ<<12) */
  q(112, 500000000UL);                 /* cycle_freq (500MHz) */
  q(120, 0xfffffffe00000000UL);        /* vptb */
  q(144, 1);                           /* nr_processors */
  q(152, 128);                         /* processor_size */
  q(160, 320);                         /* processor_offset */
  uint64_t mddt_off = 320 + 128;       /* after the per-cpu slot */
  q(200, mddt_off);                    /* mddt_offset */

  /* per-cpu slot at H+320 : mark the CPU present + EV6 type */
  s->phys_store64(H + 320 + 0, 1);     /* per_cpu.type ? placeholder */

  /* memory descriptor table : cluster 0 low reserved, cluster 1 free */
  uint64_t M = H + mddt_off;
  auto mq = [&](uint64_t off, uint64_t v) { s->phys_store64(M + off, v); };
  mq(0, 0);                            /* chksum */
  mq(8, 0);                            /* optional_pa */
  mq(16, 2);                           /* numclusters */
  uint64_t c0 = M + 24, c1 = c0 + 56;
  /* cluster 0 : pfn 0..15 reserved (console/PAL) */
  s->phys_store64(c0 + 0, 0);          /* start_pfn */
  s->phys_store64(c0 + 8, 512);        /* numpages : cover hwrpb region */
  s->phys_store64(c0 + 48, 1);         /* usage = reserved */
  /* cluster 1 : the rest free (up to 128MB) */
  s->phys_store64(c1 + 0, 512);        /* start_pfn */
  s->phys_store64(c1 + 8, 16384 - 512);/* numpages */
  s->phys_store64(c1 + 48, 0);         /* usage = free */

  /* HWRPB checksum : sum of the 36 quadwords preceding chksum(288) */
  uint64_t sum = 0;
  for(int i = 0; i < 36; i++) {
    sum += s->phys_load64(H + 8 * i);
  }
  q(288, sum);

  /* pass initrd via the standard alpha convention : the kernel reads
   * it from the command line / a bootp block.  for now stash in known
   * globals the kernel picks up (INITRD start/size fields are set by
   * the console into the HWRPB's optional area / bootp).  TODO refine
   * once we see how this kernel wants it. */
  (void)initrd_start;
  (void)initrd_size;

  s->system_mode = true;
  s->palmode = false;
  s->ipr[IPR_PS] = 0;   /* kernel mode, IPL 0 */
  return true;
}

/* find __log_buf's physical address by scanning the kernel ELF symtab
 * (so the printk dump is automatic) */
static uint64_t find_logbuf_pa(const char *kfn) {
  int fd = open(kfn, O_RDONLY);
  if(fd < 0) {
    return 0;
  }
  struct stat st;
  fstat(fd, &st);
  char *buf = static_cast<char*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  const Elf64_Ehdr *eh = reinterpret_cast<const Elf64_Ehdr*>(buf);
  const Elf64_Shdr *sh = reinterpret_cast<const Elf64_Shdr*>(buf + eh->e_shoff);
  uint64_t va = 0;
  for(int i = 0; i < eh->e_shnum && !va; i++) {
    if(sh[i].sh_type != SHT_SYMTAB) {
      continue;
    }
    const char *strtab = buf + sh[sh[i].sh_link].sh_offset;
    const Elf64_Sym *sym = reinterpret_cast<const Elf64_Sym*>(buf + sh[i].sh_offset);
    for(uint32_t j = 0; j < sh[i].sh_size / sizeof(Elf64_Sym); j++) {
      if(strcmp(strtab + sym[j].st_name, "__log_buf") == 0) {
	va = sym[j].st_value;
	break;
      }
    }
  }
  munmap(buf, st.st_size);
  return va ? va - PAGE_OFFSET : 0;
}

static bool load_alpha_elf(const char *fn, alpha_state_t *s) {
  int fd = open(fn, O_RDONLY);
  if(fd < 0) {
    fprintf(stderr, "alpha_iss: cannot open %s\n", fn);
    return false;
  }
  struct stat st;
  fstat(fd, &st);
  uint8_t *buf = static_cast<uint8_t*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  const Elf64_Ehdr *eh = reinterpret_cast<const Elf64_Ehdr*>(buf);
  if(memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
     eh->e_ident[EI_CLASS] != ELFCLASS64 ||
     eh->e_ident[EI_DATA] != ELFDATA2LSB) {
    fprintf(stderr, "alpha_iss: %s is not a little-endian ELF64\n", fn);
    return false;
  }
  if(eh->e_machine != EM_ALPHA_UNOFFICIAL && eh->e_machine != EM_FAKE_ALPHA) {
    fprintf(stderr, "alpha_iss: %s is not an alpha binary (e_machine %x)\n",
	    fn, eh->e_machine);
    return false;
  }
  const Elf64_Phdr *ph = reinterpret_cast<const Elf64_Phdr*>(buf + eh->e_phoff);
  uint64_t max_end = 0;
  for(int i = 0; i < eh->e_phnum; i++) {
    if(ph[i].p_type != PT_LOAD) {
      continue;
    }
    if(ph[i].p_vaddr + ph[i].p_memsz > MEM_SZ) {
      fprintf(stderr, "alpha_iss: segment at %lx beyond memory image\n",
	      static_cast<uint64_t>(ph[i].p_vaddr));
      return false;
    }
    memcpy(s->mem + ph[i].p_vaddr, buf + ph[i].p_offset, ph[i].p_filesz);
    /* memsz > filesz tail is bss - mem starts zeroed */
    if(ph[i].p_vaddr + ph[i].p_memsz > max_end) {
      max_end = ph[i].p_vaddr + ph[i].p_memsz;
    }
  }
  s->pc = eh->e_entry;
  s->brk_addr = (max_end + 8191UL) & ~8191UL;

  /* htif symbols for call_pal 0xb0 binaries */
  const Elf64_Shdr *sh = reinterpret_cast<const Elf64_Shdr*>(buf + eh->e_shoff);
  int32_t strtabidx = 0, symtabidx = 0;
  for(int32_t i = 0; i < eh->e_shnum; i++) {
    if(sh[i].sh_type == SHT_SYMTAB) {
      symtabidx = i;
      strtabidx = sh[i].sh_link;
    }
  }
  if(strtabidx && symtabidx) {
    const char *strtab = reinterpret_cast<const char*>(buf + sh[strtabidx].sh_offset);
    const Elf64_Sym *sym = reinterpret_cast<const Elf64_Sym*>(buf + sh[symtabidx].sh_offset);
    for(uint32_t i = 0; i < (sh[symtabidx].sh_size / sizeof(Elf64_Sym)); i++) {
      if(strcmp(strtab + sym[i].st_name, "tohost") == 0) {
	s->tohost_addr = sym[i].st_value;
      }
      if(strcmp(strtab + sym[i].st_name, "fromhost") == 0) {
	s->fromhost_addr = sym[i].st_value;
      }
    }
  }
  munmap(buf, st.st_size);
  return true;
}

/* load a PAL ELF : copy its PT_LOAD segments into the flat image and
 * set PAL_BASE to the lowest loaded vaddr (offset 0 of the PAL image
 * is the entry-point table).  the PAL image is linked at a fixed
 * physical base below the user text. */
static bool load_pal_image(const char *fn, alpha_state_t *s) {
  int fd = open(fn, O_RDONLY);
  if(fd < 0) {
    fprintf(stderr, "alpha_iss: cannot open pal image %s\n", fn);
    return false;
  }
  struct stat st;
  fstat(fd, &st);
  char *buf = static_cast<char*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  const Elf64_Ehdr *eh = reinterpret_cast<const Elf64_Ehdr*>(buf);
  const Elf64_Phdr *ph = reinterpret_cast<const Elf64_Phdr*>(buf + eh->e_phoff);
  for(int i = 0; i < eh->e_phnum; i++) {
    if(ph[i].p_type != PT_LOAD) {
      continue;
    }
    memcpy(s->mem + ph[i].p_vaddr, buf + ph[i].p_offset, ph[i].p_filesz);
  }
  /* PAL_BASE is the reset vector = the PAL image entry point (the
   * PT_LOAD vaddr can be 0 since it covers the ELF headers) */
  s->ipr[IPR_PAL_BASE] = eh->e_entry;
  munmap(buf, st.st_size);
  s->pal_loaded = true;
  return true;
}

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;
  std::string binary, palimage, kernel, initrd;
  uint64_t maxicnt = ~0UL;
  bool dump_icnt = false;
  try {
    po::options_description desc("options");
    desc.add_options()
      ("help", "print help")
      ("file,f", po::value<std::string>(&binary), "alpha binary")
      ("kernel,k", po::value<std::string>(&kernel), "kernel vmlinux (ELF) : system-mode boot")
      ("initrd,r", po::value<std::string>(&initrd), "initramfs image for system-mode boot")
      ("palcode,p", po::value<std::string>(&palimage), "PAL image (ELF) : enables real PAL dispatch")
      ("maxicnt,m", po::value<uint64_t>(&maxicnt)->default_value(~0UL), "maximum icnt")
      ("icnt,i", po::value<bool>(&dump_icnt)->default_value(false), "report icnt at exit");
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if(vm.count("help") || (binary.empty() && kernel.empty())) {
      std::cout << desc << "\n";
      return 1;
    }
  }
  catch(po::error &e) {
    std::cerr << "command-line error : " << e.what() << "\n";
    return -1;
  }

  alpha_state_t *s = new alpha_state_t;
  memset(s, 0, sizeof(*s));
  s->mem = static_cast<uint8_t*>(mmap(nullptr, MEM_SZ, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if(s->mem == MAP_FAILED) {
    fprintf(stderr, "alpha_iss: mmap failed\n");
    return -1;
  }
  s->maxicnt = maxicnt;

  if(!kernel.empty()) {
    /* system-mode kernel boot */
    if(!boot_kernel(s, kernel.c_str(), initrd.empty() ? nullptr : initrd.c_str())) {
      return -1;
    }
  }
  else {
    if(!load_alpha_elf(binary.c_str(), s)) {
      return -1;
    }
    if(!palimage.empty() && !load_pal_image(palimage.c_str(), s)) {
      return -1;
    }
    s->mmap_addr = 0x200000000UL;

    /* linux process ABI : sp points at argc / argv / envp / auxv.
     * static glibc _start consumes this. */
    uint64_t sp = STACK_TOP - 4096;
    uint64_t prog_str = sp + 512;
    uint64_t rand_bytes = sp + 544;
    strcpy(reinterpret_cast<char*>(s->mem + prog_str), "alpha_bin");
    for(int i = 0; i < 16; i++) {
      s->mem[rand_bytes + i] = 0x5a ^ i;
    }
    uint64_t v[] = {
      1, prog_str, 0, /* argc, argv[0], null */
      0, /* empty envp */
      6, 8192, /* AT_PAGESZ */
      17, 100, /* AT_CLKTCK */
      11, 1000, 12, 1000, 13, 1000, 14, 1000, /* uid/euid/gid/egid */
      23, 0, /* AT_SECURE */
      16, 0, /* AT_HWCAP */
      25, rand_bytes, /* AT_RANDOM */
      0, 0 /* AT_NULL */
    };
    memcpy(s->mem + sp, v, sizeof(v));
    s->gpr[30] = sp;
  }

  runAlpha(s);

  /* system-mode debug : dump the kernel printk ring buffer so we can
   * read the boot messages even before a console device works.  the
   * 5.10 ringbuffer interleaves record headers with text; just scrub
   * out the printable runs.  __log_buf physical addr passed via env. */
  if(!kernel.empty()) {
    uint64_t lb = find_logbuf_pa(kernel.c_str());
    if(lb) {
      fprintf(stderr, "\n===== kernel printk buffer =====\n");
      int run = 0;
      for(uint64_t i = 0; i < (1u << 17); i++) {
	uint8_t c = s->mem[lb + i];
	if(c >= 0x20 && c < 0x7f) {
	  fputc(c, stderr);
	  run++;
	}
	else if(c == '\n') {
	  fputc('\n', stderr);
	  run = 0;
	}
	else {
	  if(run > 4) {
	    fputc('\n', stderr);
	  }
	  run = 0;
	}
      }
      fprintf(stderr, "\n===== end printk =====\n");
    }
  }

  if(s->brk == 0) {
    fprintf(stderr, "alpha_iss: hit maxicnt at pc %lx\n", s->pc);
  }
  if(dump_icnt) {
    fprintf(stderr, "alpha_iss: %lu insns retired, exit code %d\n",
	    s->icnt, s->exit_code);
  }
  return s->exit_code;
}
