// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, nested_refutable_slot_rejected_spec_27_4).
//
// A nested destructuring parameter can ALSO carry a refutable element —
// verify the check recurses.

module M; record Inner(A: int, B: int); record Outer(I: Inner, C: int); class C public static method Main(): int var f: (Outer) -> int := lambda(Outer(Inner(1, b), c)) => b + c; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a destructuring lambda-parameter element must be a binding identifier, `_`, or a nested destructuring — constants, ranges, relationals, type tests, `var`-form bindings, and alternatives are refutable and not admitted in parameter position (a parameter list is not a `case`, §27.4)
