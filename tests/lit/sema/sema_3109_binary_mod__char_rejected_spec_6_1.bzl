// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #3109: §6.1 — binary `c mod 2` rejected on a `char` operand, mirroring the
// `mod=` compound guard (per #3052 / ExprTypecheck.cpp). §3.5 names `mod`
// for "integer types and `decimal`"; §4.5 puts `char` outside that table
// (its integer reading is §5.2's explicit cast). The pre-fix spelling silently
// accepted `c mod 2` while `c mod= 2` rejected — the same operand kind under
// two spellings of one rule disagreed.

module M; class C public method M(a: char, b: int) _ := a mod b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `mod` is not defined on `char` — it is defined on the integer types and `decimal`
// CHECK-SAME: (§6.1)
