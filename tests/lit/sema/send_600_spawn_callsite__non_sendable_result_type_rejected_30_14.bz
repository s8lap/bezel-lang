// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, non_sendable_result_type_rejected_30_14).
//
// §30.14: non-sendable result type in Spawn (open class result) → rejected.

module M; class OpenResult public Val: int := 0; destructor end destructor; end OpenResult; class Worker public static method Produce(): owned OpenResult return new OpenResult(); end Produce; end Worker; class Main public static async method Go(): Task using var t := Domain.Spawn(Worker.Produce); end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn entry result type is not sendable: the result must satisfy §30.14 sendable-type rules (not borrow-bearing, class types must be transfer-closed/sealed owned)
// CHECK-SAME: (§30.14, #600)
