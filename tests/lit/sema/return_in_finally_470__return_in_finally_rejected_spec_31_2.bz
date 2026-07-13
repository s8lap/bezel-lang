// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_in_finally_test.cpp
//   (BEZEL_TEST return_in_finally_470, return_in_finally_rejected_spec_31_2).
//
// §31.2: `return` inside a `finally` block is a compile error.
// Repro from bug #470: the compiler silently accepted this.

module M; class C public static method Foo(): int try var x := 1; finally return 42; end; return 0; end Foo; public static method Main(): int => Foo(); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `return` inside a `finally` block is a compile error
// CHECK-SAME: (§31.2)
