// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, non_sendable_class_arg_rejected_30_14).
//
// §30.14: non-sendable arg type (open class) → rejected naming "sendable".

module M; class OpenMsg public Val: int := 0; destructor end destructor; end OpenMsg; class Worker public static method Run(owned m: OpenMsg) end Run; end Worker; class Main public static async method Go(): Task using var msg := new OpenMsg(); using var t := Domain.Spawn(Worker.Run, move msg); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
