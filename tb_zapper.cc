/* alpha_zapper unit test vs the ISS formulas */
#include <cstdio>
#include <cstdint>
#include "Vtb_zapper.h"
#include "verilated.h"

static uint64_t byte_mask(uint32_t m) {
  uint64_t r = 0;
  for(int i = 0; i < 8; i++) {
    if((m >> i) & 1) {
      r |= 0xffUL << (8*i);
    }
  }
  return r;
}

static uint64_t ref(int idx, uint64_t a, uint64_t b) {
  uint32_t bn = b & 7;
  uint32_t w = 0;
  switch(idx)
    {
    case 3: case 10: case 17: w = 0x01; break;
    case 4: case 7: case 11: case 14: case 18: case 21: w = 0x03; break;
    case 5: case 8: case 12: case 15: case 19: case 22: w = 0x0f; break;
    case 6: case 9: case 13: case 16: case 20: case 23: w = 0xff; break;
    }
  switch(idx)
    {
    case 0: return a & ~byte_mask(b & 0xff); /* zap */
    case 1: return a & byte_mask(b & 0xff); /* zapnot */
    case 2: { /* cmpbge */
      uint64_t r = 0;
      for(int i = 0; i < 8; i++) {
	uint8_t ba = a >> (8*i), bb = b >> (8*i);
	if(ba >= bb) {
	  r |= (1UL << i);
	}
      }
      return r;
    }
    case 3: case 4: case 5: case 6: /* extXl */
      return (a >> (8*bn)) & byte_mask(w);
    case 7: case 8: case 9: /* extXh */
      return (a << ((64 - 8*bn) & 63)) & byte_mask(w);
    case 10: case 11: case 12: case 13: /* insXl */
      return (a << (8*bn)) & byte_mask((w << bn) & 0xff);
    case 14: case 15: case 16: /* insXh */
      return (a >> ((64 - 8*bn) & 63)) & byte_mask((w << bn) >> 8);
    case 17: case 18: case 19: case 20: /* mskXl */
      return a & ~byte_mask((w << bn) & 0xff);
    case 21: case 22: case 23: /* mskXh */
      return a & ~byte_mask((w << bn) >> 8);
    }
  return 0;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Vtb_zapper *tb = new Vtb_zapper;
  uint64_t r = 0x123456789abcdef1UL;
  uint64_t bad = 0, n = 0;
  for(int idx = 0; idx < 24; idx++) {
    for(int t = 0; t < 4096; t++) {
      r ^= r << 13; r ^= r >> 7; r ^= r << 17;
      uint64_t a = r;
      r ^= r << 13; r ^= r >> 7; r ^= r << 17;
      /* bias b so all byte-numbers and small masks get hit */
      uint64_t b = (t & 1) ? (r & 0xff) : r;
      tb->idx = idx;
      tb->a = a;
      tb->b = b;
      tb->eval();
      uint64_t e = ref(idx, a, b);
      n++;
      if(tb->y != e) {
	if(bad < 10) {
	  printf("idx %d a %016lx b %016lx : rtl %016lx expect %016lx\n",
		 idx, a, b, tb->y, e);
	}
	bad++;
      }
    }
  }
  printf("%lu vectors, %lu mismatches\n", n, bad);
  delete tb;
  return bad ? -1 : 0;
}
