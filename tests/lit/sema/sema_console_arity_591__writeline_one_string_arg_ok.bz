// Ported from tests/sema/console_write_arity_591_test.cpp
//   (BEZEL_TEST sema_console_arity_591, writeline_one_string_arg_ok).
//
// stdlib §2.1 example: Console.WriteLine(value: string) — 1 arg, OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main() Console.WriteLine("hello"); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
