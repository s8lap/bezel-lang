// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, instance_method_ref_entry_rejected_30_14).
//
// §30.14: An instance method reference (receiver is a borrow of origin domain)
// → rejected.

module M; class Worker public method Run() end Run; end Worker; class Main public static async method Go(): Task using var w := new Worker(); using var t := Domain.Spawn(w.Run); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn entry must be capture-free: a non-capturing lambda or a static method reference — a capturing lambda or an instance method reference is rejected because captures and receivers are borrows of the origin domain
// CHECK-SAME: (§30.14, #600)
