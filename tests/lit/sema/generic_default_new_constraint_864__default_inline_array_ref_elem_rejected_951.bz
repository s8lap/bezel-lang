// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, default_inline_array_ref_elem_rejected_951).
//
// REJECT: default(inline array of a non-nullable class).

module M; class Cell destructor end destructor; end Cell; class C public static method Main(): int var a: [3]Cell := default([3]Cell); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error for an inline array whose element type is a non-nullable reference type — it has no well-defined zero value; write `default(T?)` for the nullable counterpart where applicable
// CHECK-SAME: (§17.11)
