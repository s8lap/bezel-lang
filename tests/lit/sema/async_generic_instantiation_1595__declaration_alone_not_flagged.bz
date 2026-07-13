// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, declaration_alone_not_flagged).
//
// Sanity: the generic method DECLARATION alone (never called) reports
// neither instantiation-site diagnostic — the check is call-site-triggered,
// not a latent declaration-time false positive. (The `kMainTrailer` module
// unit must still compile as a whole for the harness to run to this pass.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
