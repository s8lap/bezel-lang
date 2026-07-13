// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, unqualified_static_method_with_sendable_int_arg_accepted_1492).
//
// fix #1492 / §30.14: an unqualified static method reference with a
// sendable value-type arg → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Main public static method Run(n: int) end Run; public static async method Go(): Task using var t := Domain.Spawn(Run, 42); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
