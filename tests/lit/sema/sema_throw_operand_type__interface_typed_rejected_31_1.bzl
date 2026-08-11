// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/throw_operand_type_test.cpp
//   (BEZEL_TEST sema_throw_operand_type, interface_typed_rejected_31_1).
//
// §31.1 (v0.243, proposal 0080, #3046 — the sharpened reason): an
// interface-typed operand is rejected — "no interface derives from `Exception`,
// and an `Object` referent may be a boxed value"; narrow to the exception class
// first. REJECTED. (Note the deliberate asymmetry the same revision introduced:
// an interface-typed CATCH ARM stays admissible, §31.2.1 — an `Exception`-
// derived class may IMPLEMENT an interface, so such an arm matches exactly the
// throwables whose class does. See
// tests/lit/sema/sema_catch_arm_type__interface_arm_admissible_31_2_1.bzl.)

module M; interface IShape end IShape; class Circle : IShape end Circle; class Widget end Widget; class AppError : Exception public constructor() base("e"); end constructor; end AppError; record Point(x: int); enum Color Red, Green; end Color; class C public static method Run() var sh: IShape := new Circle(); throw sh; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the thrown value must be an exception — an instance of `Exception` or a class deriving from it
// CHECK-SAME: (§31.1)
