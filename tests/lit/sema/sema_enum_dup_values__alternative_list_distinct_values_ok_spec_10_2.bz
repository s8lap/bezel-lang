// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, alternative_list_distinct_values_ok_spec_10_2).
//
// A `|` alternative list over DISTINCT values (§10.6-style grouping) is
// unaffected by the duplicate-value check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; end Color; class C public method M(c: Color): int return case c of Red | Green => 1, Blue => 2 end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
