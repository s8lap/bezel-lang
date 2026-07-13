// Ported from tests/sema/mixed_numeric_test.cpp
//   (BEZEL_TEST sema_mixed_numeric, int_plus_double_ok_spec_6_2).
//
// ---------------------------------------------------------------------------
// NO over-rejection: pairs that DO have a §5.1 implicit conversion in some
// direction must still type-check (they promote to the wider operand).
// These are the regression guards for #408's widening fix.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(a: int, b: double): int var r := a + b; return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
