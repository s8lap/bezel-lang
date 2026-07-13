// Ported from tests/sema/scoped_static_owned_return_1606_test.cpp
//   (BEZEL_TEST scoped_static_owned_return_1606, instance_scoped_no_owned_return_accepted).
//
// §28.14 (v0.180) control: a NON-static (instance) method with a `scoped`
// parameter and no `owned` return is unaffected — form 5 resolves the
// receiver as the bounding object regardless of return type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method Take(scoped f: () -> ()) f(); end Take; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
