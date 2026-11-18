// Ported from tests/ownership/discard_named_owned_binding_1651_test.cpp
//   (BEZEL_TEST discard_named_owned_binding_1651, double_discard_is_diagnosed_not_silently_reconsumed).
//
// ===========================================================================
// CONTROL: re-discarding an already-Disposed binding is diagnosed (§28.2) —
// not a crash, not silently accepted as a fresh consume.
//
// Re-expected by #7435 (§28.2, v0.357): "One use is exempt: a second
// `Dispose()` or `DisposeAsync()`, or a second discard, of the same binding is
// the runtime no-op the carve-out above defines and draws a compile-time
// warning". The second discard was walked as a READ and drew the *use of `x`
// after a dominating explicit Dispose()* text; that text is now the compile
// error, and the exempt repeat draws the *disposed again* warning a second
// `Dispose()` draws.
// ===========================================================================
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int var x := new R(); _ := x; _ := x; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} local `x` is disposed again here — the discard of an already explicitly disposed binding is a no-op
// CHECK-SAME: (disposal is idempotent, §28.2)
// CHECK-NOT: use of `x` after a dominating explicit Dispose() call
