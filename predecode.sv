`include "machine.vh"

/*
typedef enum logic [3:0] {
			  NOT_CFLOW = 'd0,
			  IS_COND_BR = 'd1,
 			  IS_RET = 'd2,
			  IS_J = 'd3,
			  IS_JR = 'd4,
			  IS_JAL = 'd5,
			  IS_JALR = 'd6
			  } jump_t;
*/

module predecode(pc, insn, pd);
   input logic [63:0] pc;
   input logic [31:0] insn;
   output logic [`N_PD_BITS-1:0] pd;

   /* alpha : class comes straight from the opcode and the jmp hint
    * field - no abi inference needed.  the pd taxonomy maps 1:1
    * (see PORT.md). */
   always_comb
     begin
	pd = 'd0;
	case(insn[31:26])
	  6'h30, 6'h34: /* br / bsr */
	    begin
	       pd = (insn[25:21] == 5'd31) ? 'd3 /* j */ : 'd5 /* call */;
	    end
	  6'h38, 6'h39, 6'h3a, 6'h3b, 6'h3c, 6'h3d, 6'h3e, 6'h3f: /* cond branches */
	    begin
	       pd = 'd1;
	    end
	  6'h1a: /* jmp/jsr/ret/jsr_coroutine */
	    begin
	       case(insn[15:14])
		 2'd0: /* jmp */
		   begin
		      pd = (insn[25:21] == 5'd31) ? 'd4 /* jr */ : 'd6 /* indirect call */;
		   end
		 2'd1: /* jsr */
		   begin
		      pd = (insn[25:21] == 5'd31) ? 'd4 : 'd6;
		   end
		 2'd2: /* ret */
		   begin
		      pd = (insn[25:21] == 5'd31) ? 'd2 /* return */ : 'd6;
		   end
		 2'd3: /* jsr_coroutine : pop then push */
		   begin
		      pd = 'd7;
		   end
	       endcase // case (insn[15:14])
	    end
	  default:
	    begin
	    end
	endcase // case (insn[31:26])
     end // always_comb

endmodule // predecode
