// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/destructor_field_move_763_test.cpp
//   (BEZEL_TEST destructor_field_move_763, regular_method_field_not_restored_rejected).
//
// §28.8.1: a regular METHOD that moves an owned field out and does not restore
// it leaves the field's invariant broken on exit — STILL REJECTED (the
// destructor exemption must not leak into ordinary methods).
// Note: the destructor is left empty (§28.5 auto-disposes _inner); an explicit
// `_inner.Dispose()` in the destructor is a separate error caught by #1230.

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Outer private owned _inner: Inner; public constructor() this._inner := new Inner(); end constructor; public method Take(): () using var u := move this._inner; end Take; destructor end destructor; public static method Main(): int return 0; end Main; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_inner` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
