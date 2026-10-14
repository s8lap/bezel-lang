// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, override_equals_only_eq_diagnosed_1249).
//
// §9.2 v0.156 / #1249: a body record with `override Equals` alone (without
// `operator =`/`<>`) is not auto-hashable — `=` is a compile error.
// (The §8.3 eq+hash pairing error is also expected, but the §9.2 `=` reject
// is the load-bearing assertion here.)

module M; record R public property V: int; public constructor(v: int) V := v; end constructor; public override method Equals(other: R): bool => true; end R; class C public method M() var a := new R(1); var b := new R(9); _ := a = b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a type that overrides `Equals` must also override `GetHashCode`
// CHECK-SAME: (§8.3)
