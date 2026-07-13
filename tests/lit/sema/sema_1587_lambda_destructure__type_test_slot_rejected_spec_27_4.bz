// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, type_test_slot_rejected_spec_27_4).
//
// A bare type-test slot (`Entry(int, v)` — nonsensical, but must be rejected
// with the dedicated diagnostic, not a crash or an unrelated error).

module M; record Entry(K: int, V: int); class C public static method Main(): int var f: (Entry) -> int := (Entry(int, v)) => v; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a destructuring lambda-parameter element must be a binding identifier, `_`, or a nested destructuring — constants, ranges, relationals, type tests, `var`-form bindings, and alternatives are refutable and not admitted in parameter position (a parameter list is not a `case`, §27.4)
