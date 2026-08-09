/* decode_alpha unit test : run every text-section word of real alpha
 * binaries through the decoder and check classification against an
 * independent model of the opcode map.  II on any word a compiled
 * binary contains = decoder table bug. */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vtb_decode_alpha.h"
#include "verilated.h"

/* must match opcode_t : II is the last enumerator */
static const uint32_t OP_II_PROBE = 0xffffffff;

struct expect_t {
  bool is_mem;
  bool is_store;
  bool is_br;
};

static bool classify(uint32_t insn, expect_t *e) {
  uint32_t opc = insn >> 26;
  uint32_t ra = (insn >> 21) & 31;
  memset(e, 0, sizeof(*e));
  switch(opc)
    {
    case 0x0a: case 0x0c: case 0x28: case 0x29: /* plain loads */
      e->is_mem = (ra != 31);
      return true;
    case 0x0b: /* ldq_u : unop when ra=31 */
      e->is_mem = (ra != 31);
      return true;
    case 0x2a: case 0x2b: /* ldx_l always touches the lock flag */
      e->is_mem = true;
      return true;
    case 0x0d: case 0x0e: case 0x0f: case 0x2c: case 0x2d: /* stores */
      e->is_mem = true;
      e->is_store = true;
      return true;
    case 0x2e: case 0x2f: /* stx_c : mem but sd tracked via srcB */
      e->is_mem = true;
      return true;
    case 0x30: case 0x34: /* br/bsr */
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x3c: case 0x3d: case 0x3e: case 0x3f: /* cond branches */
      e->is_br = true;
      return true;
    case 0x1a: /* jmp group */
      e->is_br = true;
      return true;
    case 0x00: /* call_pal */
    case 0x08: case 0x09: /* lda/ldah */
    case 0x10: case 0x11: case 0x12: case 0x13: /* int operates */
    case 0x18: /* misc */
    case 0x1c: /* sextb/sextw/cix */
      return true;
    default:
      return false; /* unmodeled (fp etc) - decoder should say II */
    }
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  if(argc < 2) {
    fprintf(stderr, "usage: %s text.bin [text2.bin ...]\n", argv[0]);
    return 1;
  }
  Vtb_decode_alpha *tb = new Vtb_decode_alpha;

  /* find the II enum value by decoding a word from an unallocated
   * opcode (0x01) */
  tb->insn = (0x01 << 26);
  tb->pc = 0x1000;
  tb->eval();
  uint32_t op_ii = tb->op;

  uint64_t checked = 0, ii_cnt = 0, mismatch = 0;
  for(int f = 1; f < argc; f++) {
    FILE *fp = fopen(argv[f], "rb");
    if(fp == nullptr) {
      fprintf(stderr, "cannot open %s\n", argv[f]);
      return 1;
    }
    std::vector<uint32_t> words;
    uint32_t w;
    while(fread(&w, 4, 1, fp) == 1) {
      words.push_back(w);
    }
    fclose(fp);
    for(size_t i = 0; i < words.size(); i++) {
      uint32_t insn = words[i];
      tb->insn = insn;
      tb->pc = 0x120000000UL + 4*i;
      tb->eval();
      checked++;
      expect_t e;
      bool modeled = classify(insn, &e);
      if(tb->op == op_ii) {
	if(modeled) {
	  printf("II for modeled insn %08x (opcode %02x func %02x) at word %zu of %s\n",
		 insn, insn >> 26, (insn >> 5) & 0x7f, i, argv[f]);
	  ii_cnt++;
	}
	continue;
      }
      if(!modeled) {
	printf("decoder accepted unmodeled insn %08x (opcode %02x) in %s\n",
	       insn, insn >> 26, argv[f]);
	mismatch++;
	continue;
      }
      if(tb->is_mem != e.is_mem || tb->is_store != e.is_store || tb->is_br != e.is_br) {
	printf("class mismatch insn %08x (opcode %02x) : rtl mem/store/br=%d%d%d expect %d%d%d\n",
	       insn, insn >> 26, tb->is_mem, tb->is_store, tb->is_br,
	       e.is_mem, e.is_store, e.is_br);
	mismatch++;
      }
    }
  }
  printf("checked %lu words : %lu unexpected II, %lu class mismatches\n",
	 checked, ii_cnt, mismatch);
  delete tb;
  return (ii_cnt + mismatch) ? -1 : 0;
}
