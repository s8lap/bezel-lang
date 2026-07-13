// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, static_method_with_sendable_int_arg_accepted_30_14).
//
// §30.14: static method with a sendable value-type arg → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Worker public static method Run(n: int) end Run; end Worker; class Main public static async method Go(): Task using var t := Domain.Spawn(Worker.Run, 42); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
