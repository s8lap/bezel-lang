// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, inherited_instance_member_ref_diagnoses_931).
//
// §22.8 (#931): a field initializer may not reference an INHERITED instance
// member either — `this` is not fully constructed when the initializer runs.
// Before the fix only same-class instance members were flagged.

module M; public class Base public _x: int; public constructor(x: int) _x := x; end constructor; destructor end destructor; end Base; public class Derived: Base public _doubled: int := _x * 2; public constructor(x: int) base(x); end constructor; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a field initializer may not reference instance member `_x`
// CHECK-SAME: (§22.8)
