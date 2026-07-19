// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, builtin_inumber_user_conformance).
//
// A user class conforming to the stdlib INumber<T> is accepted (the §25.2
// comparison-group rule requires the class to complete `< > <= >=`).
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// producer members are spelled `owned Meters` per the declaration-site static-
// return rule (the canonical §11.8/§2.19 spelling, which the stdlib INumber<T>
// interface also declares under `owned T`).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; public class Meters: INumber<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): owned Meters => new Meters(0); public static method One(): owned Meters => new Meters(1); public static method operator +(a: Meters, b: Meters): owned Meters => new Meters(a.Value + b.Value); public static method operator -(a: Meters, b: Meters): owned Meters => new Meters(a.Value - b.Value); public static method operator *(a: Meters, b: Meters): owned Meters => new Meters(a.Value * b.Value); public static method operator /(a: Meters, b: Meters): owned Meters => new Meters(a.Value / b.Value); public static method operator <(a: Meters, b: Meters): bool => a.Value < b.Value; public static method operator >(a: Meters, b: Meters): bool => a.Value > b.Value; public static method operator <=(a: Meters, b: Meters): bool => a.Value <= b.Value; public static method operator >=(a: Meters, b: Meters): bool => a.Value >= b.Value; end Meters;

// NO-ERROR-NOT: error:
