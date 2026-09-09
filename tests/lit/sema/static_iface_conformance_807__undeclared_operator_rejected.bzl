// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, undeclared_operator_rejected).
//
// An operator the constraint does NOT declare stays rejected (`-` on an
// interface declaring only `+`).
//
// fix #3930 (§25.2/§11.8, v0.295): the diagnostic used to name the operand
// type as the literal placeholder `?` instead of the type parameter `T`.
// Updated to the new, correct wording (mirrors the sibling ordering-
// operator diagnostic) — see the sibling
// unconstrained_t_plus_t_rejected.bzl fixture for the full rationale.

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; public class Meters: INum<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): Meters => new Meters(0); public static method operator +(a: Meters, b: Meters): Meters => new Meters(a.Value + b.Value); end Meters; static class F public static method Sub2<T>(a: T, b: T): T where T: INum<T> return a - b; end Sub2; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `-` is not available on type parameter `T`
// CHECK-SAME: add a `where T: INumber<T>` bound
// CHECK-SAME: (§25.2/§11.8)
