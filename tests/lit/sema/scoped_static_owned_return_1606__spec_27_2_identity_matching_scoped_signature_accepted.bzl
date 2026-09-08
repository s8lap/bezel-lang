// Ported from tests/sema/scoped_static_owned_return_1606_test.cpp
//   (BEZEL_TEST scoped_static_owned_return_1606,
//   spec_27_2_identity_still_works_with_owned_return — first of two
//   independent parseAndCheckOwnership() assertions in that BEZEL_TEST body,
//   split into its own .bzl per the extractor's manual-split convention).
//
// §27.2 / §28.14 interaction (the reason this fix was deferred from #1541):
// a static method with a `scoped` parameter and an `owned` return still
// participates correctly in §27.1/§27.2 function-signature identity — the
// matching scoped-position method reference is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C   public static method Take(scoped f: () -> ()): owned C     f();     return new C();   end Take;   public static method Main(): int     var g: (scoped () -> ()) -> owned C := C.Take;     return 0;   end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
