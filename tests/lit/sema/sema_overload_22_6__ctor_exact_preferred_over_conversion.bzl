// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, ctor_exact_preferred_over_conversion).
//
// §22.6 / §22.7 — the rule applies identically to constructor overload
// sets. Two same-arity ctors whose single int argument matches one exactly
// and the other (long) by conversion: `new T(5)` resolves to T(int)
// unambiguously (exact preferred over conversion), no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T public constructor(a: long) end constructor; public constructor(a: int) end constructor; end T; class C public method M() _ := new T(5); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
