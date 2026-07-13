// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/throw_operand_type_test.cpp
//   (BEZEL_TEST sema_throw_operand_type, int_primitive_rejected_31_1).
//
// §31.1: a primitive (`int`) operand cannot be thrown. REJECTED.

module M; interface IShape end IShape; class Circle : IShape end Circle; class Widget end Widget; class AppError : Exception public constructor() base("e"); end constructor; end AppError; record Point(x: int); enum Color Red, Green; end Color; class C public static method Run() throw 42; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the thrown value must be an exception — an instance of a class type
// CHECK-SAME: (§31.1)
