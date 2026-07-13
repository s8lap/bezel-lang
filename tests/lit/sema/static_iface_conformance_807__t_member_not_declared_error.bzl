// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, t_member_not_declared_error).
//
// A constraint whose interface does NOT declare the member → error.

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; static class F public static method Z<T>(): T where T: INum<T> return T.Nope(); end Z; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` has no static member `Nope` — `T.Member(...)` requires a `where T: <interface>` constraint whose interface declares that static member
// CHECK-SAME: (§11.8)
