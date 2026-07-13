// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, constraint_operator_accepted).
//
// `a + b` on T-typed operands resolves through the constraint's operator.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; public interface INum<T> where T: INum<T> static method Zero(): T; static operator +(a: T, b: T): T; end INum; public class Meters: INum<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): Meters => new Meters(0); public static method operator +(a: Meters, b: Meters): Meters => new Meters(a.Value + b.Value); end Meters; static class F public static method Add2<T>(a: T, b: T): T where T: INum<T> return a + b; end Add2; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static factory method `Zero` returns `Meters` with no `owned` qualifier and no structural borrow source; the caller treats the result as owned
// CHECK-SAME: (§28.3 sourceless-return rule)
