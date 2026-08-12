// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hashable_constraint_2087_test.cpp
//   (BEZEL_TEST hashable_constraint_2087, method_call_non_auto_eq_body_record_rejected_spec_9_2).
//
// §14.2 (v0.214) / §9.2: a BODY record with a class-typed field is borrow-
// bearing (§9.7) and thus neither auto-hashable nor a declarer of user
// equality → not hashable (§3.6) → fails `where T: hashable`, with the
// component-path reason naming the record.

module M; class R public V: int := 0; destructor end destructor; end R; record BodyBad public Item: R; public constructor(i: R) Item := i; end constructor; end BodyBad; static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<BodyBad>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `BodyBad` does not satisfy the constraint `where T: hashable` (§14.2)
// CHECK-SAME: : record `BodyBad` is neither auto-hashable (§9.2) nor declares custom equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode`, §25.2)
