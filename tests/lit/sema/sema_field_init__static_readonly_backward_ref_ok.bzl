// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, static_readonly_backward_ref_ok).
//
// §22.13: a BACKWARD reference (the read field is declared earlier) is fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static readonly B: int := 5; public static readonly A: int := B + 1; public static method Main(): int return A; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
