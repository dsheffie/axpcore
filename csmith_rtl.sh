#!/bin/bash
# csmith co-sim on the alpha RTL.  three-way chain per seed :
#   host gcc x86 run        = checksum oracle
#   alpha_iss               = golden model (validates shim/div/codegen)
#   rv64_core (alpha co-sim)= device under test, checker on
# usage: ./csmith_rtl.sh [count] [first_seed]
N=${1:-20}
SEED0=${2:-1000}
DIR=csmith_alpha_rtl
ICNT_CAP=30000000
mkdir -p $DIR
AFLAGS="-mcpu=ev4 -mbwx -O2 -nostdlib -nostartfiles -static
	-Wl,-Ttext-segment=0x20000000 -fno-builtin -I shim
	-I /usr/include/csmith -w"
pass=0
fail=0
iss_fail=0
skip=0
for ((i=0; i<N; i++)); do
    seed=$((SEED0+i))
    csmith --seed $seed -o $DIR/t.c 2>/dev/null
    if ! gcc -O2 -I/usr/include/csmith -w $DIR/t.c -o $DIR/t_host 2>/dev/null; then
	skip=$((skip+1))
	continue
    fi
    href=$(timeout 5 ./$DIR/t_host)
    if [ $? -ne 0 ] || [ -z "$href" ]; then
	# hangs under the oracle too - not our problem
	skip=$((skip+1))
	continue
    fi
    if ! alpha-linux-gnu-gcc-10 $AFLAGS shim/alpha_start_htif.S shim/shim.c \
	 shim/alpha_div.S $DIR/t.c -o $DIR/t_axp 2>/dev/null; then
	skip=$((skip+1))
	continue
    fi
    iss=$(timeout 60 ./alpha_iss -f $DIR/t_axp -i true -m $ICNT_CAP 2>$DIR/iss_err.txt)
    if grep -q "hit maxicnt" $DIR/iss_err.txt; then
	skip=$((skip+1))
	continue
    fi
    if [ "$iss" != "$href" ]; then
	iss_fail=$((iss_fail+1))
	echo "ISS-vs-HOST seed $seed : host [$href] iss [$iss]"
	cp $DIR/t.c $DIR/issfail_$seed.c
	continue
    fi
    rtl_raw=$(timeout 600 ./rv64_core -f $DIR/t_axp 2>&1)
    rtl=$(echo "$rtl_raw" | grep "^checksum")
    memok=$(echo "$rtl_raw" | grep -c "checker mem equal rtl mem")
    bad=$(echo "$rtl_raw" | grep -c "mismatch")
    if [ "$rtl" == "$href" ] && [ "$memok" -ge 1 ] && [ "$bad" -eq 0 ]; then
	pass=$((pass+1))
    else
	fail=$((fail+1))
	echo "RTL FAIL seed $seed : host [$href] rtl [$rtl] memok=$memok mism=$bad"
	cp $DIR/t.c $DIR/rtlfail_$seed.c
	echo "$rtl_raw" | grep -m5 "mismatch" > $DIR/rtlfail_$seed.log
    fi
done
echo "pass=$pass fail=$fail iss_fail=$iss_fail skip=$skip"
