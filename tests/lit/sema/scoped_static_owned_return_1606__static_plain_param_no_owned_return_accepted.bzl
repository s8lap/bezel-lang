// Ported from tests/sema/scoped_static_owned_return_1606_test.cpp
//   (BEZEL_TEST scoped_static_owned_return_1606, static_plain_param_no_owned_return_accepted).
//
// §28.14 (v0.180) control: a static method with NO `scoped` parameter at all
// is unaffected by this rule regardless of its return type (the plain
// call-bounded default of §28.14).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Take(f: () -> ()) f(); end Take; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
