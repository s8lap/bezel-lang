// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, static_member_ref_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static _seed: int := 10; public _a: int := _seed; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
