// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, weak_tryupgrade_allowed_2_16).
//
// (b) Documented Weak<T> surface still compiles without error.
//
// ACCEPT, NO WARNING control: bezelc -c must exit zero (the program is legal) and the
// bezelc stderr must be EMPTY of the §28.3 owned-parameter must-use warning.
// `Weak<T>` is runtime-managed (reference handed by the runtime, no Bezel-
// owned chain-family arm); §28.9 + Appendix C.3 item 14 (v0.307) classifies a
// runtime-managed weak-reference wrapper as TRIVIAL-DISPOSE for the §28.3
// must-use lint — the warning correctly stays silent at the implicit-drop
// site of an undischarged `owned w: Weak<Foo>` parameter. CHECK-NOT pins the
// silence: a regression that RE-EMITS the warning at this site is a §28.9
// violation.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo end Foo; class C public static method Test(owned w: Weak<Foo>) var ok := w.TryUpgrade(out using var s); end Test; end C;

// NODIAG-NOT: owned parameter `w` is implicitly disposed
// NODIAG-NOT: is implicitly disposed at the end of the method
