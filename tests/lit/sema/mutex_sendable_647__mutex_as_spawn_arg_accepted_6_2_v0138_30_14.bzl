// Ported from tests/sema/mutex_sendable_647_test.cpp
//   (BEZEL_TEST mutex_sendable_647, mutex_as_spawn_arg_accepted_6_2_v0138_30_14).
//
// §30.14 / stdlib §6.2 v0.138: Mutex passed as owned spawn argument → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Worker public static method Run(owned m: Mutex) m.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var t := Domain.Spawn(Worker.Run, new Mutex()); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
