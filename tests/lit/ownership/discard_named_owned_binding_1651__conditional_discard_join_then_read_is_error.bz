// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, conditional_discard_join_then_read_is_error).
//
// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #3: a CONDITIONAL discard — `x` discarded on one
// branch, left Owned on the other — followed by a read after the join is a
// compile ERROR (maybe-consumed, §28.3). Before the fix the discard branch
// left `x` Owned (identically to the not-taken branch), so there was no
// state disagreement at the join and the program wrongly compiled clean.
// ===========================================================================

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(cond: bool): int var x := new R(); if cond then _ := x; end; return x._h; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `x` would be dropped here without disposal
// CHECK-SAME: (§28.3 — consider `using var`, explicit `.Dispose()
