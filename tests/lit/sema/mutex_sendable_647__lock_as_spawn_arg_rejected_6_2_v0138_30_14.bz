// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mutex_sendable_647_test.cpp
//   (BEZEL_TEST mutex_sendable_647, lock_as_spawn_arg_rejected_6_2_v0138_30_14).
//
// §30.14 / stdlib §6.2 v0.138: Lock passed as spawn argument → rejected.

module M; class Worker public static method Run(owned g: Lock) g.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var mutex := new Mutex(); var guard := await mutex.AcquireAsync(); using var t := Domain.Spawn(Worker.Run, move guard); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
