// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, constraint_operator_accepted).
//
// `a + b` on T-typed operands resolves through the constraint's operator.
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// static members are spelled `owned T` / `owned Meters` per the declaration-
// site static-return rule. `Add2`'s body returns a parameter-sourced borrow of
// its `+`-operator result chain (a binary expression on bare-T operands has a
// null-declaration Named result type, so producerState falls through to
// Borrowed at the generic body level — admitted by the bare-T body-side
// check), keeping the bare-T signature admissible.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; public interface INum<T> where T: INum<T> static method Zero(): owned T; static operator +(a: T, b: T): owned T; end INum; public class Meters: INum<Meters> public Value: int; public constructor(v: int) this.Value := v; end; public static method Zero(): owned Meters => new Meters(0); public static method operator +(a: Meters, b: Meters): owned Meters => new Meters(a.Value + b.Value); end Meters; static class F public static method Add2<T>(a: T, b: T): T where T: INum<T> return a + b; end Add2; end F;

// NO-ERROR-NOT: error:
