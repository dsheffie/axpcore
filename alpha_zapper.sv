`include "machine.vh"
`include "uop.vh"

/* alpha byte-zapper : ext/ins/msk (b/w/l/q x low/high), zap/zapnot,
 * cmpbge.  a = the data operand (Ra), b = the shift/select operand
 * (Rb or lit8).  shifts are byte-granular (3-bit byte number), so the
 * muxes are cheap.  the high-half ops use the architected mod-64
 * shift : byte number 0 means shift by zero, not 64. */

module alpha_zapper(op, a, b, y);

   input opcode_t op;
   input logic [63:0] a;
   input logic [63:0] b;
   output logic [63:0] y;

   function logic [63:0] byte_mask8(logic [7:0] m);
      logic [63:0] x;
      for(integer i = 0; i < 8; i = i + 1)
	begin
	   x[8*i +: 8] = m[i] ? 8'hff : 8'h00;
	end
      return x;
   endfunction // byte_mask8

   wire [2:0]	w_bn = b[2:0];
   wire [2:0]	w_bn_h = 3'd0 - w_bn;
   wire [5:0]	w_shl = {w_bn, 3'd0};
   wire [5:0]	w_shh = {w_bn_h, 3'd0};

   logic [7:0]	t_wmask;
   always_comb
     begin
	t_wmask = 8'h00;
	case(op)
	  MSKBL, EXTBL, INSBL:
	    begin
	       t_wmask = 8'h01;
	    end
	  MSKWL, EXTWL, INSWL, MSKWH, EXTWH, INSWH:
	    begin
	       t_wmask = 8'h03;
	    end
	  MSKLL, EXTLL, INSLL, MSKLH, EXTLH, INSLH:
	    begin
	       t_wmask = 8'h0f;
	    end
	  MSKQL, EXTQL, INSQL, MSKQH, EXTQH, INSQH:
	    begin
	       t_wmask = 8'hff;
	    end
	  default:
	    begin
	    end
	endcase // case (op)
     end // always_comb

   /* width mask shifted to the byte position : low half keeps bits
    * 7:0, high half takes the spill into 15:8 */
   wire [15:0]	w_wmask_sh = {8'd0, t_wmask} << w_bn;
   wire [63:0]	w_bm_w = byte_mask8(t_wmask);
   wire [63:0]	w_bm_l = byte_mask8(w_wmask_sh[7:0]);
   wire [63:0]	w_bm_h = byte_mask8(w_wmask_sh[15:8]);
   wire [63:0]	w_bm_b = byte_mask8(b[7:0]);

   wire [63:0]	w_a_srl = a >> w_shl;
   wire [63:0]	w_a_sll = a << w_shl;
   wire [63:0]	w_a_srl_h = a >> w_shh;
   wire [63:0]	w_a_sll_h = a << w_shh;

   logic [7:0]	t_cmpbge;
   always_comb
     begin
	for(integer i = 0; i < 8; i = i + 1)
	  begin
	     t_cmpbge[i] = (a[8*i +: 8] >= b[8*i +: 8]);
	  end
     end // always_comb

   always_comb
     begin
	y = 'd0;
	case(op)
	  EXTBL, EXTWL, EXTLL, EXTQL:
	    begin
	       y = w_a_srl & w_bm_w;
	    end
	  EXTWH, EXTLH, EXTQH:
	    begin
	       y = w_a_sll_h & w_bm_w;
	    end
	  INSBL, INSWL, INSLL, INSQL:
	    begin
	       y = w_a_sll & w_bm_l;
	    end
	  INSWH, INSLH, INSQH:
	    begin
	       y = w_a_srl_h & w_bm_h;
	    end
	  MSKBL, MSKWL, MSKLL, MSKQL:
	    begin
	       y = a & (~w_bm_l);
	    end
	  MSKWH, MSKLH, MSKQH:
	    begin
	       y = a & (~w_bm_h);
	    end
	  ZAP:
	    begin
	       y = a & (~w_bm_b);
	    end
	  ZAPNOT:
	    begin
	       y = a & w_bm_b;
	    end
	  CMPBGE:
	    begin
	       y = {56'd0, t_cmpbge};
	    end
	  default:
	    begin
	    end
	endcase // case (op)
     end // always_comb

endmodule // alpha_zapper
