// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_extends_static_class_diagnoses).
//
// (g) §9.6: static class as base of a record → compile error.

module M; static class Util public static method Id(x: int): int => x; end Util; record R(X: int) : Util end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` cannot derive from class `Util`
// CHECK-SAME: (§9.6)
