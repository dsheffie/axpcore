`include "machine.vh"
`include "rob.vh"
`include "uop.vh"

/* alpha (EV4 integer subset + BWX, plus CIX bit counts) decoder.
 * drop-in replacement for decode_riscv : same port shape, one uop out.
 *
 * conventions :
 *  - reg-or-lit8 operand B : the literal form clears srcB_valid and
 *    carries the zero-extended literal in rvimm.  exec computes
 *    opB = srcB_valid ? srcB : rvimm, so each alpha instruction is a
 *    single opcode_t for both forms.
 *  - r31 is the zero register : dst_valid = (rc != 31), operates and
 *    loads targeting r31 fold to NOP.  reads of r31 go through the
 *    rename path and return the never-written phys reg 31 (zero).
 *  - branches test one register against zero : srcB_valid = 0.
 *    branch targets are precomputed absolute (pc + 4 + disp*4) and
 *    ride in rvimm, matching the rv64 decoder's convention.
 *  - register-form CMOVxx emits the CMOV_LO marker (cracked into two
 *    uops at dispatch, exactly the rv64 proof-point machinery);
 *    literal-form emits single-uop CMOV_LIT with srcB = old rc.
 *    condition code lives in imm[2:0] :
 *    0=eq 1=ne 2=lt 3=ge 4=le 5=gt 6=lbs 7=lbc */

module decode_alpha(
		    insn,
		    page_fault,
		    bad_page_permissions,
		    irq,
		    pc,
		    insn_pred,
		    bpu_idx,
		    insn_pred_target,
`ifdef ENABLE_CYCLE_ACCOUNTING
		    fetch_cycle,
`endif
		    syscall_emu,
		    uop);

   input logic [31:0] insn;
   input logic	      page_fault;
   input logic	      bad_page_permissions;
   input logic	      irq;
   input logic [`M_WIDTH-1:0] pc;
   input logic	      insn_pred;
   input logic [`LG_BPU_TBL_SZ-1:0] bpu_idx;

   input logic [`M_WIDTH-1:0]	insn_pred_target;
