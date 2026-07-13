// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example2_explicit_cast_disambiguates).
//
// §22.6 Example 2 — an explicit cast disambiguates (long(a) -> Convert(long)).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Converter public method Convert(n: long): int => 1; public method Convert(n: float): int => 2; public method Use(a: int) _ := this.Convert(long(a)); end Use; end Converter;

// NODIAG-NOT: error
// NODIAG-NOT: §
