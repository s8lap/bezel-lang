// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_in_finally_test.cpp
//   (BEZEL_TEST return_in_finally_470, bare_return_in_finally_void_method_rejected_spec_31_2).
//
// §31.2: `return` in `finally` of a void method — also rejected.

module M; class C public static method Foo() try var x := 1; finally return; end; end Foo; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `return` inside a `finally` block is a compile error
// CHECK-SAME: (§31.2)
