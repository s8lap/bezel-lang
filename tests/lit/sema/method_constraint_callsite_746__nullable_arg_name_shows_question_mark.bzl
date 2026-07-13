// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, nullable_arg_name_shows_question_mark).
//
// §7.1 / §14.2 (#741 f13): a nullable type argument that violates a `value`
// constraint must be NAMED with its `?` in the diagnostic. The constraint was
// always correctly enforced; only the displayed name was wrong (`int` for the
// `int?` argument), because argNameOf dropped the nullable flag.

module M; class F public static method ValueOnly<T>(x: T): T where T: value return x; end ValueOnly; end F; class C public static method Main(): int var v: int? := 5; var r := F.ValueOnly<int?>(v!); return r!; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `int?` does not satisfy the constraint `where T: value`
// CHECK-SAME: (§14.2)
