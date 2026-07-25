// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #2108 / §27.1.1 (v0.226): distinct BUILTIN function-type heads must
// compare UNEQUAL. `Array<int>` and `Set<int>` are borrowing stdlib collections
// (stdlib §4.4 / §4.10) that resolve to Named TypeRefs with a SYNTHETIC astNode
// but a null `declaration` (no user TypeDecl — §2.5 / #495). The function-value
// slot equality helper (functionSlotTypeRefEqual) compared Named slots by
// `declaration` pointer plus generic args, so ANY two null-declaration builtins
// with equal generic-arg lists compared EQUAL — here `Array<int>` vs `Set<int>`
// (both `<int>`). That made an invariant function-VALUE slot assignability
// mismatch go undetected: a `(Set<int>) -> int` value was silently accepted
// where a `(Array<int>) -> int` value is expected (parameter slot, contravariant
// — §27.1.1 first bullet; the invariant fallback for null-declaration heads is
// §27.1.1 third bullet). After the fix the two distinct builtin heads compare
// unequal and the assignment is rejected as not implicitly convertible.
//
// Mirrors the record-param invariant test
// (function_type_assignability_1544__record_param_invariant_rejected_spec_27_1_1)
// but with two distinct builtin collection heads in place of the two user
// records: the records have distinct declaration pointers and so were already
// distinguished; only the builtin heads exercise the null-declaration arm.

module M; class C public static method TakeSet(s: Set<int>): int return 0; end TakeSet; public static method Main(): int var f: (Set<int>) -> int := TakeSet; var g: (Array<int>) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
