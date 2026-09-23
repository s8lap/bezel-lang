// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, shared_documented_surface_allowed_2_16).
//
// (b) Documented Shared<T> surface still compiles without error.
//
// ACCEPT, NO WARNING control: bezelc -c must exit zero (the program is legal) and the
// bezelc stderr must be EMPTY of the §28.3 owned-parameter must-use warning.
// `Shared<T>` is runtime-managed (ref-count handed by the runtime primitive
// `bz_shared_release`, not by a Bezel-owned chain-family arm); §28.9 +
// Appendix C.3 item 14 (v0.307) classifies a runtime-managed reference-count
// wrapper as TRIVIAL-DISPOSE for the §28.3 must-use lint — the warning
// correctly stays silent at the implicit-drop site of an undischarged
// `owned s: Shared<Foo>` parameter. CHECK-NOT pins the silence: a regression
// that RE-EMITS the warning at this site is a §28.9 violation.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo end Foo; class C public static method Test(owned s: Shared<Foo>) using var s2 := s.Clone(); var _f := s2.Get(); using var w := s2.Downgrade(); end Test; end C;

// NODIAG-NOT: owned parameter `s` is implicitly disposed
// NODIAG-NOT: is implicitly disposed at the end of the method
