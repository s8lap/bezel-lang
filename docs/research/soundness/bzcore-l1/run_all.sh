#!/usr/bin/env bash
# The recorded runs behind README.md. Each line writes one file under out/. Single-threaded Python;
# run two lanes at a time on a 4-core box (see the end of the file).
set -u
cd "$(dirname "$0")"
mkdir -p out
B="python3 -I bzcore.py"

lane_a() {
  $B selftest                                                                 > out/selftest.txt 2>&1
  $B enumerate --features locals --vars 3 --len 5                            > out/enum_locals_v3l5.txt 2>&1
  $B enumerate --features locals --vars 3 --len 4 --if                       > out/enum_locals_v3l4_if.txt 2>&1
  $B enumerate --vars 3 --len 4                                              > out/enum_all_v3l4.txt 2>&1
  $B enumerate --vars 2 --len 3 --try --while                                > out/enum_all_v2l3_try_while.txt 2>&1
  $B mutate --features locals --vars 3 --len 4 --if                          > out/mutate_locals_v3l4_if.txt 2>&1
  $B mutate --features locals --vars 3 --len 5                               > out/mutate_locals_v3l5.txt 2>&1
  $B mutate --vars 3 --len 4                                                 > out/mutate_all_v3l4.txt 2>&1
  $B retention --vars 3 --len 4                                              > out/retention_all_v3l4.txt 2>&1
  $B retention --vars 2 --len 3 --if --try --while                           > out/retention_all_v2l3_if_try_while.txt 2>&1
  $B retention --features locals,fields --vars 3 --len 4 --if                > out/retention_fields_v3l4_if.txt 2>&1
  for m in "K5.3-a" "K5.3-b: dominated" "K5.3-b (borrow" "K5.3-b (same" "K5.3-c"; do
    f=$(echo "$m" | tr -c 'A-Za-z0-9' '_' | sed 's/_*$//')
    $B check --mutation "$m" --vars 3 --len 4                                > out/check_${f}_all_v3l4.txt 2>&1
    $B check --mutation "$m" --vars 2 --len 3 --if --try --while             > out/check_${f}_all_v2l3_if_try_while.txt 2>&1
  done
  # stability across bounds (K5.3-b)
  for bnd in "--vars 2 --len 3" "--vars 2 --len 4" "--vars 3 --len 3" "--vars 2 --len 4 --if" "--vars 3 --len 3 --if"; do
    f=$(echo "$bnd" | tr -c 'A-Za-z0-9' '_' | sed 's/_*$//')
    $B check --mutation "K5.3-b: dominated" $bnd                              > out/stab_K5_3_b${f}.txt 2>&1
  done
  # the fields fragment at five statements (the slot-displaced alias and the K4.3 residual need a, b, q)
  $B enumerate --features locals,fields --vars 3 --len 5                     > out/enum_fields_v3l5.txt 2>&1
  $B retention --features locals,fields --vars 2 --len 5                     > out/retention_fields_v2l5.txt 2>&1
  $B retention --features locals,fields --vars 3 --len 5                     > out/retention_fields_v3l5.txt 2>&1
  python3 -I examples/probes.py                                              > out/probes.txt 2>&1
  echo done > out/lane_a.done
}

lane_b() {
  $B enumerate --vars 3 --len 4 --if                                         > out/enum_all_v3l4_if.txt 2>&1
  $B mutate --vars 3 --len 4 --if                                            > out/mutate_all_v3l4_if.txt 2>&1
  $B mutate --vars 2 --len 3 --try --while                                   > out/mutate_all_v2l3_try_while.txt 2>&1
  for m in "K5.3-a" "K5.3-b: dominated" "K5.3-b (borrow" "K5.3-b (same" "K5.3-c"; do
    f=$(echo "$m" | tr -c 'A-Za-z0-9' '_' | sed 's/_*$//')
    $B check --mutation "$m" --vars 3 --len 4 --if                           > out/check_${f}_all_v3l4_if.txt 2>&1
  done
  $B retention --vars 3 --len 4 --if                                         > out/retention_all_v3l4_if.txt 2>&1
  $B check --define examples/mutation_k52_unfrozen_when_dead.py --vars 3 --len 4 --if > out/check_define_example.txt 2>&1
  echo done > out/lane_b.done
}

case "${1:-both}" in
  a) lane_a ;;
  b) lane_b ;;
  both) lane_a & lane_b & wait ;;
esac
