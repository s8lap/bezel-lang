// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, record_extends_abstract_class_diagnoses).
//
// (a) §9.6: record deriving from an abstract class → compile error.

module M; abstract class Base public abstract method Compute(): int; end Base; record R(Val: int) : Base public override method Compute(): int => Val; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` cannot derive from class `Base`
// CHECK-SAME: (§9.6)
