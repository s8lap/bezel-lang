// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/console_write_arity_591_test.cpp
//   (BEZEL_TEST sema_console_arity_591, write_zero_args_diagnoses).
//
// Console.Write with 0 args — rejected (Write always needs exactly 1 arg,
// stdlib §2.1 has no Write() overload).

module M; class C public static method Main() Console.Write(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Console.Write has no overload that accepts 0 argument(s): stdlib §2.1 defines only Write(value) — for composite formatting use string interpolation (`$"{...}"`) instead
