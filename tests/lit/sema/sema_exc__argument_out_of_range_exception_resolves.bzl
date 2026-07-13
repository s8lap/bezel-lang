// Ported from tests/sema/builtin_exceptions_test.cpp
//   (BEZEL_TEST sema_exc, argument_out_of_range_exception_resolves).
//
// #1586 / stdlib §2.5/§4.4 (v0.183, #1396): ArgumentOutOfRangeException is a
// new builtin exception type, declared alongside DateTime's component
// validation and used by the negative-magnitude constructor guards.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _e: ArgumentOutOfRangeException; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
