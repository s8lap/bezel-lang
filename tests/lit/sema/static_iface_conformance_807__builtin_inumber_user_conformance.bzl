// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, builtin_inumber_user_conformance).
//
// A user class conforming to the stdlib INumber<T> is accepted (the §25.2
// comparison-group rule requires the class to complete `< > <= >=`).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; public class Meters: INumber<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): Meters => new Meters(0); public static method One(): Meters => new Meters(1); public static method operator +(a: Meters, b: Meters): Meters => new Meters(a.Value + b.Value); public static method operator -(a: Meters, b: Meters): Meters => new Meters(a.Value - b.Value); public static method operator *(a: Meters, b: Meters): Meters => new Meters(a.Value * b.Value); public static method operator /(a: Meters, b: Meters): Meters => new Meters(a.Value / b.Value); public static method operator <(a: Meters, b: Meters): bool => a.Value < b.Value; public static method operator >(a: Meters, b: Meters): bool => a.Value > b.Value; public static method operator <=(a: Meters, b: Meters): bool => a.Value <= b.Value; public static method operator >=(a: Meters, b: Meters): bool => a.Value >= b.Value; end Meters;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static factory method `Zero` returns `Meters` with no `owned` qualifier and no structural borrow source; the caller treats the result as owned
// CHECK-SAME: (§28.3 sourceless-return rule)
