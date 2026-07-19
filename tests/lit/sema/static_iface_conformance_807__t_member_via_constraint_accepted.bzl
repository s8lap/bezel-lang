// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, t_member_via_constraint_accepted).
//
// `T.Zero()` under `where T: INum<T>` typechecks (the constraint provides it).
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// static members are spelled `owned T` / `owned Meters` per the declaration-
// site static-return rule. The generic helper `Z<T>(): owned T` is spelled
// `owned T` (empty source set) so the body's `T.Zero()` (a fresh value at
// class T) transfers through the return.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; public interface INum<T> where T: INum<T> static method Zero(): owned T; static operator +(a: T, b: T): owned T; end INum; public class Meters: INum<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): owned Meters => new Meters(0); public static method operator +(a: Meters, b: Meters): owned Meters => new Meters(a.Value + b.Value); end Meters; static class F public static method Z<T>(): owned T where T: INum<T> return T.Zero(); end Z; end F;

// NO-ERROR-NOT: error:
