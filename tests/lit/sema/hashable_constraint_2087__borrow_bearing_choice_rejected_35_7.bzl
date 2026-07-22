// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hashable_constraint_2087_test.cpp
//   (BEZEL_TEST hashable_constraint_2087, method_call_borrow_bearing_choice_rejected_spec_35_7).
//
// §14.2 (v0.214) / §35.7: a borrow-bearing choice (a plain class payload,
// §35.4.2) is NOT value-like auto-equality-bearing → not equality-bearing (§3.6)
// → fails `where T: hashable`, with the component-path reason.

module M; class R public V: int := 0; destructor end destructor; end R; choice Handle Some(value: R), None; end Handle; static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<Handle>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Handle` does not satisfy the constraint `where T: hashable` (§14.2)
// CHECK-SAME: : choice `Handle` is borrow-bearing (§35.7), not value-like auto-equality-bearing
