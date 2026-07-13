// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, subtype_exact_preferred_over_supertype_convert).
//
// §22.6 — a subtype argument is an exact match against its own type and a
// (ranked-lower) conversion against a supertype parameter. F(Derived) is
// placed second; for a Derived argument it is exact while F(Base) converts,
// so F(Derived) is unambiguously better. It returns int; F(Base) returns
// bool — binding to `var x: int` confirms F(Derived) was chosen.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base end Base; class Derived : Base end Derived; class C public method F(b: Base): bool => true; public method F(d: Derived): int => 1; public method Use(d: Derived) var x: int := this.F(d); end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
