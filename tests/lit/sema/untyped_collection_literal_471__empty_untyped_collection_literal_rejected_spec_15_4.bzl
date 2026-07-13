// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST untyped_collection_literal_471, empty_untyped_collection_literal_rejected_spec_15_4).
//
// §15.4: empty literal `[]` without target type — also a compile error.

module M; class C public static method M() var a := []; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal has no determinable target type — add a type annotation (`var a: <CollectionType> := [...]`)
// CHECK-SAME: (§15.4)
