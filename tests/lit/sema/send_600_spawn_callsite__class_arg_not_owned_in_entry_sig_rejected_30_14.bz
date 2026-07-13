// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, class_arg_not_owned_in_entry_sig_rejected_30_14).
//
// §30.14: class-typed arg not declared `owned` in the entry signature →
// rejected. The arg must be owned at the call site.

module M; sealed class Msg public Val: int := 0; destructor end destructor; end Msg; class Worker public static method Run(m: Msg?) end Run; end Worker; class Main public static async method Go(): Task using var msg := new Msg(); using var t := Domain.Spawn(Worker.Run, msg); await t; end Go; end Main;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Domain.Spawn argument is a class type but the entry function does not take it as `owned`: class arguments to Domain.Spawn must be transferred (entry parameter must be `owned`)
// CHECK-SAME: (§30.14, #600)
