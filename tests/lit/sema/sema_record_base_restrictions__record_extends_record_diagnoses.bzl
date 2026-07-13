// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_extends_record_diagnoses).
//
// (c) §9.6: record deriving from another record → compile error.

module M; record Base(X: int) end Base; record Derived(X: int, Y: int) : Base end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Derived` cannot derive from record `Base`
// CHECK-SAME: (§9.6)
