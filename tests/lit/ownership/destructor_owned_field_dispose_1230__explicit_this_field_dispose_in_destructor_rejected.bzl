// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/destructor_owned_field_dispose_1230_test.cpp
//   (BEZEL_TEST destructor_owned_field_dispose_1230, explicit_this_field_dispose_in_destructor_rejected).
//
// §28.4 / §28.5 (#1230): explicit-`this` form `this._sub.Dispose()` in a
// destructor — same double-free hazard, same diagnostic. REJECTED.

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Widget private owned _sub: Inner; public constructor() this._sub := new Inner(); end constructor; destructor this._sub.Dispose(); end destructor; public static method Main(): int return 0; end Main; end Widget;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: explicit `.Dispose()` of an owned field inside a destructor double-frees: the compiler-generated Dispose() runs the destructor body and then auto-disposes all owned fields
// CHECK-SAME: (§28.4)
