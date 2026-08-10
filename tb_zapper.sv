`include "machine.vh"
`include "uop.vh"

/* unit-test wrapper : drive the zapper with an opcode index so the
 * harness doesn't need enum values.  idx maps 0..25 in a fixed order. */
module tb_zapper(
		 input logic [4:0] idx,
		 input logic [63:0] a,
		 input logic [63:0] b,
		 output logic [63:0] y);

   opcode_t t_op;
   always_comb
     begin
	t_op = NOP;
	case(idx)
	  5'd0: t_op = ZAP;
	  5'd1: t_op = ZAPNOT;
	  5'd2: t_op = CMPBGE;
	  5'd3: t_op = EXTBL;
	  5'd4: t_op = EXTWL;
	  5'd5: t_op = EXTLL;
	  5'd6: t_op = EXTQL;
	  5'd7: t_op = EXTWH;
	  5'd8: t_op = EXTLH;
	  5'd9: t_op = EXTQH;
	  5'd10: t_op = INSBL;
	  5'd11: t_op = INSWL;
	  5'd12: t_op = INSLL;
	  5'd13: t_op = INSQL;
	  5'd14: t_op = INSWH;
	  5'd15: t_op = INSLH;
	  5'd16: t_op = INSQH;
	  5'd17: t_op = MSKBL;
	  5'd18: t_op = MSKWL;
	  5'd19: t_op = MSKLL;
	  5'd20: t_op = MSKQL;
	  5'd21: t_op = MSKWH;
	  5'd22: t_op = MSKLH;
	  5'd23: t_op = MSKQH;
	  default:
	    begin
	    end
	endcase // case (idx)
     end // always_comb

   alpha_zapper z0
     (
      .op(t_op),
      .a(a),
      .b(b),
      .y(y)
      );

endmodule // tb_zapper
