// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/using_var_return_762_test.cpp
//   (BEZEL_TEST using_var_return_762, using_var_borrow_return_rejected).
//
// §28.3: `return w` of a `using var` under a borrow return type is a borrow
// that escapes the scope that disposes it — REJECTED.

module M; class Widget destructor end destructor; end Widget; class C public static method BadBorrow(): Widget using var w := new Widget(); return w; end BadBorrow; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot return `using var` `w`; a `using var` is scope-bound — it cannot be moved out of (only borrowed from, and a borrow of it may not escape its scope)
// CHECK-SAME: (§28.3 / §28.10)
