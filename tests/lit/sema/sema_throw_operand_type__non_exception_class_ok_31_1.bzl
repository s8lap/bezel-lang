// Ported from tests/sema/throw_operand_type_test.cpp
//   (BEZEL_TEST sema_throw_operand_type, non_exception_class_ok_31_1).
//
// §31.1: a plain non-`Exception` class instance is a valid throw operand — the
// #771 ruling that throw is not restricted to `Exception`-derived types stands
// (v0.182, #1568). ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IShape end IShape; class Circle : IShape end Circle; class Widget end Widget; class AppError : Exception public constructor() base("e"); end constructor; end AppError; record Point(x: int); enum Color Red, Green; end Color; class C public static method Run() throw new Widget(); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
