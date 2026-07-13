// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/console_write_arity_591_test.cpp
//   (BEZEL_TEST sema_console_arity_591, error_stream_two_args_diagnoses).

module M; class C public static method Main() Console.Error.WriteLine("err={0}", 1); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Console.WriteLine has no overload that accepts 2 argument(s): stdlib §2.1 defines only WriteLine() and WriteLine(value) — for composite formatting use string interpolation (`$"{...}"`) instead
