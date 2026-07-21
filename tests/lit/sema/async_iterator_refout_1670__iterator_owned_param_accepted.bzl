// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, iterator_owned_param_accepted).
//
// `owned` parameters on an iterator method are likewise unaffected. // per §29.3, §29.8
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Res destructor end destructor; end Res; class Foo public method Gen(owned r: Res): IEnumerable<int> using var _ := owned r; yield return 1; end Gen; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
