// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, missing_static_method_named).
//
// Missing static method → conformance error NAMING the member and §11.8.

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; public class Broken: INum<Broken> public static method operator +(a: Broken, b: Broken): Broken => a; end Broken;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Broken` does not implement static interface member `Zero` of `INum` — a type declaring conformance to an interface with static members must supply each of them as a `public static` member
// CHECK-SAME: (§11.8)
