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

int main(int argc, char *argv[]) {
  namespace po = boost::program_options;
  std::string binary;
  uint64_t maxicnt = ~0UL;
  bool dump_icnt = false;
  try {
    po::options_description desc("options");
    desc.add_options()
      ("help", "print help")
      ("file,f", po::value<std::string>(&binary), "alpha binary")
      ("maxicnt,m", po::value<uint64_t>(&maxicnt)->default_value(~0UL), "maximum icnt")
      ("icnt,i", po::value<bool>(&dump_icnt)->default_value(false), "report icnt at exit");
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if(vm.count("help") || binary.empty()) {
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
  if(!load_alpha_elf(binary.c_str(), s)) {
    return -1;
  }
  s->maxicnt = maxicnt;
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

  runAlpha(s);

  if(s->brk == 0) {
    fprintf(stderr, "alpha_iss: hit maxicnt at pc %lx\n", s->pc);
  }
  if(dump_icnt) {
    fprintf(stderr, "alpha_iss: %lu insns retired, exit code %d\n",
	    s->icnt, s->exit_code);
  }
  return s->exit_code;
}
