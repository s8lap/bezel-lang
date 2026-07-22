// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/choice_recursive_equality_rule_2083_2087_test.cpp
//   (BEZEL_TEST choice_recursive_equality_rule_2083_2087,
//    spec_14_2_v214_function_payload_choice_fails_hashable_with_component_path).
//
// §14.2 (v0.214) / §35.7 (v0.210, #2083 × #2087 merged recursive rule): a
// VALUE-LIKE choice with a function-typed payload is outside the §9.2
// admissible-component recursion, so it is not auto-equality-bearing → not
// equality-bearing (§3.6) → fails `where T: hashable`. The component path
// names the offending case/payload (the pre-merge classification-only rule
// over-admitted this shape — the #2110 key-position half).

module M; choice Holder Fn(callback: (int) -> int), None; end Holder; static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<Holder>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Holder` does not satisfy the constraint `where T: hashable` (§14.2)
// CHECK-SAME: : choice `Holder` is value-like but case `Fn` payload `callback` is not an admissible equality component (§35.7/§9.2)
