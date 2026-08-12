// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hashable_constraint_2087_test.cpp
//   (BEZEL_TEST hashable_constraint_2087, method_call_inadmissible_positional_record_rejected_spec_9_3).
//
// §14.2 (v0.214) / §9.3: a POSITIONAL record whose fields fall outside the §9.2
// admissible-component rule (a class-typed field) does not synthesize equality
// and is not hashable (§3.6) → fails `where T: hashable`. The message is
// the same record component path as the §9.2 body-record case.

module M; class R public V: int := 0; destructor end destructor; end R; record Holder(item: R); static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<Holder>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Holder` does not satisfy the constraint `where T: hashable` (§14.2)
// CHECK-SAME: : record `Holder` is neither auto-hashable (§9.2) nor declares custom equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode`, §25.2)
