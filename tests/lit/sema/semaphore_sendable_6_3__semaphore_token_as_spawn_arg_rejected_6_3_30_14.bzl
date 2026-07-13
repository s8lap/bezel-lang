// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, semaphore_token_as_spawn_arg_rejected_6_3_30_14).
//
// §30.14 / stdlib §6.3: SemaphoreToken passed as spawn argument → rejected.

module M; class Worker public static method Run(owned tok: SemaphoreToken) tok.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var sem := new Semaphore(1); var tok := await sem.AcquireAsync(); using var t := Domain.Spawn(Worker.Run, move tok); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