`ifdef ENABLE_CYCLE_ACCOUNTING
   input logic [63:0]		fetch_cycle;
`endif
   input logic			syscall_emu;
   output	uop_t uop;

   /* 6'h01 is an unallocated alpha opcode - park faulted/irq slots
    * there so the case falls through with the default op (CALL_PAL is
    * opcode 0, so forcing 0 like the rv64 decoder would misdecode) */
   wire [5:0]	opc = (page_fault|irq|bad_page_permissions) ? 6'h01 : insn[31:26];

   localparam ZP = (`LG_PRF_ENTRIES-5);
   wire [`LG_PRF_ENTRIES-1:0] ra = {{ZP{1'b0}}, insn[25:21]};
   wire [`LG_PRF_ENTRIES-1:0] rb = {{ZP{1'b0}}, insn[20:16]};
   wire [`LG_PRF_ENTRIES-1:0] rc = {{ZP{1'b0}}, insn[4:0]};

   wire		w_ra_is_z = (insn[25:21] == 5'd31);
   wire		w_rc_is_z = (insn[4:0] == 5'd31);

   wire [6:0]	w_func = insn[11:5];
   wire		w_is_lit = insn[12];
   wire [7:0]	w_lit = insn[20:13];
   wire [1:0]	w_hint = insn[15:14];

   localparam PP = (`M_WIDTH-16);
   wire [`M_WIDTH-1:0]	w_disp16 = {{PP{insn[15]}}, insn[15:0]};
   wire [`M_WIDTH-1:0]	w_lit64 = {{(`M_WIDTH-8){1'b0}}, w_lit};

   logic [`M_WIDTH-1:0]	t_imm;
   wire [`M_WIDTH-1:0]	w_pc_imm;

   /* branch displacement : signed insn count relative to the updated
    * pc, so target = pc + 4 + (sext(disp21) << 2).  the +4 folds into
    * the immediate. */
   always_comb
     begin
	t_imm = {{(`M_WIDTH-23){insn[20]}}, insn[20:0], 2'd0} + 'd4;
     end

   mwidth_add imm_add (.A(pc), .B(t_imm), .Y(w_pc_imm));

   always_comb
     begin
	uop.op = page_fault ? FETCH_PF :
		 bad_page_permissions ? FETCH_NOT_EXEC :
		 (irq ? IRQ : II);
	uop.rsb_ptr = 'd0;
	uop.srcA = 'd0;
	uop.srcB = 'd0;
	uop.dst = 'd0;
	uop.srcA_valid = 1'b0;
	uop.srcB_valid = 1'b0;

	uop.dst_valid = 1'b0;

	uop.imm = 16'd0;
	uop.jmp_imm = {(`M_WIDTH-16){1'b0}};
	uop.rvimm = 'd0;

	uop.pc = pc;
	uop.serializing_op = 1'b0;
	uop.must_restart = 1'b0;
	uop.rob_ptr = 'd0;
	uop.br_pred = 1'b0;
	uop.is_br = 1'b0;
	uop.bpu_idx = bpu_idx;
	uop.is_mem = 1'b0;
	uop.is_int = 1'b0;
	uop.is_cheap_int = 1'b0;
	uop.is_store = 1'b0;
`ifdef ENABLE_CYCLE_ACCOUNTING
	uop.fetch_cycle = fetch_cycle;
	uop.raw_insn = insn;
`endif
	case(opc)
	  6'h00: /* call_pal */
	    begin
	       if(syscall_emu && ((insn[25:0] == 26'h83) || (insn[25:0] == 26'hb0)))
		 begin
		    /* 0xb0 is our htif escape : magic-mem args via
		     * tohost, results written back to memory by the
		     * harness - same flow as the rv64 MONITOR.  0x83
		     * (callsys) also maps here for now. */
		    uop.op = MONITOR;
		    uop.serializing_op = 1'b1;
		    uop.must_restart = 1'b1;
		    uop.is_int = 1'b1;
		 end
	       else if(insn[25:0] == 26'h86)
		 begin
		    /* imb */
		    uop.op = FENCEI;
		    uop.serializing_op = 1'b1;
		    uop.must_restart = 1'b1;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h00
	  6'h08: /* lda */
	    begin
	       uop.op = w_ra_is_z ? NOP : ADDI;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	    end
	  6'h09: /* ldah */
	    begin
	       uop.op = w_ra_is_z ? NOP : ADDI;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = {{(PP-16){insn[15]}}, insn[15:0], 16'd0};
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	    end
	  6'h0a: /* ldbu */
	    begin
	       uop.op = w_ra_is_z ? NOP : LBU;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = !w_ra_is_z;
	    end
	  6'h0b: /* ldq_u */
	    begin
	       uop.op = w_ra_is_z ? NOP : LDQU;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = !w_ra_is_z;
	    end
	  6'h0c: /* ldwu */
	    begin
	       uop.op = w_ra_is_z ? NOP : LHU;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = !w_ra_is_z;
	    end
	  6'h0d: /* stw */
	    begin
	       uop.op = SH;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.is_store = 1'b1;
	    end
	  6'h0e: /* stb */
	    begin
	       uop.op = SB;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.is_store = 1'b1;
	    end
	  6'h0f: /* stq_u */
	    begin
	       uop.op = STQU;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.is_store = 1'b1;
	    end
	  6'h10: /* INTA : add/sub/scaled/compares */
	    begin
	       uop.dst = rc;
	       uop.dst_valid = !w_rc_is_z;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = rb;
	       uop.srcB_valid = !w_is_lit;
	       uop.rvimm = w_lit64;
	       uop.is_int = 1'b1;
`ifdef TWO_SRC_CHEAP
	       uop.is_cheap_int = 1'b1;
`else
	       uop.is_cheap_int = w_is_lit;
`endif
	       case(w_func)
		 7'h00: /* addl */
		   begin
		      uop.op = ADDW;
		   end
		 7'h02: /* s4addl */
		   begin
		      uop.op = S4ADDL;
		   end
		 7'h09: /* subl */
		   begin
		      uop.op = SUBW;
		   end
		 7'h0b: /* s4subl */
		   begin
		      uop.op = S4SUBL;
		   end
		 7'h0f: /* cmpbge */
		   begin
		      uop.op = CMPBGE;
		      uop.is_cheap_int = 1'b0;
		   end
		 7'h12: /* s8addl */
		   begin
		      uop.op = S8ADDL;
		   end
		 7'h1b: /* s8subl */
		   begin
		      uop.op = S8SUBL;
		   end
		 7'h1d: /* cmpult */
		   begin
		      uop.op = SLTU;
		   end
		 7'h20: /* addq */
		   begin
		      uop.op = ADDU;
		   end
		 7'h22: /* s4addq */
		   begin
		      uop.op = SH2ADD;
		   end
		 7'h29: /* subq */
		   begin
		      uop.op = SUBU;
		   end
		 7'h2b: /* s4subq */
		   begin
		      uop.op = S4SUBQ;
		   end
		 7'h2d: /* cmpeq */
		   begin
		      uop.op = CMPEQ;
		   end
		 7'h32: /* s8addq */
		   begin
		      uop.op = SH3ADD;
		   end
		 7'h3b: /* s8subq */
		   begin
		      uop.op = S8SUBQ;
		   end
		 7'h3d: /* cmpule */
		   begin
		      uop.op = CMPULE;
		   end
		 7'h40: /* addl/v : overflow traps not implemented */
		   begin
		      uop.op = ADDW;
		   end
		 7'h49: /* subl/v */
		   begin
		      uop.op = SUBW;
		   end
		 7'h4d: /* cmplt */
		   begin
		      uop.op = SLT;
		   end
		 7'h60: /* addq/v */
		   begin
		      uop.op = ADDU;
		   end
		 7'h69: /* subq/v */
		   begin
		      uop.op = SUBU;
		   end
		 7'h6d: /* cmple */
		   begin
		      uop.op = CMPLE;
		   end
		 default:
		   begin
		      uop.op = II;
		   end
	       endcase // case (w_func)
	       if(w_rc_is_z && (uop.op != II))
		 begin
		    uop.op = NOP;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h10
	  6'h11: /* INTL : logic + cmov */
	    begin
	       uop.dst = rc;
	       uop.dst_valid = !w_rc_is_z;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = rb;
	       uop.srcB_valid = !w_is_lit;
	       uop.rvimm = w_lit64;
	       uop.is_int = 1'b1;
`ifdef TWO_SRC_CHEAP
	       uop.is_cheap_int = 1'b1;
`else
	       uop.is_cheap_int = w_is_lit;
`endif
	       case(w_func)
		 7'h00: /* and */
		   begin
		      uop.op = AND;
		   end
		 7'h08: /* bic */
		   begin
		      uop.op = ANDN;
		   end
		 7'h14: /* cmovlbs */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd6;
		   end
		 7'h16: /* cmovlbc */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd7;
		   end
		 7'h20: /* bis */
		   begin
		      uop.op = OR;
		   end
		 7'h24: /* cmoveq */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd0;
		   end
		 7'h26: /* cmovne */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd1;
		   end
		 7'h28: /* ornot */
		   begin
		      uop.op = ORN;
		   end
		 7'h40: /* xor */
		   begin
		      uop.op = XOR;
		   end
		 7'h44: /* cmovlt */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd2;
		   end
		 7'h46: /* cmovge */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd3;
		   end
		 7'h48: /* eqv */
		   begin
		      uop.op = XNOR;
		   end
		 7'h61: /* amask : only BWX advertised */
		   begin
		      if(w_is_lit)
			begin
			   uop.op = ADDI;
			   uop.srcA = {{ZP{1'b0}}, 5'd31};
			   uop.rvimm = w_lit64 & ~64'h1;
			end
		      else
			begin
			   uop.op = ANDI;
			   uop.srcA = rb;
			   uop.rvimm = ~64'h1;
			   uop.srcB_valid = 1'b0;
			end
		   end
		 7'h64: /* cmovle */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd4;
		   end
		 7'h66: /* cmovgt */
		   begin
		      uop.op = w_is_lit ? CMOV_LIT : CMOV_LO;
		      uop.imm = 16'd5;
		   end
		 7'h6c: /* implver : 1 = EV5 family */
		   begin
		      uop.op = ADDI;
		      uop.srcA = {{ZP{1'b0}}, 5'd31};
		      uop.srcA_valid = 1'b1;
		      uop.rvimm = 'd1;
		   end
		 default:
		   begin
		      uop.op = II;
		   end
	       endcase // case (w_func)
	       if((uop.op == CMOV_LIT) || (uop.op == CMOV_LO))
		 begin
		    /* literal form reads the old destination as srcB;
		     * register form is the crack marker (dispatch
		     * rewrites srcB), never cheap */
		    uop.is_cheap_int = 1'b0;
		    if(uop.op == CMOV_LIT)
		      begin
			 uop.srcB = rc;
			 uop.srcB_valid = 1'b1;
		      end
		 end
	       if(w_rc_is_z && (uop.op != II))
		 begin
		    uop.op = NOP;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h11
	  6'h12: /* INTS : shifts + byte zapper */
	    begin
	       uop.dst = rc;
	       uop.dst_valid = !w_rc_is_z;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = rb;
	       uop.srcB_valid = !w_is_lit;
	       uop.rvimm = w_lit64;
	       uop.is_int = 1'b1;
	       case(w_func)
		 7'h02: /* mskbl */
		   begin
		      uop.op = MSKBL;
		   end
		 7'h06: /* extbl */
		   begin
		      uop.op = EXTBL;
		   end
		 7'h0b: /* insbl */
		   begin
		      uop.op = INSBL;
		   end
		 7'h12: /* mskwl */
		   begin
		      uop.op = MSKWL;
		   end
		 7'h16: /* extwl */
		   begin
		      uop.op = EXTWL;
		   end
		 7'h1b: /* inswl */
		   begin
		      uop.op = INSWL;
		   end
		 7'h22: /* mskll */
		   begin
		      uop.op = MSKLL;
		   end
		 7'h26: /* extll */
		   begin
		      uop.op = EXTLL;
		   end
		 7'h2b: /* insll */
		   begin
		      uop.op = INSLL;
		   end
		 7'h30: /* zap */
		   begin
		      uop.op = ZAP;
		   end
		 7'h31: /* zapnot */
		   begin
		      uop.op = ZAPNOT;
		   end
		 7'h32: /* mskql */
		   begin
		      uop.op = MSKQL;
		   end
		 7'h34: /* srl */
		   begin
		      uop.op = SRL;
`ifdef TWO_SRC_CHEAP
		      uop.is_cheap_int = 1'b1;
`else
		      uop.is_cheap_int = w_is_lit;
`endif
		   end
		 7'h36: /* extql */
		   begin
		      uop.op = EXTQL;
		   end
		 7'h39: /* sll */
		   begin
		      uop.op = SLL;
`ifdef TWO_SRC_CHEAP
		      uop.is_cheap_int = 1'b1;
`else
		      uop.is_cheap_int = w_is_lit;
`endif
		   end
		 7'h3b: /* insql */
		   begin
		      uop.op = INSQL;
		   end
		 7'h3c: /* sra */
		   begin
		      uop.op = SRA;
`ifdef TWO_SRC_CHEAP
		      uop.is_cheap_int = 1'b1;
`else
		      uop.is_cheap_int = w_is_lit;
`endif
		   end
		 7'h52: /* mskwh */
		   begin
		      uop.op = MSKWH;
		   end
		 7'h57: /* inswh */
		   begin
		      uop.op = INSWH;
		   end
		 7'h5a: /* extwh */
		   begin
		      uop.op = EXTWH;
		   end
		 7'h62: /* msklh */
		   begin
		      uop.op = MSKLH;
		   end
		 7'h67: /* inslh */
		   begin
		      uop.op = INSLH;
		   end
		 7'h6a: /* extlh */
		   begin
		      uop.op = EXTLH;
		   end
		 7'h72: /* mskqh */
		   begin
		      uop.op = MSKQH;
		   end
		 7'h77: /* insqh */
		   begin
		      uop.op = INSQH;
		   end
		 7'h7a: /* extqh */
		   begin
		      uop.op = EXTQH;
		   end
		 default:
		   begin
		      uop.op = II;
		   end
	       endcase // case (w_func)
	       if(w_rc_is_z && (uop.op != II))
		 begin
		    uop.op = NOP;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h12
	  6'h13: /* INTM */
	    begin
	       uop.dst = rc;
	       uop.dst_valid = !w_rc_is_z;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = rb;
	       uop.srcB_valid = !w_is_lit;
	       uop.rvimm = w_lit64;
	       uop.is_int = 1'b1;
	       case(w_func)
		 7'h00: /* mull */
		   begin
		      uop.op = MULW;
		   end
		 7'h20: /* mulq */
		   begin
		      uop.op = MUL;
		   end
		 7'h30: /* umulh */
		   begin
		      uop.op = MULHU;
		   end
		 7'h40: /* mull/v */
		   begin
		      uop.op = MULW;
		   end
		 7'h60: /* mulq/v */
		   begin
		      uop.op = MUL;
		   end
		 default:
		   begin
		      uop.op = II;
		   end
	       endcase // case (w_func)
	       if(w_rc_is_z && (uop.op != II))
		 begin
		    uop.op = NOP;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h13
	  6'h18: /* MISC : barriers + rpcc */
	    begin
	       case(insn[15:0])
		 16'h0000: /* trapb */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'h0400: /* excb */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'h4000: /* mb */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'h4400: /* wmb */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'h8000: /* fetch */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'ha000: /* fetch_m */
		   begin
		      uop.op = NOP;
		      uop.is_int = 1'b1;
		   end
		 16'hc000: /* rpcc */
		   begin
		      uop.op = w_ra_is_z ? NOP : RDCYCLE;
		      uop.dst = ra;
		      uop.dst_valid = !w_ra_is_z;
		      uop.is_int = 1'b1;
		      uop.serializing_op = !w_ra_is_z;
		   end
		 default:
		   begin
		   end
	       endcase // case (insn[15:0])
	    end // case: 6'h18
	  6'h1a: /* jmp/jsr/ret/jsr_coroutine : semantics identical,
		  * hint bits steer the rsb in fetch */
	    begin
	       uop.op = !w_ra_is_z ? JALR :
			(w_hint == 2'd2) ? RET : JR;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = 'd0;
	       uop.imm = insn_pred_target[15:0];
	       uop.jmp_imm = insn_pred_target[`M_WIDTH-1:16];
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = 1'b1;
	    end
	  6'h1c: /* sextb/sextw (BWX) + cix bit counts.  the operand is
		  * B : register form rides in srcA so the existing exec
		  * arms work unchanged, literal form constant-folds
		  * right here into an ADDI */
	    begin
	       uop.dst = rc;
	       uop.dst_valid = !w_rc_is_z;
	       uop.srcA = w_is_lit ? {{ZP{1'b0}}, 5'd31} : rb;
	       uop.srcA_valid = 1'b1;
	       uop.is_int = 1'b1;
	       case(w_func)
		 7'h00: /* sextb */
		   begin
		      uop.op = w_is_lit ? ADDI : SEXTB;
		      uop.rvimm = {{(`M_WIDTH-8){w_lit[7]}}, w_lit};
		   end
		 7'h01: /* sextw */
		   begin
		      uop.op = w_is_lit ? ADDI : SEXTH;
		      uop.rvimm = w_lit64;
		   end
		 7'h30: /* ctpop */
		   begin
		      uop.op = w_is_lit ? ADDI : CPOP;
		      uop.rvimm = {{(`M_WIDTH-4){1'b0}}, 4'($countones(w_lit))};
		   end
		 7'h32: /* ctlz */
		   begin
		      uop.op = w_is_lit ? ADDI : CLZ;
		      uop.rvimm = (w_lit == 8'd0) ? 'd64 :
				  (w_lit[7] == 1'b1) ? 'd56 :
				  (w_lit[6] == 1'b1) ? 'd57 :
				  (w_lit[5] == 1'b1) ? 'd58 :
				  (w_lit[4] == 1'b1) ? 'd59 :
				  (w_lit[3] == 1'b1) ? 'd60 :
				  (w_lit[2] == 1'b1) ? 'd61 :
				  (w_lit[1] == 1'b1) ? 'd62 : 'd63;
		   end
		 7'h33: /* cttz */
		   begin
		      uop.op = w_is_lit ? ADDI : CTZ;
		      uop.rvimm = (w_lit == 8'd0) ? 'd64 :
				  (w_lit[0] == 1'b1) ? 'd0 :
				  (w_lit[1] == 1'b1) ? 'd1 :
				  (w_lit[2] == 1'b1) ? 'd2 :
				  (w_lit[3] == 1'b1) ? 'd3 :
				  (w_lit[4] == 1'b1) ? 'd4 :
				  (w_lit[5] == 1'b1) ? 'd5 :
				  (w_lit[6] == 1'b1) ? 'd6 : 'd7;
		   end
		 default:
		   begin
		      uop.op = II;
		   end
	       endcase // case (w_func)
	       if(w_rc_is_z && (uop.op != II))
		 begin
		    uop.op = NOP;
		    uop.is_int = 1'b1;
		 end
	    end // case: 6'h1c
	  6'h28: /* ldl */
	    begin
	       uop.op = w_ra_is_z ? NOP : LW;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = !w_ra_is_z;
	    end
	  6'h29: /* ldq */
	    begin
	       uop.op = w_ra_is_z ? NOP : LD;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = !w_ra_is_z;
	    end
	  6'h2a: /* ldl_l */
	    begin
	       uop.op = LRW;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.serializing_op = 1'b1;
	    end
	  6'h2b: /* ldq_l */
	    begin
	       uop.op = LRD;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.serializing_op = 1'b1;
	    end
	  6'h2c: /* stl */
	    begin
	       uop.op = SW;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.is_store = 1'b1;
	    end
	  6'h2d: /* stq */
	    begin
	       uop.op = SD;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.is_store = 1'b1;
	    end
	  6'h2e: /* stl_c : ra is both store data and the 0/1 result */
	    begin
	       uop.op = SCW;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.serializing_op = 1'b1;
	    end
	  6'h2f: /* stq_c */
	    begin
	       uop.op = SCD;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.srcA = rb;
	       uop.srcA_valid = 1'b1;
	       uop.srcB = ra;
	       uop.srcB_valid = 1'b1;
	       uop.rvimm = w_disp16;
	       uop.is_mem = 1'b1;
	       uop.serializing_op = 1'b1;
	    end
	  6'h30: /* br */
	    begin
	       uop.op = w_ra_is_z ? J : JAL;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = 1'b1;
	    end
	  6'h34: /* bsr */
	    begin
	       uop.op = w_ra_is_z ? J : JAL;
	       uop.dst = ra;
	       uop.dst_valid = !w_ra_is_z;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = 1'b1;
	    end
	  6'h38: /* blbc */
	    begin
	       uop.op = BLBC;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h39: /* beq */
	    begin
	       uop.op = BEQZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3a: /* blt */
	    begin
	       uop.op = BLTZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3b: /* ble */
	    begin
	       uop.op = BLEZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3c: /* blbs */
	    begin
	       uop.op = BLBS;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3d: /* bne */
	    begin
	       uop.op = BNEZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3e: /* bge */
	    begin
	       uop.op = BGEZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  6'h3f: /* bgt */
	    begin
	       uop.op = BGTZ;
	       uop.srcA = ra;
	       uop.srcA_valid = 1'b1;
	       uop.rvimm = w_pc_imm;
	       uop.is_int = 1'b1;
	       uop.is_cheap_int = 1'b1;
	       uop.is_br = 1'b1;
	       uop.br_pred = insn_pred;
	    end
	  default:
	    begin
	    end
	endcase // case (opc)
     end // always_comb

endmodule // decode_alpha
