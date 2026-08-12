// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, ineligible_gethashcode_call_diagnosed_spec_8_3).
//
// §9.2 / §8.3: `.GetHashCode()` on a non-auto-hashable body record is a
// compile error at the call site.

module M; record HasField public property X: int; private _cache: int := 0; public constructor(x: int) X := x; end constructor; end HasField; class C public method M() var a := new HasField(1); _ := a.GetHashCode(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `.GetHashCode` is not available on record `HasField`: it is not auto-hashable
// CHECK-SAME: (§9.2/§9.3)
