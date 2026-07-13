// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/console_write_arity_591_test.cpp
//   (BEZEL_TEST sema_console_arity_591, writeline_two_args_diagnoses).
//
// stdlib §2.1: Console.WriteLine("value={0}", 42) — 2 args, no overload.
// Before #591 this compiled silently and produced NO output at runtime.

module M; class C public static method Main() Console.WriteLine("value={0}", 42); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Console.WriteLine has no overload that accepts 2 argument(s): stdlib §2.1 defines only WriteLine() and WriteLine(value) — for composite formatting use string interpolation (`$"{...}"`) instead
