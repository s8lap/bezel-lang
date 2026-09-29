// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, double_discard_is_diagnosed_not_silently_reconsumed).
//
// ===========================================================================
// CONTROL: re-discarding an already-Disposed binding is diagnosed the same
// way any other post-dispose read is (§28.2) — not a crash, not silently
// accepted as a fresh consume. (The gate is `state == Owned`; a second
// discard sees `Disposed` and falls through to the ordinary read check.)
// ===========================================================================
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int var x := new R(); _ := x; _ := x; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} use of `x` after a dominating explicit Dispose() call — use-after-dispose is undefined behaviour in unchecked mode and raises DisposedAccessException in checked mode
// CHECK-SAME: (§28.2)
