// Prove the decoder never emits a valid integer dest of logical r31
// (the alpha zero register).  This is the invariant that makes a
// phys-reg-0 write impossible : reset maps r31 -> phys 0 forever and
// rf6r3w hardwires reads of ptr 0 to zero, so a renamed r31 dest
// would silently break the zero register.
// Expose the violation as an output; yosys `sat -set bad 1` must be
// UNSAT.  Ported from r9999's formal_decode (mips $0 flavor).
module formal_decode(
   input logic [31:0] 		insn,
   input logic 			page_fault,
   input logic 			bad_page_permissions,
   input logic 			irq,
   input logic [`M_WIDTH-1:0] 	pc,
   input logic 			insn_pred,
   input logic [`LG_BPU_TBL_SZ-1:0] bpu_idx,
   input logic [`M_WIDTH-1:0] 	insn_pred_target,
   input logic 			syscall_emu,
   output logic 		dv,     // dst_valid  (sanity: must be reachable =1)
   output logic 		bad     // dst_valid & (dst[4:0]==31)  (invariant: must be unreachable =1)
);
   uop_t uop;
   decode_alpha dec(
      .insn(insn),
      .page_fault(page_fault),
      .bad_page_permissions(bad_page_permissions),
      .irq(irq),
      .pc(pc),
      .insn_pred(insn_pred),
      .bpu_idx(bpu_idx),
      .insn_pred_target(insn_pred_target),
      .syscall_emu(syscall_emu),
      .uop(uop)
   );
   assign dv  = uop.dst_valid;
   assign bad = uop.dst_valid & (uop.dst[4:0] == 5'd31);
endmodule
