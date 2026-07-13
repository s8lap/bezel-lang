// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, ordinary_method_out_accepted).
//
// An ordinary method with an `out` parameter is unaffected. // per §30.1, §22.3
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public method Fill(out x: int) x := 5; end Fill; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
