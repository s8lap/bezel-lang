// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, recurrence_conformance_accepted).
//
// The self-referential conformance `class Meters: INum<Meters>` under the
// recurrence constraint `where T: INum<T>` resolves (no ordering false-reject).
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// static members are spelled `owned T` / `owned Meters` per the declaration-
// site static-return rule.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; public interface INum<T> where T: INum<T> static method Zero(): owned T; static operator +(a: T, b: T): owned T; end INum; public class Meters: INum<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): owned Meters => new Meters(0); public static method operator +(a: Meters, b: Meters): owned Meters => new Meters(a.Value + b.Value); end Meters;

// NO-ERROR-NOT: error:
