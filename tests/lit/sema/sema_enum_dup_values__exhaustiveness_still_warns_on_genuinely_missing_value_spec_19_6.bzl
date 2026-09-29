// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, exhaustiveness_still_warns_on_genuinely_missing_value_spec_19_6).
//
// A genuinely missing (uncovered) constant still warns — the value-based
// exhaustiveness rule only merges ALIASES of a covered value; it does not
// suppress the warning for an unrelated, truly-uncovered value.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; end Color; class C public method M(c: Color): int return (case c of Red => 1, Green => 2); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} non-exhaustive `case`: enum `Color` is missing constant(s): Blue (add a `_` arm or cover the remaining value(s))
// CHECK-SAME: (§19.6)
