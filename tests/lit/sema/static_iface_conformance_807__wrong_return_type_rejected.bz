// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, wrong_return_type_rejected).
//
// Wrong signature (return type differs after substitution) → rejected.

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; public class Broken: INum<Broken> public static method Zero(): int => 0; public static method operator +(a: Broken, b: Broken): Broken => a; end Broken;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Broken` implements static interface member `Zero` of `INum` with a different signature — the implementing static must match the interface member exactly: same parameter count, parameter types and modifiers, and return type including `owned`
// CHECK-SAME: (§11.2 / §11.8)
