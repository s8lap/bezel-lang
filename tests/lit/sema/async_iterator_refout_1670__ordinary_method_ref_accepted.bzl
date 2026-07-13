// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, ordinary_method_ref_accepted).
//
// An ORDINARY (non-async, non-iterator) method with a `ref` parameter is
// unaffected — its by-reference argument is bounded by the ordinary
// synchronous call frame, no heap state machine involved. // per §30.1, §22.3
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public method Bump(ref x: int) x := x + 1; end Bump; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
