// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, instance_method_does_not_satisfy).
//
// An INSTANCE method of the right name does not satisfy a static member.

module M; public interface IZ<T> where T: IZ<T> static method Zero(): T; end IZ; public class W: IZ<W> public method Zero(): W => this; end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `W` does not implement static interface member `Zero` of `IZ` — a type declaring conformance to an interface with static members must supply each of them as a `public static` member
// CHECK-SAME: (§11.8)
