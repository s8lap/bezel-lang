// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, exhaustiveness_arm_naming_one_alias_covers_both_spec_19_6).
//
// `Legacy` and `Current` share value 1; an arm naming only `Legacy` (plus one
// covering the distinct `Other := 2`) is exhaustive — "an arm naming either
// alias covers both" (§10.2 / §19.6). No non-exhaustiveness warning.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E Legacy := 1, Current := 1, Other := 2; end E; class D public method M(v: E): int return case v of Legacy => 1, Other => 2 end; end M; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
