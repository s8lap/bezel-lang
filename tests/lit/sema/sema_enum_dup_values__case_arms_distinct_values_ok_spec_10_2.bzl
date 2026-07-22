// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, case_arms_distinct_values_ok_spec_10_2).
//
// All three constants hold DISTINCT explicit values; the `case` names each
// exactly once and must compile with no diagnostic at all.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A := 1, B := 2, C := 3; end E; class D public method M(v: E): int return (case v of A => 1, B => 2, C => 3); end M; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
