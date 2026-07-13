// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/shared_sync_sendable_689_test.cpp
//   (BEZEL_TEST shared_sync_sendable_689, weak_mutex_as_spawn_arg_rejected_28_13_v0140).
//
// §28.13 v0.140: Weak<Mutex> is NOT sendable → rejected.

module M; class Worker public static method Run(owned w: Weak<Mutex>) w.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var h := new Shared<Mutex>(new Mutex()); using var w := h.Downgrade(); using var t := Domain.Spawn(Worker.Run, move w); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
