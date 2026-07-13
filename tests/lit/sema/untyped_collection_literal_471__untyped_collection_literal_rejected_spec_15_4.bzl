// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST untyped_collection_literal_471, untyped_collection_literal_rejected_spec_15_4).
//
// §15.4: `var x := [1, 2, 3]` — no target type, must be a compile error.
// Spec example: `var x := [1, 2, 3]; // ERROR: cannot infer target type`

module M; class C public static method M() var a := [1, 2, 3]; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal has no determinable target type — add a type annotation (`var a: <CollectionType> := [...]`)
// CHECK-SAME: (§15.4)
