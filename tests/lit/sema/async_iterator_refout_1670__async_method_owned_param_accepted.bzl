// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, async_method_owned_param_accepted).
//
// `owned` parameters are explicitly exempted by both §30.1 and §29.3 — an
// `async` method taking an `owned` parameter is unaffected. // per §30.1, §29.8
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Res destructor end destructor; end Res; class Foo public async method Take(owned r: Res): Task using var _ := owned r; return; end Take; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
