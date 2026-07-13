// Ported from tests/sema/shared_sync_sendable_689_test.cpp
//   (BEZEL_TEST shared_sync_sendable_689, shared_mutex_as_spawn_arg_accepted_28_13_v0140).
//
// §28.13 v0.140 / §30.14: Shared<Mutex> passed as owned spawn argument → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Worker public static method Run(owned h: Shared<Mutex>) h.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var h := new Shared<Mutex>(new Mutex()); using var t := Domain.Spawn(Worker.Run, h.Clone()); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
