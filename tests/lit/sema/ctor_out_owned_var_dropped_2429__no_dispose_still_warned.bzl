// Ported from tests/codegen/ctor_out_owned_var_dropped_2429_test.cpp
//   (BEZEL_TEST ctor_out_owned_var_dropped_2429,
//    dropped_on_the_floor_disposes_exactly_once_spec_28_3).
//
// §28.3 / §22.4 / #2429 (constructor `out var` recipient dropped on
// the floor, spec v0.235): before this fix, no diagnostic at all was
// emitted for this program — `MethodOwnership::recordMovesIn`'s NewExpr
// branch never wired the recipient into the ownership dataflow's `states_`
// map, so it was invisible to the §28.3 affine auto-drop end-of-scope check
// that already emits the must-use warning for the equivalent method-call
// shape (compare out_owned_if_branch_1407__no_dispose_still_warned.bzl /
// out_owned_transfer_860__user_out_owned_param_dropped_rejected.bzl, both
// pre-existing). After this fix the constructor recipient gets the SAME
// warning.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal — §28.3's affine auto-drop reclaims the value implicitly at scope
// end), but it DOES emit the must-use warning — pinned below so the
// diagnostic itself stays observed and does not silently regress back to
// "no diagnostic at all" (the pre-fix, silently-leaking behavior).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Item end Item; class Widget public constructor(s: string, out owned n: Item) n := new Item(); end constructor; end Widget; class C public static method Main(): int var w := new Widget("x", out var n); w.Dispose(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `n` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
