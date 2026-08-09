#!/bin/bash
# csmith diff-testing : alpha_iss vs qemu-alpha (reference model).
# usage: ./csmith_diff.sh [count] [first_seed]
CSMITH_INC=/usr/include/csmith
N=${1:-50}
SEED0=${2:-1}
DIR=csmith_alpha
mkdir -p $DIR
pass=0
fail=0
skip=0
iss_timeout=0
for ((i=0; i<N; i++)); do
    seed=$((SEED0+i))
    csmith --seed $seed -o $DIR/t.c 2>/dev/null
    if ! alpha-linux-gnu-gcc-10 -mcpu=ev4 -mbwx -O2 -static -I$CSMITH_INC -w \
	 $DIR/t.c -o $DIR/t 2>/dev/null; then
	skip=$((skip+1))
	continue
    fi
    qout=$(timeout 10 qemu-alpha $DIR/t 2>/dev/null)
    qrc=$?
    if [ $qrc -ne 0 ]; then
	# hangs or crashes under the reference too - not our problem
	skip=$((skip+1))
	continue
    fi
    iout=$(timeout 120 ./alpha_iss -f $DIR/t 2>/dev/null)
    irc=$?
    if [ $irc -eq 124 ]; then
	iss_timeout=$((iss_timeout+1))
	echo "ISS TIMEOUT seed $seed"
	cp $DIR/t.c $DIR/timeout_$seed.c
    elif [ "$qout" == "$iout" ] && [ $irc -eq 0 ]; then
	pass=$((pass+1))
    else
	fail=$((fail+1))
	echo "MISMATCH seed $seed : qemu [$qout] iss rc=$irc [$iout]"
	cp $DIR/t.c $DIR/fail_$seed.c
    fi
done
echo "pass=$pass fail=$fail skip=$skip iss_timeout=$iss_timeout"
