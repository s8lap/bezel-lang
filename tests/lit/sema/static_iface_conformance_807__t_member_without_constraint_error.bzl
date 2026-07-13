// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, t_member_without_constraint_error).
//
// Without ANY constraint, `T.Member` stays an error (naming §11.8).

module M; static class F public static method Z<T>(): int return T.Ignore(); end Z; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` has no static member `Ignore` — `T.Member(...)` requires a `where T: <interface>` constraint whose interface declares that static member
// CHECK-SAME: (§11.8)
