// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, class_owned_field_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C private owned _t: T := new T(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
