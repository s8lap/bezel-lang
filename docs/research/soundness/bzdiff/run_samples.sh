#!/usr/bin/env bash
# Stratified sample sweep (one sweep at a time; 3 compiles in parallel inside each).
set -u
cd "$(dirname "$0")"
P="python3 -I bzdiff.py sweep --jobs 3"
$P --features locals,pins       --vars 3 --len 4 --sample 3000 --out runs/pins          --label pins-v3l4
$P --features locals,temps      --vars 3 --len 4 --sample 3000 --out runs/temps         --label temps-v3l4
$P --features locals,pins,temps --vars 2 --len 4 --if --sample 3000 --out runs/pins_if  --label pins-temps-v2l4-if
$P --features locals,fields     --vars 3 --len 4 --no-fmove --sample 3000 --out runs/fields --label fields-v3l4
$P --features locals,asgm,pins,temps,throw --vars 2 --len 3 --try --while --sample 3000 --out runs/trywhile --label all-nofields-v2l3-try-while
$P --features all --vars 3 --len 4 --no-fmove --sample 3000 --out runs/all --label all-v3l4
echo DONE > runs/samples.done
