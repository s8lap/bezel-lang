// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #5906 / §14.2 *Type-parameter bounds* (v0.338): `where T: U` is satisfied by
// subtyping only. `Rel<int, Object>` boxes `int` to `Object`, which is not a
// subtype of `Object` in the bound's sense, so the instantiation is rejected.

module M; class Rel<T, U> where T: U public item: T; end Rel; class C public static method Main(): int var r: Rel<int, Object>? := null; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `int` does not satisfy the constraint `where T: Object`
// CHECK-SAME: (§14.2)
