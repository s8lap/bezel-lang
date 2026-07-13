// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_30_3_1175, task_result_return_no_borrow_escape).
//
// §30.3 (#1175 fix): `Task<T>.Result` typed T means `return t.Result` is a
// value-typed return (e.g. int), not a borrow. Pre-fix: returnedExprMayBorrow()
// fell to its conservative default (return true) on the untyped expression and
// emitted a false-positive "borrow returned from `t`..." ownership error. Post-
// fix: Result is typed int -> isBorrowableType(int) = false -> no escape error.
// Uses the ownership checker (§28) to drive returnedExprMayBorrow.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Produce(): Task<int> return 42; end Produce; public static method Get(): int using var t: Task<int> := C.Produce(); t.Wait(); return t.Result; end Get; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
