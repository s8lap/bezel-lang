// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, static_method_ref_entry_accepted_30_14).
//
// §30.14: A static method reference as entry with no args → accepted.
// Domain.Spawn(MyClass.StaticEntry) must compile without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Worker public static method Run() end Run; end Worker; class Main public static async method Go(): Task using var t := Domain.Spawn(Worker.Run); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
