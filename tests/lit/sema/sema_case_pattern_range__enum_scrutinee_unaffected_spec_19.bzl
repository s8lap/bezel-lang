// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, enum_scrutinee_unaffected_spec_19).
//
// An enum scrutinee with bare-constant patterns must NOT trip the integer
// range gate (the gate applies only to a primitive integer scrutinee). #1153.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; end Color; class C public static method M(c: Color): int return (case c of Red => 1, Green => 2, Blue => 3); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
