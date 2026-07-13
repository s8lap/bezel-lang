// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_enum_pattern_still_ok_1205).
//
// §A.7/§19.3: an enum-constant pattern is still valid after the #1205 fix. #1205.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; end Color; class C public static method M(c: Color): int return case c of Red => 1, Green => 2, Blue => 3 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
