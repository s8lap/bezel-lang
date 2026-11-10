#!/bin/bash
# Model sweeps for proposal 0212 (revision 2); outputs in out/. Each out/compare_*.txt is produced by
# `bzcore_0212.py compare`: the v0.355 baseline (proposal 0205's flips applied) against four candidates —
# the note's candidate A, 0212 as first drafted (f72926c68), 0212 revised after the adversarial critique, and the
# revised rules with an oracle mint that carries only what the constructor stores.
# Feature set p0212r = the note's `static` set + newk2 (two-argument mint) + erase/euse (owned erasure)
#                      + newko (`out owned` recipient) + wrap/wuse (an owning choice that owns a holder).
# --hidden-link: every R is built with its child linked back to it, which the checker cannot see (critic c03).
cd "$(dirname "$0")"
M="python3 -I bzcore_0212.py"
$M selftest > out/selftest.txt 2>&1
$M selftest0212 > out/selftest0212.txt 2>&1
run() { tag=$1; shift; $M compare --features p0212r --v0355 "$@" > out/compare_$tag.txt 2>&1; }
run v3l4             --vars 3 --len 4
run v3l4_kdtor       --vars 3 --len 4 --kdtor
run v3l4_qdtor       --vars 3 --len 4 --qdtor
run v3l4_qdtor_r40   --vars 3 --len 4 --qdtor --r40-teardown
run v3l4_kq          --vars 3 --len 4 --kdtor --qdtor
run v3l4_kq_hidden   --vars 3 --len 4 --kdtor --qdtor --hidden-link
run v2l4_if          --vars 2 --len 4 --if
run v3l4_if          --vars 3 --len 4 --if
run v3l5_kq          --vars 3 --len 5 --kdtor --qdtor
run v3l5_kq_hidden   --vars 3 --len 5 --kdtor --qdtor --hidden-link
run v3l4_block_kq    --vars 3 --len 4 --block --body 2 --kdtor --qdtor
run v3l5             --vars 3 --len 5
run v3l4_block       --vars 3 --len 4 --block --body 2
run v3l4_while       --vars 3 --len 4 --while --body 2
run v3l5_qdtor_r40   --vars 3 --len 5 --qdtor --r40-teardown
# Critique round 2, note 2 (proposal §4.7): the open refinement of the taken-out rule. Feature set p0212t = p0212r
# + kdup/kself (a member's owned result over the receiver's own source / over the receiver). `compare --reach` shows
# 0212 revised, the refinement in its closed form, and the refinement read off the declared type of an open class.
# 4 variables: the builder must be moved into a fresh binding for the three to differ. 5 statements at 4 variables
# was not run: it outgrew a 15 GiB machine.
reach() { tag=$1; shift; $M compare --reach --features p0212t --v0355 "$@" > out/reach_$tag.txt 2>&1; }
reach v3l4_kdtor     --vars 3 --len 4 --kdtor
reach v4l4           --vars 4 --len 4
reach v4l4_kdtor     --vars 4 --len 4 --kdtor
reach v4l4_kq        --vars 4 --len 4 --kdtor --qdtor
python3 -I takeout.py --qdtor > out/takeout_v3l4_qdtor.txt 2>&1
echo DONE > out/DONE
