// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, readonly_property_unqualified_write_rejected_1173).
//
// §23.1 (#1173): an UNQUALIFIED implicit-this self-write `X := 9` to a readonly
// auto-property inside a regular method is rejected exactly like `this.X := 9`.
// Previously the readonly guard only fired for a MemberAccessExpr target, so the
// unqualified NameExpr form slipped through (an ACCEPT_TRAP — `X` was mutated).

module M; class A public readonly property X: int := 5; public constructor() end constructor; public method Bad() X := 9; end Bad; public static method Main(): int return 0; end Main; end A;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly property `X` outside a field initializer or constructor
// CHECK-SAME: (§23.1)
