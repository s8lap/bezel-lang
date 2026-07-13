// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_int_over_nullable_int_ok_spec_19_5_1203).
//
// §19.5 (#1203): `var n: int` over an `int?` (nullable) scrutinee is VALID —
// this is the nullable value-type unbox path (§19.5 / #784) and must not
// be rejected by the new check (the scrutinee is NULLABLE, not a plain
// value-type primitive).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(x: int?): int return case x of null => 0, var n: int => n end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
