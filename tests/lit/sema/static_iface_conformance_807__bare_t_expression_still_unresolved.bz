// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, bare_t_expression_still_unresolved).
//
// A bare `T` OUTSIDE a member-access target keeps today's unresolved error.

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; static class F public static method Z<T>(): int where T: INum<T> var x := T; return 0; end Z; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `T`
