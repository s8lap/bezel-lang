// Ported from tests/sema/async_iterator_ref_out_1670_test.cpp
//   (BEZEL_TEST async_iterator_refout_1670, sync_lambda_ref_accepted).
//
// A SYNCHRONOUS (non-`async`) lambda with an explicit `ref` parameter is
// unaffected — §30.6's rejection is async-lambda-only; a plain lambda's
// parameters are bounded by the ordinary call, no state machine. // per §30.6
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public static method Run() var f: (ref int) -> () := lambda(ref x: int) => do x := x + 1; end; return; end Run; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
