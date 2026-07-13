// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_30_3_1175, task_result_type_mismatch_rejected).
//
// §30.3 (#1175 fix): `Task<T>.Result` is typed T in sema. Pre-fix the
// expression was left untyped, which silently accepted a type-mismatch
// initializer (`var s: string := t.Result` where t is Task<int>). Post-fix
// the type is known (int) and the mismatch is diagnosed as a type-conversion
// error (§5.1). Sema-level test verifying the ACCEPT_TRAP is gone.

module M; class C public static async method Produce(): Task<int> return 42; end Produce; public static method Main(): int using var t: Task<int> := C.Produce(); var s: string := t.Result; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `s` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
