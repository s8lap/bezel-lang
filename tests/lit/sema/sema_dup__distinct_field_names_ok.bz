// Ported from tests/sema/duplicate_member_test.cpp
//   (BEZEL_TEST sema_dup, distinct_field_names_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: int; public _y: int; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
