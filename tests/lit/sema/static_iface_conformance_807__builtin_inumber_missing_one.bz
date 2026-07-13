// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, builtin_inumber_missing_one).
//
// A user class declaring `: INumber<Self>` but missing `One` is rejected.

module M; public class Meters: INumber<Meters> public static method Zero(): Meters => new Meters(); end Meters;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Meters` does not implement static interface member `One` of `INumber` — a type declaring conformance to an interface with static members must supply each of them as a `public static` member
// CHECK-SAME: (§11.8)
