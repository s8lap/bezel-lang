// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, plain_local_read_after_discard_draws_28_2_warning).
//
// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #2: a read of `x` AFTER the discard must draw the
// §28.2 use-after-dispose diagnostic — proving `x` was left
// explicitly-disposed by the discard, not merely borrow-read. Before the fix
// `x` stayed Owned across the discard, so this read drew NO diagnostic at all
// (a false "still valid" acceptance).
//
// Re-expected by #7435 (§28.2, v0.357): "A use of a local variable ... after an
// explicit `Dispose()` or direct `DisposeAsync()` call — or a discard of the
// binding (§17.13) — on the same variable that dominates the use site on every
// path is a **compile error**". The diagnostic was a warning (the file name
// keeps the ported test's name); it is now the error, and the program is
// rejected.
// ===========================================================================
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int var x := new R(); _ := x; return x._h; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: use of `x` after a dominating explicit Dispose() call — the disposal reaches this use on every path
// CHECK-SAME: a use the explicit disposal dominates is §28.2's use-after-dispose compile error
// CHECK-NOT: warning: use of `x`
