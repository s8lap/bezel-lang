// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, semaphore_as_spawn_arg_accepted_6_3_30_14).
//
// §30.14 / stdlib §6.3: Semaphore passed as owned spawn argument → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Worker public static method Run(owned s: Semaphore) s.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var t := Domain.Spawn(Worker.Run, new Semaphore(1)); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
