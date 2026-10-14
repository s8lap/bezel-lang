// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, override_equals_gethashcode_eq_diagnosed_1249).
//
// §9.2 v0.156 / #1249: a body record that declares `override Equals`
// (with its required companion `override GetHashCode`) WITHOUT `operator =`/`<>`
// is NOT auto-hashable — `=` is a compile error.

module M; record R public property V: int; public constructor(v: int) V := v; end constructor; public override method Equals(other: R): bool => true; public override method GetHashCode(): int => 0; end R; class C public method M() var a := new R(1); var b := new R(9); _ := a = b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `=` is not available on record `R`: it is not auto-hashable
// CHECK-SAME: (§9.2/§9.3)
