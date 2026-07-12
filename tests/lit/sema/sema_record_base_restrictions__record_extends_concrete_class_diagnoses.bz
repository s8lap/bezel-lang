// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_extends_concrete_class_diagnoses).
//
// (b) §9.6: record deriving from a concrete (non-abstract) class → compile error.

module M; class Base public method Greet(): int => 42; end Base; record R(X: int) : Base end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` cannot derive from class `Base`
// CHECK-SAME: (§9.6)
