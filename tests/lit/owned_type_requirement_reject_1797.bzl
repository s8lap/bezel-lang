// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.1 "Type requirement" (v0.193; bare-type-parameter arm revised
// v0.196), issue #1797. In an owned-admitting DECLARATION position — a method
// parameter, a return type, or an `out` parameter — `owned` requires the
// declared type (nullability stripped) to be a class type, an interface type,
// or a bare type parameter. "On any other type — a primitive, `string`, a
// function type, an ordinary record, an enum, a non-owning inline array, or
// the unit type — `owned` is a compile error: such values carry no disposal
// obligation to transfer (§28.9), the same reasoning as §28.5's field
// restriction." Before the fix every method below compiled clean (the marker
// silently reached the ownership pass), so this reject test fails (finds no
// §28.1 diagnostic) without the fix and passes with it.
//
// Each diagnostic is pinned with `[[@LINE-1]]` to the exact declaration line —
// a location-precise contract the coarse conformance manifest cannot express.
// (The `owned` function-type-slot and lambda-parameter positions of §28.1 are
// covered in owned_type_requirement_functype_lambda_reject_1797.bzl.)
module M;
record Pt(X: int, Y: int)
end Pt;
enum Col
  Red, Green, Blue;
end Col;
class R
  // A primitive is a value type — no disposal obligation.
  public method P1(owned x: int): () return; end P1;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type a primitive value type
  // CHECK-SAME: (§28.1)

  // An ordinary record is a value type (§9.7's field restriction is the same
  // reasoning).
  public method P2(owned p: Pt): () return; end P2;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type a record (value type)
  // CHECK-SAME: (§28.1)

  // An enum is a value type.
  public method P3(owned c: Col): () return; end P3;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type an enum (value type)
  // CHECK-SAME: (§28.1)

  // `string` is a runtime-managed reference type — no disposal obligation.
  public method P4(owned s: string): () return; end P4;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type `string` (a runtime-managed reference type)
  // CHECK-SAME: (§28.1)

  // `out owned` on a primitive is rejected the same way.
  public method P5(out owned x: int): () x := 1; return; end P5;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on an `out` parameter of type a primitive value type
  // CHECK-SAME: (§28.1)

  // A `owned` unit-typed return.
  public method P6(): owned () return; end P6;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a return type of type the unit type `()`
  // CHECK-SAME: (§28.1)

  // A non-owning inline array `[N]T` is a value type (only the move-only
  // `[N owned]T` form admits the marker, §15.3).
  public method P7(owned a: [3]int): () return; end P7;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a parameter of type a non-owning inline array (a value type)
  // CHECK-SAME: (§28.1)

  // An `owned` primitive return type.
  public method P8(): owned int return 5; end P8;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a return type of type a primitive value type
  // CHECK-SAME: (§28.1)
end R;
