#!/bin/bash
# run_decode_formal.sh -- formally prove the decoder never emits a valid
# integer destination of logical r31 (dst_valid |-> dst[4:0] != 31).  r31 is
# the alpha zero register : reset maps it to phys 0 forever and rf6r3w
# hardwires reads of ptr 0 to zero, so this decode invariant is what keeps
# the zero register zero.  Ported from r9999 (mips $0 flavor, which caught
# SSNOP and the mfc-to-$0 moves).
#
#   ./run_decode_formal.sh
#
# Uses sv2v + yosys `sat`.  sv2v drops immediate `assert`s, so the property
# is exposed as an output `bad` and proven UNSAT.
set -e
cd "$(dirname "$0")"
V=$(mktemp -d)/formal_decode.v
sv2v ../machine.vh ../rob.vh ../uop.vh ../mwidth_add.sv ../decode_alpha.sv formal_decode.sv > "$V" 2>/dev/null

echo "=== sanity: dst_valid must be reachable (flow non-vacuous) ==="
yosys -p "read_verilog -sv $V; hierarchy -check -top formal_decode; proc; flatten; opt -fast; sat -set dv 1" \
  2>&1 | grep -iE "model found|no model" | head -1

echo "=== proof: bad = dst_valid & (dst[4:0]==31) -- must be UNSAT (no model) ==="
if yosys -p "read_verilog -sv $V; hierarchy -check -top formal_decode; proc; flatten; opt -fast; sat -set bad 1" \
     2>&1 | grep -qi "no model found"; then
  echo "PASS: dst_valid |-> dst[4:0] != 31 holds across all 2^32 insns x all inputs"
  exit 0
else
  echo "FAIL: found an insn with dst_valid & dst==r31 (see the model above)"
  exit 1
fi
