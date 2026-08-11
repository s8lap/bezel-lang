// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/throw_operand_type_test.cpp
//   (BEZEL_TEST sema_throw_operand_type, object_typed_rejected_31_1).
//
// §31.1: an `Object`-typed operand is rejected — same boxed-referent reason as
// the interface case; narrow to the class first. REJECTED.

module M; interface IShape end IShape; class Circle : IShape end Circle; class Widget end Widget; class AppError : Exception public constructor() base("e"); end constructor; end AppError; record Point(x: int); enum Color Red, Green; end Color; class C public static method Run() var o: Object := new Widget(); throw o; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the thrown value must be an exception — an instance of `Exception` or a class deriving from it
// CHECK-SAME: (§31.1)
