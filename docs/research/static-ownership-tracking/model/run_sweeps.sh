#!/bin/bash
# Model sweeps for the static-ownership-tracking study; outputs in out/. Each line of out/*.txt is produced by
# `bzcore_static.py compare`: the v0.355 baseline (proposal 0205's flips applied) against candidates A, A-static, B, D, A+D, E.
cd "$(dirname "$0")"
M="python3 -I bzcore_static.py"
$M selftest > out/selftest.txt 2>&1
run() { tag=$1; shift; $M compare --features static --v0355 "$@" > out/compare_$tag.txt 2>&1; }
run v3l4            --vars 3 --len 4
run v3l4_kdtor      --vars 3 --len 4 --kdtor
run v2l4_if         --vars 2 --len 4 --if
run v2l4_if_kdtor   --vars 2 --len 4 --if --kdtor
run v3l4_block      --vars 3 --len 4 --block --body 2
run v3l4_block_kdtor --vars 3 --len 4 --block --body 2 --kdtor
run v3l4_while      --vars 3 --len 4 --while --body 2
run v3l4_if         --vars 3 --len 4 --if
run v3l5            --vars 3 --len 5
run v3l5_kdtor      --vars 3 --len 5 --kdtor
echo DONE > out/DONE
