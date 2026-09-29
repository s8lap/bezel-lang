// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, plain_local_read_after_discard_draws_28_2_warning).
//
// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #2: a read of `x` AFTER the discard must draw the
// §28.2 use-after-dispose warning (must-level per v0.184, straight-line
// dominated) — proving `x` was left explicitly-disposed by the discard, not
// merely borrow-read. Before the fix `x` stayed Owned across the discard, so
// this read drew NO diagnostic at all (a false "still valid" acceptance).
// Not a hard error: §28.2 downgrades a statically-provable, dominating
// use-after-dispose to a warning (the checked-disposal runtime is the error
// path).
// ===========================================================================
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int var x := new R(); _ := x; return x._h; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} use of `x` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode
// CHECK-SAME: (§28.2)
