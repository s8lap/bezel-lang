// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, instance_field_is_not_null_guard_rejected).
//
// §7.4 (#862): `is not null` desugars to `<> null` (per §7.4 / §12.7); the
// field-narrowing rejection must fire regardless of which null-guard spelling is
// used.

module M; class Container public _name: string?; public constructor(n: string) _name := n; end constructor; destructor end destructor; end Container; class C public static method Main(): int using var c := new Container("hello"); if c._name is not null then return c._name.Length; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
