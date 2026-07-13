// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/console_write_arity_591_test.cpp
//   (BEZEL_TEST sema_console_arity_591, writeline_three_args_diagnoses).
//
// 3 args — also rejected (stdlib §2.1).

module M; class C public static method Main() Console.WriteLine("{0}+{1}={2}", 1, 2, 3); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Console.WriteLine has no overload that accepts 4 argument(s): stdlib §2.1 defines only WriteLine() and WriteLine(value) — for composite formatting use string interpolation (`$"{...}"`) instead
