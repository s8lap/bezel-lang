// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/move_after_move_field_897_898_test.cpp
//   (BEZEL_TEST move_after_move_field_897_898, double_move_qualified_rejected).
//
// §28.8.1 / #897: straight-line double-move of a QUALIFIED owned field. The
// second `move this._res` operates on an already-moved-from field — there is no
// ownership to transfer; it double-owns the original object. Must be REJECTED.
// (Before the fix this compiled and crashed at runtime with a use-after-free.)

module M; class Resource public constructor() end constructor; destructor end destructor; end Resource; class Owner private owned _res: Resource; public constructor() _res := new Resource(); end constructor; public method TakeTwice(): int using var a := move this._res; using var b := move this._res; this._res := new Resource(); return 0; end TakeTwice; destructor end destructor; end Owner; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned field `_res` is already moved-from at this point; cannot move it again — restore it (`this._res := ...`) before moving again
// CHECK-SAME: (§28.8.1)
