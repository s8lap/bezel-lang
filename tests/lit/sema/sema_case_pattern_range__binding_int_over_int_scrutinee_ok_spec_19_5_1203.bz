// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_int_over_int_scrutinee_ok_spec_19_5_1203).
//
// §19.5 (#1203): `var n: int` over `int` scrutinee is VALID (same type) and
// must not be rejected by the new check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: int): int return case x of var n: int => n, _ => 0 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
