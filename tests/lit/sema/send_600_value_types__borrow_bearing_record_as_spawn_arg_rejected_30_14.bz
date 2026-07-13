// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, borrow_bearing_record_as_spawn_arg_rejected_30_14).
//
// §30.14: borrow-bearing record as a spawn arg is also rejected.
// Domain.Spawn with a borrow-bearing arg type must fail.

module M; class Node end Node; record BorrowArg public Ref: Node?; end BorrowArg; class Worker public static method Run(arg: BorrowArg) end Run; end Worker; class Main public static method Go() var arg := new BorrowArg(); using var t := Domain.Spawn(Worker.Run, arg); end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
