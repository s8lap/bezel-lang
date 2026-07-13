// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, static_readonly_cross_type_ref_ok).
//
// §22.13: the rule is per-type — reading a DIFFERENT type's static readonly
// field is not a same-type forward reference (cross-type ordering is governed
// by the cycle rule, not this one).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class D public static readonly Q: int := 10; end D; class C public static readonly X: int := D.Q + 1; public static method Main(): int return X; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
