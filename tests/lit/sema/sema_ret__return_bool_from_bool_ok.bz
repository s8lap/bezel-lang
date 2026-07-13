// Ported from tests/sema/return_assignability_test.cpp
//   (BEZEL_TEST sema_ret, return_bool_from_bool_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(): bool return true; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
