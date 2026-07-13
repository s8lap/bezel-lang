// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST untyped_collection_literal_471, annotated_inline_array_collection_literal_ok_spec_15_4).
//
// §15.4: annotated `var a: [3]int := [1, 2, 3]` — has a target type, OK.
// (Note: the codegen path for Array<int> has a known separate bug #465;
// we test with inline-array target which has a working codegen path.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M() var a: [3]int := [1, 2, 3]; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
