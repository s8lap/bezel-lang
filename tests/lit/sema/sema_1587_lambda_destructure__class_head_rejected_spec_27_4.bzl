// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lambda_param_record_destructuring_1587_test.cpp
//   (BEZEL_TEST sema_1587_lambda_destructure, class_head_rejected_spec_27_4).
//
// A class head is rejected — classes have no positional-destructuring
// surface (§19.3, restated by §27.4's "must name a positional record type").

module M; class Widget public Id: int; public constructor(id: int) Id := id; end constructor; end Widget; class C public static method Main(): int var f := lambda(Widget(id)) => id; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: destructuring lambda parameter `Widget(...)` is not valid on a class type — classes do not have a positional-destructuring surface; the head of a destructuring lambda parameter must name a positional record type
// CHECK-SAME: (§27.4)
