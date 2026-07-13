// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, plain_async_and_iterator_no_refout_accepted).
//
// Sanity: an `async` method / iterator method with NO `ref`/`out` parameter
// is unaffected by the rule (no false positive on an ordinary plain
// parameter). // per §30.1, §29.3
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public async method Bump(x: int): Task<int> return x + 1; end Bump; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
