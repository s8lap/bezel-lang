// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/shared_sync_sendable_689_test.cpp
//   (BEZEL_TEST shared_sync_sendable_689, shared_string_as_spawn_arg_rejected_28_13_v0140).
//
// §28.13 v0.140 / §30.14: Shared<string> is NOT sendable → rejected.

module M; class Worker public static method Run(owned h: Shared<string>) h.Dispose(); end Run; end Worker; class Main public static async method Go(): Task using var h := new Shared<string>("hi"); using var t := Domain.Spawn(Worker.Run, h.Clone()); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Shared<T> requires T to be a proper class type: `string` is not a class — runtime-managed and value types have no disposal obligation to share
// CHECK-SAME: (§28.13 / stdlib §2.16, #603)
