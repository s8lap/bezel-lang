// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hashable_constraint_2087_test.cpp
//   (BEZEL_TEST hashable_constraint_2087, method_call_owning_choice_rejected_spec_35_7).
//
// §14.2 (v0.214) / §35.7: an owning choice (an `owned` class payload, §35.4.1)
// is NOT value-like auto-hashable, so it is not hashable (§3.6)
// and fails `where T: hashable`. The §14.2 diagnostic names the constraint, the
// offending type argument, and the structural reason as a component path.

module M; class R public V: int := 0; destructor end destructor; end R; choice FetchOutcome Fetched(owned response: R), NotModified; end FetchOutcome; static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<FetchOutcome>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `FetchOutcome` does not satisfy the constraint `where T: hashable` (§14.2)
// CHECK-SAME: : choice `FetchOutcome` is owning (§35.7), not value-like auto-hashable
