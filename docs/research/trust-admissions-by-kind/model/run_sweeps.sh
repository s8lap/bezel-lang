#!/bin/bash
# model sweeps for the trust-shape study; outputs in out/
cd "$(dirname "$0")"
M="python3 -I bzcore_trust.py"
for B in "--features trust --vars 3 --len 4" "--features trust --vars 3 --len 5" "--features trust --vars 2 --len 4 --if"; do
  tag=$(echo "$B" | tr -d ' -' )
  $M enumerate $B > out/enum_$tag.txt 2>&1
  $M check --mutation "derived retention" $B > out/check_derived_$tag.txt 2>&1
  $M check --mutation "owner-outlives" $B > out/check_owner_$tag.txt 2>&1
done
$M selftest > out/selftest.txt 2>&1
echo DONE > out/DONE
