// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST untyped_collection_literal_471, discard_untyped_collection_literal_rejected_spec_15_4).
//
// §15.4: discard `_ := [1, 2, 3]` — no target type, must be a compile error.

module M; class C public static method M() _ := [1, 2, 3]; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal has no determinable target type in discard position — add a type annotation
// CHECK-SAME: (§15.4)
