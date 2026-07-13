// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, t_member_instance_member_error).
//
// `T.Member` on an INSTANCE interface member → error (static members only).

module M; public interface IS<T> where T: IS<T> method Scale(k: int): int; end IS; static class F public static method Z<T>(): int where T: IS<T> return T.Scale(2); end Z; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `T.Scale` requires a STATIC interface member — `Scale` is an instance member of the constraint interface; call it on a value of type `T` instead
// CHECK-SAME: (§11.8)
