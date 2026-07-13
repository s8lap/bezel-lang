// Ported from tests/sema/scoped_static_owned_return_1606_test.cpp
//   (BEZEL_TEST scoped_static_owned_return_1606, static_scoped_with_owned_return_accepted).
//
// §28.14 (v0.180) control: a `scoped` parameter on a static method WITH an
// `owned` return is admitted — form 2/3 bounding is resolvable at qualifying
// call sites.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Take(scoped f: () -> ()): owned C f(); return new C(); end Take; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
