// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, var_form_binding_slot_rejected_spec_27_4).
//
// A `var`-form binding (`Entry(var k, v)`) is explicitly disallowed even
// though it is irrefutable — §27.4 lists it among the rejected forms
// ("Constants, ranges, relationals, type tests, `var`-form bindings, and
// alternatives are compile errors in parameter position"), unlike §19.3
// case patterns where `var x` and naked `x` are equivalent.

module M; record Entry(K: int, V: int); class C public static method Main(): int var f: (Entry) -> int := lambda(Entry(var k, v)) => k + v; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a destructuring lambda-parameter element must be a binding identifier, `_`, or a nested destructuring — constants, ranges, relationals, type tests, `var`-form bindings, and alternatives are refutable and not admitted in parameter position (a parameter list is not a `case`, §27.4)
