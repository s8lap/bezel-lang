#!/bin/bash
cd "$(dirname "$0")"
M="python3 -I bzcore_trust.py"
for B in "--features trust --vars 3 --len 4" "--features trust --vars 3 --len 5" "--features trust --vars 2 --len 4 --if"; do
  tag=$(echo "$B" | tr -d ' -' )
  $M check --mutation "owner-outlives, sharp" $B > out/check_ownersharp_$tag.txt 2>&1
  $M check --mutation "both:" $B > out/check_both_$tag.txt 2>&1
done
echo DONE > out/DONE2
