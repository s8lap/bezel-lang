// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, default_record_ref_field_rejected_951).
//
// REJECT: default(record with a non-nullable string field).

module M; record Named(Name: string, Value: int); class C public static method Main(): int var n: Named := default(Named); return n.Value; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error for a record any of whose fields is a non-nullable reference type — it has no well-defined zero value; write `default(T?)` for the nullable counterpart where applicable
// CHECK-SAME: (§17.11)
