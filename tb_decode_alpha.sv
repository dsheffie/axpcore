`include "machine.vh"
`include "uop.vh"

/* unit-test wrapper : flattens the uop struct into discrete ports so
 * the verilator harness can check decode of real binaries */
module tb_decode_alpha(
		       input logic [31:0] insn,
		       input logic [63:0] pc,
		       output logic [7:0] op,
		       output logic	  dst_valid,
		       output logic	  srcA_valid,
		       output logic	  srcB_valid,
		       output logic	  is_mem,
		       output logic	  is_store,
		       output logic	  is_br,
		       output logic	  is_int,
		       output logic	  serializing,
		       output logic [6:0] dst,
		       output logic [6:0] srcA,
		       output logic [6:0] srcB,
		       output logic [63:0] rvimm,
		       output logic [15:0] imm);

   uop_t uop;

   decode_alpha d0
     (
      .insn(insn),
      .page_fault(1'b0),
      .bad_page_permissions(1'b0),
      .irq(1'b0),
      .pc(pc),
      .insn_pred(1'b0),
      .bpu_idx('d0),
      .insn_pred_target('d0),
`ifdef ENABLE_CYCLE_ACCOUNTING
      .fetch_cycle('d0),
`endif
      .syscall_emu(1'b1),
      .uop(uop)
      );

   assign op = uop.op;
   assign dst_valid = uop.dst_valid;
   assign srcA_valid = uop.srcA_valid;
   assign srcB_valid = uop.srcB_valid;
   assign is_mem = uop.is_mem;
   assign is_store = uop.is_store;
   assign is_br = uop.is_br;
   assign is_int = uop.is_int;
   assign serializing = uop.serializing_op;
   assign dst = uop.dst;
   assign srcA = uop.srcA;
   assign srcB = uop.srcB;
   assign rvimm = uop.rvimm;
   assign imm = uop.imm;

endmodule // tb_decode_alpha
