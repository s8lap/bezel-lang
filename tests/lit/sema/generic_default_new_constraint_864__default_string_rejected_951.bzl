// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, default_string_rejected_951).
//
// REJECT: default(string) — string is a non-nullable runtime-managed reference.

module M; class C public static method Main(): int var s: string? := default(string); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error for a non-nullable reference type (a class, interface, `string`, or function type) — it has no well-defined zero value; write `default(T?)` for the nullable counterpart where applicable
// CHECK-SAME: (§17.11)
