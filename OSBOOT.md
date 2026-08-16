# axpcore : booting Linux on the ISS (system mode)

Goal : boot a real Linux/alpha kernel on the ISS, using die-on-
unimplemented to enumerate what the RTL will eventually need.  The ISS
is the scoping vehicle (see PALCODE.md for the privileged-model plan).

## the kernel image (validated on qemu first)

Built in `../alpha-boot/` (NOT in this repo) : linux-v5.10,
`CONFIG_ALPHA_GENERIC`, `-mcpu=ev6` (needs our FPU/FIX), cross-gcc-10.
A clean `git worktree` off the RISC-V tree; had to neutralize the
RISC-V debug hooks (`csr_read(CSR_INSTRET)` in init/main.c, the
CSR-0xc03 console in printk.c) that live in arch-generic files.
Config for a headless boot : `BLK_DEV_INITRD=y`, disable floppy / IDE /
i8042 (the i8042 PS/2 probe HANGS on qemu-clipper).  Boots on
`qemu-system-alpha -M clipper -cpu ev6 -kernel vmlinux -initrd ...`
all the way to `Run /init` - that's the known-good reference.

## system-mode ISS (done : boots to first printk)

`./alpha_iss -k vmlinux [-r initramfs] [-m maxicnt]`

- **loader** (boot_kernel) : the kernel links at kseg virtual
  0xfffffc0000310000; load each PT_LOAD at pa = vaddr - PAGE_OFFSET
  (PAGE_OFFSET = 0xfffffc0000000000).  entry = e_entry (kseg va).
- **translation** (alpha_state_t::xlate) : system mode maps kseg
  (va >= PAGE_OFFSET -> pa = va - PAGE_OFFSET) direct, low addresses
  identity (early boot + the HWRPB at phys 0x10000000).  the general
  ptbr page-table walk is a TODO (kernel is almost all kseg early).
  hw_ld/hw_st use the raw phys_* accessors (physical by definition).
- **HWRPB** : built at phys 0x10000000 (INIT_HWRPB) with the fields the
  kernel parses - id "HWRPB", pagesize 8192, cycle_freq, sys_type =
  ST_DEC_2100_A50 (Avanti), a per-cpu slot, and a memory descriptor
  table (2 clusters : low reserved + free RAM to 128MB).  checksum =
  sum of the 36 leading quadwords.
- **PAL-in-C** (handle_pal_call) : CALL_PAL serviced directly in the
  interpreter (no PAL asm) - halt, draina, imb, wrfen, rd/wrmces,
  wrvptptr, wrent, swpipl, rdps, wrkgp, wr/rdusp, whami, rd/wrunique.
  unimplemented ones exit(2) with a message = the scoping signal.
- **printk dump** : on exit the driver scans the kernel ELF for
  __log_buf and dumps the printable runs - so we read the boot log
  even before any console device works.

**milestone reached** : the kernel loads, enters, runs head.S, jsr's
into start_kernel, runs ~7037 instructions of early setup, and logs

    Linux version 5.10.0-... (alpha-linux-gnu-gcc-10 ...)

into __log_buf - dumped from the ISS's own memory.  it then halts
trying to FLUSH that message via the SRM console.

## next : the console handoff (where it halts now)

At instruction ~7037 the kernel calls `srm_fixup` (the early SRM
console output path).  it reads `hwrpb->crb_offset`, computes the CRB
(console routine block), loads the DISPATCH routine descriptor, and
`jmp`s to it.  crb_offset is 0 -> jumps to a null pointer -> halt.

two ways forward (matches the PALCODE.md discussion) :

1. **SRM CRB trampoline** : build a real CRB whose DISPATCH entry
   points at a tiny alpha stub in memory (`call_pal 0xNN ; ret`); the
   ISS intercepts 0xNN and implements the SRM console PUTS/GETC in C,
   then returns.  gets console output immediately (the kernel is
   already calling it) but needs the DISPATCH calling convention
   (routine selector + args) decoded.

2. **MILO model (preferred, per plan)** : make the kernel think it did
   NOT boot from SRM (srm_fixup branches to `nosrm`) by setting the
   HWRPB/CTB so `alpha_using_srm` = 0, then emulate an 8250 at its
   Avanti/APECS ISA I/O physical address and use `console=ttyS0`.
   this is the PC-like path we're targeting; the 8250 is trivial
   (THR write -> putchar, LSR read -> ready).

Recommended : chase (2) - find what sets `alpha_using_srm`, present a
non-SRM HWRPB, then add the 8250 device.  the die-on-unimplemented log
after that will show the next platform (APECS/SIO) access.

## milestone ladder (M1 done)

- **M1 : kernel logs "Linux version"** - DONE (printk buffer dump)
- M2 : live console (8250) - see boot messages stream
- M3 : memory init / page tables - forces the ptbr page-table walk
- M4 : timer + interrupts (PAL rti/swpctx, the interrupt vector)
- M5 : initramfs unpack, /init execs -> userspace
