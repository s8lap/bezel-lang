// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, ctor_defaulted_arity_no_defaults_preferred).
//
// §22.6 / §22.7 — the defaulted-arity tie-break applies identically to
// constructor overload sets. `new T(5)` resolves to the no-defaults
// `T(int)` constructor rather than being reported ambiguous against
// `T(int, int := 0)`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T public constructor(a: int, b: int := 0) end constructor; public constructor(a: int) end constructor; end T; class C public method M() _ := new T(5); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
